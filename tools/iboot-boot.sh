#!/bin/bash
# Boot the blob through Apple's own recovery chain.
#
# iBoot is asked to restore a device, and m1n1 is handed to it where the
# restore kernelcache would go. That keeps iBSS in the picture, so DRAM, the
# ADT and the display are all already up, and m1n1 gets a real BootArgs.
#
# Order and component types come from HoolockLinux's remoteboot.sh. The
# manifests are not theirs: there is no generic IM4M for an A12, because from
# A11 on the manifest is tied to the device's own ECID. Any IM4M gets the
# parser far enough once tools/iboot-image4-patch.py has stopped iBoot from
# checking what is inside one.
#
# The phone has to be sitting in pwned DFU on the jig before this runs.
# Sending anything to it while it is still in DFU corrupts the exploit's
# request handler, so each step checks the mode it expects first.
set -u
cd "$(dirname "$0")/.."

IBSS=build/iboot/iBSS.patched.raw
RDTR=build/iboot/devicetree.img4
RTSC=build/iboot/trustcache.img4
KRNL=${1:-build/iboot/kernelcache.img4}

mode() { irecovery -q 2>/dev/null | sed -n 's/^MODE: //p'; }
pwnd() { irecovery -q 2>/dev/null | sed -n 's/^PWND: //p'; }

wait_mode() {
	local want=$1 n=${2:-20} i
	for ((i = 0; i < n; i++)); do
		[ "$(mode)" = "$want" ] && return 0
		sleep 1
	done
	return 1
}

send() {
	echo "  send $1"
	timeout 300 irecovery -f "$1" >/dev/null 2>&1 || {
		echo "  upload failed: $1"
		exit 1
	}
}

# iBoot answers a bad component by logging and carrying on, and a component it
# accepts but cannot use by falling over, so the mode afterwards is the only
# reliable signal that something went wrong before bootx.
cmd() {
	echo "  cmd $1"
	timeout 30 irecovery -c "$1" >/dev/null 2>&1
	sleep 1
	local m
	m=$(mode)
	if [ -z "$m" ] && [ "$1" != bootx ]; then
		echo "  device fell off USB on '$1'"
		exit 1
	fi
	echo "    mode: '${m:-gone}'"
}

for f in "$IBSS" "$RDTR" "$RTSC" "$KRNL"; do
	[ -f "$f" ] || {
		echo "missing $f, see README.md"
		exit 1
	}
done

if ! wait_mode DFU 5; then
	echo "not in DFU, mode is '$(mode)'. Put it back on the jig."
	exit 1
fi
if [ -z "$(pwnd)" ]; then
	echo "in DFU but not pwned, the jig has not run the exploit."
	exit 1
fi

echo "pwned DFU, booting $IBSS"
timeout 90 build/venv/bin/python tools/dfuboot.py "$IBSS" >/dev/null 2>&1

wait_mode Recovery 20 || {
	echo "iBSS did not come up, mode is '$(mode)'"
	exit 1
}
echo "iBSS up in recovery"

send "$RDTR"
cmd devicetree
send "$RTSC"
cmd firmware
send "$KRNL"
cmd bootx
