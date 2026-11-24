#!/bin/sh
# leo_build_fw.sh - builds the ak_rtsp firmware image ON THE CAMERA from the camera's own
# original APP partition backup (no vendor code is distributed with the installer).
#   $1 = original APP image (mtdblock7.bin, 5242880 B, squashfs xz)
#   $2 = output image path (5242880 B, squashfs gzip, padded with 0xFF)
# Uses leo/bin/unsquashfs + mksquashfs (squashfs-tools 4.6.1, GPL-2.0, built static for ARMv5)
# and leo/payload/ (ak_rtsp, wrapper, ini, flash_tool, sounds).   (c) 2026 leotech.cz, MIT
L="${L:-/tmp/sd/leo}"
RUN="${RUN:-}"                       # test hook: RUN="qemu-arm" on a PC
SRC="$1"; OUT="$2"
W="${LEO_BUILD_DIR:-/tmp/leo_build}"
TARGET=5242880
log() { echo "[build] $*"; }

[ -f "$SRC" ] || { log "missing $SRC"; exit 1; }
for f in unsquashfs mksquashfs; do chmod +x "$L/bin/$f" 2>/dev/null; done

if [ -z "$RUN" ]; then
    AVAIL=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo 2>/dev/null)
    [ -n "$AVAIL" ] || AVAIL=$(awk '/^MemFree:/{f=$2} /^Cached:/{c=$2} END{print f+c}' /proc/meminfo)
    log "MemAvailable ${AVAIL} kB"
    if [ "${AVAIL:-0}" -lt 18000 ]; then log "not enough RAM for the build"; exit 4; fi
fi

rm -rf "$W"; mkdir -p "$W" || exit 1
log "unpacking original APP (without the Tuya app anyka_ipc)"
$RUN "$L/bin/unsquashfs" -n -excludes -d "$W/app" "$SRC" bin/anyka_ipc >"$W/unsq.log" 2>&1 || { cat "$W/unsq.log"; log "unsquashfs failed"; rm -rf "$W"; exit 2; }
[ -d "$W/app/bin" ] && [ -f "$W/app/local/_ht_process.ini" ] || { log "unexpected APP layout"; rm -rf "$W"; exit 2; }
rm -f "$W/app/bin/anyka_ipc"

log "adding ak_rtsp"
P="$L/payload"
cp "$P/ak_rtsp" "$W/app/bin/ak_rtsp" && chmod 755 "$W/app/bin/ak_rtsp" || exit 2
cp "$P/flash_tool" "$W/app/bin/flash_tool" && chmod 755 "$W/app/bin/flash_tool"
cp "$P/ak_rtsp_wrapper.sh" "$W/app/sbin/ak_rtsp_wrapper.sh" && chmod 755 "$W/app/sbin/ak_rtsp_wrapper.sh"
cp "$P/_ht_process.ini" "$W/app/local/_ht_process.ini"
if [ -d "$P/leo_sounds" ]; then rm -rf "$W/app/share/leo_sounds"; cp -r "$P/leo_sounds" "$W/app/share/leo_sounds"; fi

log "packing new image (gzip, takes a few minutes)"
NEWSQ="$(dirname "$OUT")/new_tmp.sqfs"   # output goes to the SD card, not RAM
rm -f "$NEWSQ"
$RUN "$L/bin/mksquashfs" "$W/app" "$NEWSQ" -comp gzip -b 131072 -all-root -noappend -no-progress -processors 1 -mem 16M >"$W/mksq.log" 2>&1 || { tail -5 "$W/mksq.log"; log "mksquashfs failed"; rm -rf "$W"; exit 3; }
rm -rf "$W/app"
SIZE=$(stat -c%s "$NEWSQ")
log "squashfs size $SIZE"
if [ "$SIZE" -gt "$TARGET" ]; then log "image too big"; rm -rf "$W"; exit 3; fi
PAD=$((TARGET - SIZE))
mv -f "$NEWSQ" "$OUT" || exit 3
# 0xFF padding in 4 KiB blocks (mksquashfs pads to 4 KiB) + remainder
BLK=$((PAD / 4096)); REM=$((PAD % 4096))
[ "$BLK" -gt 0 ] && dd if=/dev/zero bs=4096 count="$BLK" 2>/dev/null | tr '\000' '\377' >> "$OUT"
[ "$REM" -gt 0 ] && dd if=/dev/zero bs=1 count="$REM" 2>/dev/null | tr '\000' '\377' >> "$OUT"
sync
FINAL=$(stat -c%s "$OUT")
[ "$FINAL" = "$TARGET" ] || { log "padding failed ($FINAL)"; exit 3; }
[ "$(dd if="$OUT" bs=4 count=1 2>/dev/null)" = "hsqs" ] || { log "bad magic"; exit 3; }
rm -rf "$W"
log "OK $OUT $(md5sum "$OUT" | awk '{print $1}')"
exit 0
