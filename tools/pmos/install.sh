#!/bin/sh
# Runs on the phone from the plain ramdisk ("make boot"), with the repo
# served on the host:
#   python3 -m http.server 8003 --bind 172.16.42.2
#   tools/tsh.py "wget -qO- http://172.16.42.2:8003/tools/pmos/install.sh | sh"
# Puts a fresh ext4 with rootfs.tar on nvme0n1p2. Partitions the disk first
# if p2 is missing: GPT, p1 512MB FAT32 for boot files, p2 the rest.
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
chroot $D mkfs.ext4 -q -F -L pmOS /dev/nvme0n1p2
mkdir -p /mnt/r && mount /dev/nvme0n1p2 /mnt/r
wget -q -O - $H/rootfs.tar | tar x -C /mnt/r
sync
umount /mnt/r
echo DONE
