#!/bin/bash
cat > alpine-minirootfs/etc/resolv.conf << EOF
nameserver 8.8.8.8
nameserver 8.8.4.4
EOF
cat > alpine-minirootfs/mk.sh << EOF
apk update
apk upgrade
apk add openrc nano mc htop tcpdump parted wpa_supplicant dropbear dropbear-ssh efibootmgr busybox-initscripts
echo "exit from here typing exit"
exit
EOF
chmod +x alpine-minirootfs/mk.sh
echo "run ./mk.sh"

chroot alpine-minirootfs /bin/ash
rm alpine-minirootfs/mk.sh