#!/bin/bash
touch alpine-minirootfs/dev/urandom
mount --bind /dev/urandom alpine-minirootfs/dev/urandom
chroot alpine-minirootfs /bin/ash
