#!/bin/sh
# Script for using udhcpc (started by ifup) with wpa_supplicant.
#
# Distributed under the same license as wpa_supplicant itself.
# Copyright (c) 2015,2018 Sören Tempel <soeren+alpine@soeren-tempel.net>

if [ $# -ne 2 ]; then
	logger -t wpa_cli "this script should be called from wpa_cli(8)"
	exit 1
fi

IFNAME="${1}"
ACTION="${2}"
SIGNAL=""
DHCPID=""

# PID file created by the busybox ifup applet for udhcpc.
DHCPIDFILE="/var/run/udhcpc.${IFNAME}.pid"

if [ ! -e "${DHCPIDFILE}" ]; then
	logger -t wpa_cli "udhcpc isn't running for interface '${IFNAME}'"
	exit 1
fi

logger -t wpa_cli "interface ${IFNAME} ${ACTION}"
case "${ACTION}" in
	CONNECTED)
		SIGNAL="USR1"
		;;
	DISCONNECTED)
		SIGNAL="USR2"
		;;
	*)
		logger -t wpa_cli "unknown action '${ACTION}'"
		exit 1
esac

read -r DHCPID < "${DHCPIDFILE}"
kill -${SIGNAL} "${DHCPID}" || logger -t wpa_cli \
	"Couldn't send '${SIGNAL}' to '${DHCPID}'"
