#!/bin/sh
# leo_autoflash.sh - automatic ak_rtsp firmware install from the SD card (no PC needed).
# Runs on the camera from the SD boot hook (hack.sh). Voice prompts EN+CZ via leo_say.
#   stock APP (known MD5)          -> full backup of all MTD partitions to the card, flash
#   older ak_rtsp build (known MD5) -> backup of current APP, flash (upgrade)
#   same image already installed    -> say so once, then stay silent
#   anything else                   -> "unsupported", nothing is written
# On flash failure the APP from before is written back.  (c) 2026 leotech.cz, MIT
SD=/tmp/sd
L="$SD/leo"
LOG="$SD/logs/leo_autoflash.log"
mkdir /tmp/leo_autoflash.lock 2>/dev/null || exit 0
mkdir -p "$SD/logs" "$L/state" "$L/backup"
exec >>"$LOG" 2>&1
say() { chmod +x "$L/leo_say" 2>/dev/null; "$L/leo_say" "$L/voice/$1.adp" || echo "say $1 failed"; }
echo "=== leo_autoflash $(date) ==="

# give the system time to start (audio driver, network, ak_rtsp speaker service)
sleep 25

MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':\n')
[ -n "$MAC" ] || MAC=unknown
echo "camera MAC: $MAC"

check_layout() {
    grep -q '^mtd0: 00032000 00001000 "UBOOT"$' /proc/mtd && \
    grep -q '^mtd1: 00001000 00001000 "ENV"$' /proc/mtd && \
    grep -q '^mtd2: 00001000 00001000 "ENVBK"$' /proc/mtd && \
    grep -q '^mtd3: 00010000 00001000 "DTB"$' /proc/mtd && \
    grep -q '^mtd4: 00180000 00001000 "KERNEL"$' /proc/mtd && \
    grep -q '^mtd5: 000fc000 00001000 "ROOTFS"$' /proc/mtd && \
    grep -q '^mtd6: 00040000 00001000 "CONFIG"$' /proc/mtd && \
    grep -q '^mtd7: 00500000 00001000 "APP"$' /proc/mtd
}
if ! check_layout; then
    echo "unsupported flash layout:"; cat /proc/mtd
    say sd_unsupported; exit 1
fi

IMG=""
for f in "$L"/firmware/ak_rtsp_firmware_*.squashfs; do [ -f "$f" ] && IMG="$f"; done
if [ -z "$IMG" ] || [ "$(stat -c%s "$IMG")" != "5242880" ]; then
    echo "no valid firmware image in $L/firmware"; say sd_unsupported; exit 1
fi
NEW=$(md5sum "$IMG" | awk '{print $1}')
CUR=$(md5sum /dev/mtdblock7 | awk '{print $1}')
echo "image $IMG md5 $NEW"
echo "current APP md5 $CUR"

if [ "$CUR" = "$NEW" ]; then
    if [ -f "$L/state/$MAC.$NEW" ]; then echo "already installed (announced before) - silent"; exit 0; fi
    touch "$L/state/$MAC.$NEW"; sync
    echo "already installed"; say sd_already; exit 0
fi

say sd_hello
KIND=""
grep -qi "^$CUR" "$L/stock_md5.txt" 2>/dev/null && KIND=stock
grep -qi "^$CUR" "$L/custom_md5.txt" 2>/dev/null && KIND=custom
if [ -z "$KIND" ]; then
    echo "current APP is neither a known stock nor a known ak_rtsp build - nothing written"
    say sd_unsupported; exit 1
fi
echo "current APP: $KIND"

BK="$L/backup/$MAC"
mkdir -p "$BK"
if [ ! -f "$BK/md5sums.txt" ]; then
    say sd_backup
    for i in 0 1 2 3 4 5 6 7; do dd if=/dev/mtdblock$i of="$BK/mtdblock$i.bin" bs=64k 2>/dev/null; done
    (cd "$BK" && md5sum mtdblock*.bin > md5sums.txt)
    sync
    echo "full backup ($KIND) saved to $BK"
fi
PREV="$BK/app_before_$CUR.bin"
if [ ! -f "$PREV" ]; then dd if=/dev/mtdblock7 of="$PREV" bs=64k 2>/dev/null; sync; fi
if [ "$(md5sum "$PREV" | awk '{print $1}')" != "$CUR" ]; then
    echo "backup of current APP does not verify - nothing written"; say sd_unsupported; exit 1
fi

# run flash_tool from RAM, not from the APP partition being overwritten
if [ -f "$L/flash_tool" ]; then cp "$L/flash_tool" /tmp/leo_flash_tool; else cp /usr/bin/flash_tool /tmp/leo_flash_tool; fi; chmod +x /tmp/leo_flash_tool
cp "$L/leo_say" /tmp/leo_say && chmod +x /tmp/leo_say
say sd_flashing
sync
/tmp/leo_flash_tool "$IMG" /dev/mtd7 >/tmp/leo_flash.log 2>&1
RC=$?
cat /tmp/leo_flash.log
if [ "$RC" -eq 0 ]; then
    touch "$L/state/$MAC.$NEW"; sync
    echo "FLASH OK -> reboot"
    /tmp/leo_say "$L/voice/sd_done.adp"
    sync; sleep 2; reboot; exit 0
fi
echo "flash failed ($RC) - restoring previous APP"
/tmp/leo_flash_tool "$PREV" /dev/mtd7 >/tmp/leo_flash.log 2>&1
RRC=$?
cat /tmp/leo_flash.log
if [ "$RRC" -eq 0 ]; then
    /tmp/leo_say "$L/voice/sd_failed.adp"; sync; sleep 2; reboot; exit 2
fi
echo "RESTORE FAILED ($RRC) - keep the camera powered"
/tmp/leo_say "$L/voice/sd_fatal.adp"
exit 3
