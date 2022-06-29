#!/usr/bin/env bash

PWD="$(pwd)"
GIT="$(which git)"
MAKE="$(which gmake)"

XDPTOOLS_PATH="${PWD}/xdp-tools"
XDPTOOLS_CONFIGURE="${XDPTOOLS_PATH}/configure"
XDPTOOLS_CONFIG_MK="config.mk"

function fatal()
{
	echo "FATAL ERROR: $@"
	exit 1
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

while (( "$#" )); do
	case "${1}" in
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

git_submodule_prep
build_xdptools
