#!/bin/bash
# Build build/pmos/rootfs.tar for nvme0n1p2: postmarketOS phosh from
# pmbootstrap, our Mesa from "make rootfs", the phone firmware from the
# ramdisk cpios, and tools/pmos/overlay. Also disktools.tar, the mkfs tools
# install.sh runs from the ramdisk. Needs sudo to read the root-owned chroot.
set -e
cd "$(dirname "$0")/../.."
P=build/pmos
C=$P/work/chroot_rootfs_qemu-aarch64
R=build/alpine/rootfs
O=$P/overlay
U=https://dl-cdn.alpinelinux.org/alpine/v3.24
CHANNEL=v26.06

mkdir -p $P
[ -d $P/pmaports ] || {
	git clone -q --depth 1 -b $CHANNEL https://gitlab.postmarketos.org/postmarketOS/pmaports.git $P/pmaports
	# pmbootstrap wants origin/main to exist
	git -C $P/pmaports fetch -q --depth 1 origin main:refs/remotes/origin/main
}
# qemu-aarch64 stands in for a device package; our kernel boots it
[ -f $P/pmbootstrap.cfg ] || cat > $P/pmbootstrap.cfg <<CFG
[pmbootstrap]
aports = $PWD/$P/pmaports
device = qemu-aarch64
extra_packages = e2fsprogs,openssh-server
hostname = xsmax
is_default_channel = False
jobs = $(nproc)
kernel = lts
service_manager = openrc
ui = phosh
work = $PWD/$P/work
CFG
[ -d $P/work ] || yes "" | pmbootstrap -c $P/pmbootstrap.cfg init >/dev/null
pmbootstrap -c $P/pmbootstrap.cfg -y install --no-image --password 147147
pmbootstrap -c $P/pmbootstrap.cfg shutdown

rm -rf $O
mkdir -p $O/usr/lib $O/usr/share $O/etc/OpenCL $O/etc/phosh $O/etc/apk/keys
cp -a tools/pmos/overlay/. $O/
cp -a $R/usr/lib/dri $R/usr/lib/gbm $R/usr/lib/libgallium-*.so $R/usr/lib/libvulkan_asahi.so \
    $R/usr/lib/lib{EGL,gbm,GL,GLESv1_CM,GLESv2,RusticlOpenCL}.so* $O/usr/lib/
cp -a $R/usr/share/vulkan $R/usr/share/drirc.d $O/usr/share/
cp -a $R/etc/OpenCL/vendors $O/etc/OpenCL/
{ cat $C/usr/share/phosh/phoc.ini; printf '\n[output:DSI-1]\nscale = 3\n'; } > $O/etc/phosh/phoc.ini
# pmbootstrap leaves etc/apk/keys empty, and apk then refuses every package
cp $C/usr/share/apk/keys/*.pub $C/usr/share/apk/keys/aarch64/*.pub $O/etc/apk/keys/
# compile our gschema override in with the chroot's schemas
S=$(mktemp -d)
cp $C/usr/share/glib-2.0/schemas/* $O/usr/share/glib-2.0/schemas/* $S/
glib-compile-schemas --targetdir=$O/usr/share/glib-2.0/schemas $S
rm -rf $S
# into usr/, since /lib is a link to it
for c in build/iboot/mtfw.cpio build/iboot/bcmfw.cpio; do (cd $O/usr && cpio -idm --quiet) < $c; done
# zram is built in, but without these modprobe fails and zram swap never starts
K=$(cat build/linux/include/config/kernel.release)
mkdir -p $O/usr/lib/modules/$K
cp build/linux/modules.builtin build/linux/modules.builtin.modinfo $O/usr/lib/modules/$K/
touch $O/usr/lib/modules/$K/modules.order
depmod -b $O/usr $K

# pmOS's own libgallium stays out, or apps that load it first crash ours
sudo tar --numeric-owner --format=gnu --exclude='./usr/lib/libgallium-*' -C $C -cf $P/rootfs.tar .
sudo chown "$(id -u):$(id -g)" $P/rootfs.tar
tar --numeric-owner --owner=0 --group=0 --format=gnu -C $O -rf $P/rootfs.tar .

rm -rf $P/disktools
build/alpine/sbin/apk.static --arch aarch64 --usermode --allow-untrusted --no-scripts \
    -X $U/main -X $U/community --root $PWD/$P/disktools --initdb --update-cache \
    add busybox sfdisk e2fsprogs dosfstools >/dev/null
tar -C $P/disktools -cf $P/disktools.tar .
