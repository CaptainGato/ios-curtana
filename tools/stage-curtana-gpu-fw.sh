#!/bin/bash
# Stage Adreno 618 (SM7125 curtana / miatoll) firmware into a mounted iOS rootfs (/firm & /fw).
#
# Usage:
#   sudo tools/stage-curtana-gpu-fw.sh <MOUNTED_IOS_ROOT> [VENDOR_FIRMWARE_DIR] [--fetch-open-fw]
#
# Required files in /firm/ on the rootfs for sweetgpu=9..22 and gpu=1:
#   - a618_gmu.bin   (32,768 bytes flat Cortex-M3 ITCM+DTCM image)
#   - a630_sqe.fw    (CP SQE microcode for Adreno 6xx)
#   - a615_zap.mdt   (Signed TrustZone PIL ELF header for PAS ID 18, ~6,860 bytes)
#   - a615_zap.b00   (ELF program headers segment)
#   - a615_zap.b01   (Hash table segment)
#   - a615_zap.b02   (Zap shader microcode payload, loaded at physical 0x93b15000)

set -euo pipefail

if [ $# -lt 1 ] || [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
	echo "Usage: sudo $0 <MOUNTED_IOS_ROOT> [VENDOR_FIRMWARE_DIR] [--fetch-open-fw]" >&2
	[ $# -lt 1 ] && exit 1 || exit 0
fi

ROOT="$1"
FW_SRC="${2:-}"
FETCH_OPEN=0
for arg in "$@"; do
	if [ "$arg" = "--fetch-open-fw" ]; then
		FETCH_OPEN=1
	fi
done

mkdir -p "$ROOT/firm" "$ROOT/fw"

if [ "$FETCH_OPEN" = 1 ]; then
	echo "[*] Fetching upstream linux-firmware a618_gmu.bin and a630_sqe.fw..."
	LFW_BASE="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/qcom"
	curl -sL --fail -o "$ROOT/firm/a618_gmu.bin" "$LFW_BASE/a618_gmu.bin"
	curl -sL --fail -o "$ROOT/firm/a630_sqe.fw" "$LFW_BASE/a630_sqe.fw"
fi

if [ -n "$FW_SRC" ] && [ "$FW_SRC" != "--fetch-open-fw" ] && [ -d "$FW_SRC" ]; then
	echo "[*] Copying Adreno 618 firmware from $FW_SRC -> $ROOT/firm/ ..."
	for f in a618_gmu.bin a630_sqe.fw a615_zap.mdt a615_zap.b00 a615_zap.b01 a615_zap.b02 a615_zap.elf; do
		if [ -f "$FW_SRC/$f" ]; then
			cp -v "$FW_SRC/$f" "$ROOT/firm/$f"
		fi
	done
fi

# Mirror /firm/* into /fw/* so both paths work identically
for f in "$ROOT/firm"/*; do
	[ -f "$f" ] || continue
	chmod 0644 "$f"
	cp -f "$f" "$ROOT/fw/$(basename "$f")"
done

echo ""
echo "=== Verifying Staged Adreno 618 Firmware in $ROOT/firm ==="
for req in a618_gmu.bin a630_sqe.fw a615_zap.mdt a615_zap.b02; do
	if [ -f "$ROOT/firm/$req" ]; then
		sz=$(stat -c %s "$ROOT/firm/$req" 2>/dev/null || stat -f %z "$ROOT/firm/$req")
		echo "  [OK] $req ($sz bytes)"
		if [ "$req" = "a618_gmu.bin" ] && [ "$sz" != "32768" ]; then
			echo "  [!] WARNING: a618_gmu.bin is $sz bytes (expected 32768 bytes for legacy A618 GMU)"
		fi
	else
		echo "  [MISSING] $req (pull from Curtana Android /vendor/firmware/$req)"
	fi
done
