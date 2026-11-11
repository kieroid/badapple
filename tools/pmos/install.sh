#!/bin/sh
# Runs on the phone from the plain ramdisk ("make boot"), with the repo
# served on the host:
#   python3 -m http.server 8003 --bind 172.16.42.2
#   tools/tsh.py "wget -qO- http://172.16.42.2:8003/tools/pmos/install.sh | sh"
# Puts a fresh ext4 with rootfs.tar on nvme0n1p2, and boot.bin on p1. Partitions the disk first
# if p2 is missing: GPT, p1 512MB FAT32 for boot.bin, p2 the rest.
set -e
H=http://172.16.42.2:8003/build/pmos
D=/tmp/dt
mkdir -p $D && wget -q -O - $H/disktools.tar | tar x -C $D
mkdir -p $D/dev $D/proc $D/sys
mount --bind /dev $D/dev && mount -t proc proc $D/proc && mount -t sysfs sys $D/sys
if [ ! -e /dev/nvme0n1p2 ]; then
	printf 'label: gpt\nsize=512MiB, type=uefi, name=boot\ntype=linux, name=pmos\n' |
		chroot $D sfdisk -q --wipe always /dev/nvme0n1
	sleep 2
	chroot $D mkfs.vfat -F 32 -n BOOT /dev/nvme0n1p1
fi
# the Makefile's BOOT_PART, which the chainload kernelcache looks for
chroot $D sfdisk -q --part-uuid /dev/nvme0n1 1 65f10032-02c6-4a7b-8623-373969fc66c4
chroot $D mkfs.ext4 -q -F -L pmOS /dev/nvme0n1p2
mkdir -p /mnt/r && mount /dev/nvme0n1p2 /mnt/r
wget -q -O - $H/rootfs.tar | tar x -C /mnt/r
umount /mnt/r
mkdir -p /mnt/b && mount /dev/nvme0n1p1 /mnt/b
wget -q -O /mnt/b/boot.bin ${H%/pmos}/iboot/boot.bin
umount /mnt/b
sync
echo DONE
