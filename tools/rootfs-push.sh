#!/bin/sh
# Copy build/alpine/rootfs.tar to the phone over the USB link into a tmpfs at
# /mnt/a, with /dev, /proc and /sys mounted, ready for chroot. The PC serves
# the tarball over HTTP on its end of the link; nothing needs root here.
set -e
cd "$(dirname "$0")/.."
TSH="build/venv/bin/python tools/tsh.py"

python3 -m http.server --bind 172.16.42.2 8000 --directory build/alpine >/dev/null 2>&1 &
trap 'kill $!' EXIT
sleep 1

$TSH '(rm -f /tmp/push.txt; umount -R /mnt/a 2>/dev/null; mkdir -p /mnt/a && mount -t tmpfs -o size=1536m tmpfs /mnt/a &&
	wget -q -O - http://172.16.42.2:8000/rootfs.tar | tar x -C /mnt/a &&
	mount --bind /dev /mnt/a/dev && mount -t proc proc /mnt/a/proc &&
	mount -t sysfs sys /mnt/a/sys && chroot /mnt/a /bin/busybox --install -s && echo pushed > /tmp/push.txt || echo failed > /tmp/push.txt) &' >/dev/null
for i in $(seq 120); do
	sleep 2
	r=$($TSH 'cat /tmp/push.txt 2>/dev/null' | tr -d '\r' | grep -E '^(pushed|failed)$' || true)
	[ -n "$r" ] && break
done
echo "${r:-timed out}"
[ "$r" = pushed ]
