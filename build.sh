#!/usr/bin/env bash

PWD="$(pwd)"
GIT="$(which git)"
MAKE="$(which gmake)"
GCC="$(which gcc)"

PREPARE=

XDPTOOLS_PATH="${PWD}/xdp-tools"
XDPTOOLS_CONFIGURE="${XDPTOOLS_PATH}/configure"
XDPTOOLS_CONFIG_MK="config.mk"

function features_needed()
{
	echo "We need the following packages to build on Red Hat:"
	echo "  libcap libcap-devel"
	echo "	libz libz-devel"
	echo "	libelf libelf-devel"
}

function fatal()
{
	echo "FATAL ERROR: $@"
	exit 1
}

# Check we have all the headers we need
function check_headers()
{
	echo "Checking headers..."

	local usr_includes="sys/capability.h pthread.h"
	for f in ${usr_includes}; do
		local fatal=0
		f="/usr/include/${f}"

		echo " Checking: ${f}"
		if [ ! -f "${f}" ]; then
			features_needed
			fatal "Cannot find: ${f}"
		fi
	done

	echo "Got all headers."
}

# Prepare git submodules and validate they exist
function git_submodule_prep()
{
	${GIT} submodule init
	${GIT} submodule update --remote --force --recursive
	[ -d "${XDPTOOLS_PATH}" ] || fatal "Can't find XDPTOOLS_PATH: ${XDPTOOLS_PATH}"
	[ -f "${XDPTOOLS_CONFIGURE}" ] || fatal "Can't find XDPTOOLS_CONIFGURE: ${XDPTOOLS_CONFIGURE}"

	# there's probably a saner way to do this, but also init xdp-tools submodules
	${GIT} -C "${XDPTOOLS_PATH}" init
	${GIT} -C "${XDPTOOLS_PATH}" update --remote --force --recursive
}

# Build xpdtools
function build_xdptools()
{
	# run xdptools configure and validate it dropped a config.mk in our pwd
	${XDPTOOLS_CONFIGURE} PRODUCTION=1 DYNAMIC_LIBXDP=0 MAX_DISPATCHER_ACTIONS=10
	[ -f "${XDPTOOLS_CONFIG_MK}" ] || fatal "xdptools configure didn't generate ${XDPTOOLS_CONFIG_MK}"

	# move it into xdptools
	mv "${XDPTOOLS_CONFIG_MK}" "${XDPTOOLS_PATH}"

	# try compile
	${MAKE} -C ${XDPTOOLS_PATH} V=0
}

# Build xdpsock user-space application
function build_xdpsock_app()
{
	local usrsrc="xdpsock_user.c"
	local usrobj="xdpsock_user"
	local libbpf_static="${XDPTOOLS_PATH}/lib/libbpf/src/libbpf.a"
	local libbpf_headers="${XDPTOOLS_PATH}/lib/libbpf/src/root/usr/include"
	local libxdp_static="${XDPTOOLS_PATH}/lib/libxdp/libxdp.a"
	local libxdp_headers="${XDPTOOLS_PATH}/lib/libxdp"

	# Build xdpsock user app
	local build_cmd="-I. -I${libxdp_headers} -I${libbpf_headers} -o ${usrobj} ${usrsrc}"
	build_cmd="${build_cmd} ${libxdp_static} ${libbpf_static} -lcap -pthread -lelf -lz"

	rm -f ${usrobj}
	[ ! -f "${usrobj}" ] || fatal "Can't delete stale ${usrobj}"

	echo ""
	echo "Running: ${GCC} ${build_cmd}"
	echo ""
	${GCC} ${build_cmd}
	[ -f "${usrobj}" ] || fatal "Object not compiled: ${usrobj}"

	echo "Compiled successfully: ${usrobj}"
	echo ""
}

# Build xdpsock bpf object
function build_xdpsock_bpf()
{
	local bpfsrc="xdpsock_kern.c"
	local bpfobj="xdpsock_kern.bpf"
}

while (( "$#" )); do
	case "${1}" in
		--prepare)
			PREPARE=1
			shift
			;;
		--check)
			check_headers
			exit 0
			;;
		--cleanup)
			${MAKE} -C "${XDPTOOLS_PATH}" distclean
			exit 0
			;;
		--clean)
			${MAKE} -C "${XDPTOOLS_PATH}" distclean
			shift
			;;
		*)
			fatal "Unknown arg: ${1}"
			;;
	esac
done

#
# Default action below here
#

check_headers
[ -z ${PREPARE} ] || git_submodule_prep
[ -z ${PREPARE} ] || build_xdptools
build_xdpsock_app
build_xdpsock_bpf
