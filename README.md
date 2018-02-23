## One File Linux
Live linux distro combined in one EFI file.  


## Install on Mac

### 1. Mount EFI System Partition 
```mkdir /tmp/efi
sudo mount -t msdos /tmp/efi /dev/diskN```
To find out EFI partition disk number use *diskutil list*

### 2. Copy OneFileLinux.EFI to EFI Partiotion
`cp ~/Downloads/OneFileLinux.efi /tmp/efi/`


### 3. Set NVRAM to boot linux once
`sudo bless --mount /tmp/efi --setBoot --nextonly --file /tmp/OneFileLinux.efi`

This command will boot linux only once. Next reboot will return previous boot sequence.

**!!!** Note that  System Integrity Protection (SIP) prohibits to change boot options.  
You can run `bless` from Recover Mode console. Press CMD+R while power on and go to "Utilities —> Terminal"

## Install on PC

If your motherboard has UEFI Shell, just choose the path to OneFileLinux.efi on ESP.  
Otherwise add new boot options to NVRAM and choose it from boot menu.  

Example for ThinkPad x220 

### 1. Copy OneFileLinux.efi to EFI Partition

### 2. Add NVRAM entry
`efibootmgr --disk /dev/sda --part 2 --create --label "One File Linux" --loader /OneFileLinux.efi`

### 3. Choose new entry from boot menu
Press F12 while power on and choose new boot entry  

![Thinkpad x220 boot menu](https://habrastorage.org/webt/wv/6f/tm/wv6ftmykf6wncgtkzx7chiiz-cm.png)


## Building
This project is based on vanilla linux kernel `4.16-rc1`  
and Alpine Linux Minimal root filesystem https://alpinelinux.org/downloads/

### Download kernel

`https://git.kernel.org/torvalds/t/linux-4.16-rc1.tar.gz` and extract it to `linux-4.16-rc1`  
This repository contatins `linux-4.16-rc1/.config` file with kernel config.   

### Edit root filesystem

Chroot into root filesystem  
`chroot ./alpine-minirootfs /bin/as`

Edit what you need. Install packages with `apk` packet manager.

### Create cpio file

`find ./alpine-minirootfs | cpio -H newc -o > ./alpineramfs.cpio`

### Build kernel

Edit  path to `alpineramfs.cpio` in kernel config  

```
$ make menuconfig

General Setup --->
   Initramfs source file
```

Build kernel  

`make -j4`
