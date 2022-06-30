#!/usr/bin/env bash

PWD="$(pwd)"
GIT="$(which git)"
MAKE="$(which gmake)"
GCC="$(which gcc)"
CLANG="$(which clang)"
LLC="$(which llc)"
TAR="$(which tar)"

XDPTOOLS_PATH="${PWD}/xdp-tools"
XDPTOOLS_CONFIGURE="${XDPTOOLS_PATH}/configure"
XDPTOOLS_CONFIG_MK="config.mk"

BUILDDEPS=1
MAX_SOCKS=2
MULTI_FCQ=
TARGZ=
XDPSOCK_KRNL="xdpsock_kern.bpf"

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
	local build_cmd="${GCC} -I. -I${libxdp_headers} -I${libbpf_headers}"
	build_cmd="${build_cmd} -Wall -g -O2 -DMAX_SOCKS=${MAX_SOCKS} ${MULTI_FCQ} -DXDPSOCK_KRNL=\"${XDPSOCK_KRNL}\""
	build_cmd="${build_cmd} -o ${usrobj} ${usrsrc}"
	build_cmd="${build_cmd} ${libxdp_static} ${libbpf_static} -lcap -pthread -lelf -lz"

	rm -f ${usrobj}
	[ ! -f "${usrobj}" ] || fatal "Can't delete stale ${usrobj}"

	echo ""
	echo "Running: ${build_cmd}"
	echo ""
	${build_cmd}
	[ -f "${usrobj}" ] || fatal "Object not compiled: ${usrobj}"

	echo "Compiled successfully: ${usrobj}"
	echo ""
}

# Build xdpsock bpf object
function build_xdpsock_bpf()
{
	local bpfsrc="xdpsock_kern.c"
	local bpfint="xdpsock_kern.ll"
	local bpfobj="xdpsock_kern.bpf"
	local libbpf_headers="${XDPTOOLS_PATH}/lib/libbpf/src/root/usr/include"

	# Build xdpsock kernel app via clang
	local build1_cmd="${CLANG} -I. -I${libbpf_headers} -D__KERNEL__ -D__BPF_TRACING__"
	build1_cmd="${build1_cmd} -DMAX_SOCKS=${MAX_SOCKS} -Wall -g -O2"
	build1_cmd="${build1_cmd} -target bpf -S -emit-llvm ${bpfsrc} -o ${bpfint}"
	local build2_cmd="${LLC} -march=bpf -filetype=obj ${bpfint} -o ${bpfobj}"

	rm -f ${bpfint} ${bpfobj}
	[ ! -f "${bpfint}" ] || fatal "Can't delete stale intermediate file ${bpfint}"
	[ ! -f "${bpfobj}" ] || fatal "Can't delete stale object file ${bpfobj}"

	echo ""
	echo "Running: ${build1_cmd}"
	echo ""
	${build1_cmd}
	[ -f "${bpfint}" ] || fatal "Intermediate not compiled: ${bpfint}"

	echo ""
	echo "Running: ${build2_cmd}"
	echo ""
	${build2_cmd}
	[ -f "${bpfobj}" ] || fatal "BPF object not compiled: ${bpfobj}"

	echo "Compiled successfully: ${bpfobj}"
	echo ""
}

# Tar up binary files
function tar_xdpsock()
{
	local files="xdpsock_user ${XDPSOCK_KRNL}"
	local tar="xdpsock.tar.gz"

	rm -f "${tar}"
	[ ! -f "${tar}" ] || fatal "Can't delete stale tar: ${tar}"

	echo ""
	echo "Creating ${tar} from: ${files}"
	echo ""
	${TAR} -zcvf ${tar} ${files}
	[ -f "${tar}" ] || fatal "Tar file not created: ${tar}"

	echo ""
	echo "Tar created: ${tar}"
	echo ""
}

while (( "$#" )); do
	case "${1}" in
		--nodeps)
			BUILDDEPS=
			shift
			;;
		--check)
			check_headers
			exit 0
			;;
		--cleanup)
			${MAKE} -C "${XDPTOOLS_PATH}" distclean
			rm -f xdpsock_user.o xdpsock_kern.ll xdpsock_kern.bpf
			exit 0
			;;
		--clean)
			${MAKE} -C "${XDPTOOLS_PATH}" distclean
			rm -f xdpsock_user.o xdpsock_kern.ll xdpsock_kern.bpf
			shift
			;;
		--max-xsk)
			[ $# -ge 2 ] || fatal "Insufficient args: --max-xsk <num>"
			MAX_SOCKS="${2}"
			shift
			shift
			;;
		--multi-fcq)
			MULTI_FCQ="-DMULTI_FCQ=1"
			shift
			;;
		--bpf-object)
			[ $# -ge 2 ] || fatal "Insufficient args: --bpf-object <object_filename>"
			XDPSOCK_KRNL="${2}"
			shift
			shift
			;;
		--tar)
			TARGZ=1
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
[ -z "${BUILDDEPS}" ] || git_submodule_prep
[ -z "${BUILDDEPS}" ] || build_xdptools
build_xdpsock_app
build_xdpsock_bpf
[ -z "${TARGZ}" ] || tar_xdpsock