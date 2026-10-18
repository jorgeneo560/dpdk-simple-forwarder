#!/bin/sh
# Prepares the machine for DPDK (hugepages, driver binding) and runs flowfwd as root.
# Usage: ./run.sh [EAL options]    default: -l 0-8 (4 queues need 8 worker lcores plus the main one)
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
BIN="$ROOT/build/flowfwd"
NICS="0000:09:00.0 0000:09:00.1"	# the two X540 ports
DRIVER=uio_pci_generic
HUGEPAGES=2G

if [ ! -x "$BIN" ]; then
	echo "$BIN not found, run ./setup.sh first" >&2
	exit 1
fi

echo "[1/3] hugepages"
if [ "$(awk '/HugePages_Free/ {print $2}' /proc/meminfo)" -eq 0 ]; then
	sudo dpdk-hugepages.py -p 2M --setup "$HUGEPAGES"
else
	echo "  already allocated"
fi

echo "[2/3] driver $DRIVER"
sudo modprobe "$DRIVER"

echo "[3/3] binding the NICs"
for nic in $NICS; do
	current=$(basename "$(readlink "/sys/bus/pci/devices/$nic/driver" 2>/dev/null || echo none)")
	if [ "$current" = "$DRIVER" ]; then
		echo "  $nic already bound"
	else
		echo "  $nic: $current -> $DRIVER"
		sudo dpdk-devbind.py --force -b "$DRIVER" "$nic"
	fi
done

if [ "$#" -eq 0 ]; then
	set -- -l 0-8
fi
echo "starting flowfwd $*"
cd "$(dirname "$BIN")"
exec sudo "$BIN" "$@"
