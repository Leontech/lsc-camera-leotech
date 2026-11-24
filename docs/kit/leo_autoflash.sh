#!/bin/sh
# leo_autoflash.sh - automatic ak_rtsp firmware install from the SD card (no PC needed).
# Runs on the camera from the SD boot hook (hack.sh). Voice prompts EN+CZ via leo_say.
#   stock APP (known MD5)          -> full backup of all MTD partitions to the card, flash
#   older ak_rtsp build (known MD5) -> backup of current APP, flash (upgrade)
#   same image already installed    -> say so once, then stay silent
#   no prebuilt image on the card   -> the firmware is BUILT ON THE CAMERA from its own original
#                                      firmware (leo_build_fw.sh + leo/payload), then flashed
#   anything else                   -> "unsupported", nothing is written
#   leo/restore/mtdblock7.bin present -> RESTORE the original firmware from that backup
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

# original firmware: the vendor app anyka_ipc holds the speaker and ~3 MB RAM. Replace it by a
# sleeping stub (bind mount, RAM only) - its supervisor restarts the stub, the watchdog stays fed.
if [ -x /usr/bin/anyka_ipc ] && pidof anyka_ipc >/dev/null 2>&1; then
    printf '#!/bin/sh\nexec sleep 999999\n' > /tmp/leo_ipc_stub; chmod +x /tmp/leo_ipc_stub
    if mount --bind /tmp/leo_ipc_stub /usr/bin/anyka_ipc; then
        killall anyka_ipc 2>/dev/null; sleep 2; killall -9 anyka_ipc 2>/dev/null; sleep 3
        echo "vendor app stopped"
    fi
fi

# leotech firmware running: before real work (backup, build, flash) stop ak_rtsp the same way, so the
# voice goes straight to the audio chip (a busy ak_rtsp lets the speaker hiss), the CPU/RAM are free for
# the build and nothing runs from the APP partition while it is rewritten. If nothing gets written,
# ak_rtsp is started again on exit.
APP_STOPPED=0
stop_app() {
    [ "$APP_STOPPED" = 1 ] && return
    [ -x /usr/bin/ak_rtsp ] && pidof ak_rtsp >/dev/null 2>&1 || return
    printf '#!/bin/sh\nexec sleep 999998\n' > /tmp/leo_app_stub; chmod +x /tmp/leo_app_stub
    mount --bind /tmp/leo_app_stub /usr/bin/ak_rtsp || return
    killall ak_rtsp 2>/dev/null; sleep 2; killall -9 ak_rtsp 2>/dev/null; sleep 2
    APP_STOPPED=1; echo "ak_rtsp stopped"
}
resume_app() {
    [ "$APP_STOPPED" = 1 ] || return
    umount /usr/bin/ak_rtsp 2>/dev/null
    for p in $(ps | grep "sleep 999998" | grep -v grep | awk '{print $1}'); do kill "$p" 2>/dev/null; done
    APP_STOPPED=0; echo "ak_rtsp started again"
}
trap resume_app EXIT

MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null | sed 's/://g')
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
# kernel + rootfs must be a known (tested) version - a vendor online update could change them
KMD5=$(md5sum /dev/mtdblock4 | awk '{print $1}'); RMD5F=$(md5sum /dev/mtdblock5 | awk '{print $1}')
echo "kernel md5 $KMD5 rootfs md5 $RMD5F"
if [ -f "$L/system_md5.txt" ] && ! { grep -qi "^$KMD5 .*kernel" "$L/system_md5.txt" && grep -qi "^$RMD5F .*rootfs" "$L/system_md5.txt"; }; then
    echo "unknown kernel/rootfs version - nothing written (please report these MD5s)"
    UNKNOWN_SYSTEM=1
fi
if ! check_layout; then
    echo "unsupported flash layout:"; cat /proc/mtd
    say sd_unsupported; exit 1
fi

# ---------------- RESTORE MODE: leo/restore/mtdblock7.bin = original firmware backup ----------------
RS="$L/restore"
if [ -f "$RS/mtdblock7.bin" ]; then
    R="$RS/mtdblock7.bin"
    echo "RESTORE requested: $R"
    if [ "$(stat -c%s "$R")" != "5242880" ] || [ "$(dd if="$R" bs=4 count=1 2>/dev/null)" != "hsqs" ]; then
        echo "restore image invalid (size/magic)"; say sd_unsupported; exit 1
    fi
    RMD5=$(md5sum "$R" | awk '{print $1}')
    if [ -f "$RS/md5sums.txt" ] && ! grep -q "^$RMD5 .*mtdblock7.bin" "$RS/md5sums.txt"; then
        echo "restore image does not match md5sums.txt - nothing written"; say sd_unsupported; exit 1
    fi
    CUR=$(md5sum /dev/mtdblock7 | awk '{print $1}')
    if [ "$CUR" = "$RMD5" ]; then
        echo "original firmware already installed"; mv "$RS" "$L/restore.done.$(date +%s)" 2>/dev/null; sync
        say sd_restored; exit 0
    fi
    BK="$L/backup/$MAC"; mkdir -p "$BK"
    dd if=/dev/mtdblock7 of="$BK/app_before_restore_$CUR.bin" bs=64k 2>/dev/null; sync
    if [ -f "$L/flash_tool" ]; then cp "$L/flash_tool" /tmp/leo_flash_tool; else cp /usr/bin/flash_tool /tmp/leo_flash_tool; fi; chmod +x /tmp/leo_flash_tool
    cp "$L/leo_say" /tmp/leo_say && chmod +x /tmp/leo_say
    stop_app
    say sd_restore
    sync
    /tmp/leo_flash_tool "$R" /dev/mtd7 >/tmp/leo_flash.log 2>&1
    RC=$?; tail -n 3 /tmp/leo_flash.log
    if [ "$RC" -eq 0 ]; then
        mv "$RS" "$L/restore.done.$(date +%s)" 2>/dev/null; sync
        echo "$RMD5" > "$L/state/$MAC.restored"; sync   # do not install again from this card
        echo "ORIGINAL FIRMWARE RESTORED -> reboot"
        /tmp/leo_say "$L/voice/sd_restored.adp"; sync; sleep 2; APP_STOPPED=0; reboot; exit 0
    fi
    echo "restore flash failed ($RC) - writing back previous APP"
    /tmp/leo_flash_tool "$BK/app_before_restore_$CUR.bin" /dev/mtd7 >/tmp/leo_flash.log 2>&1
    if [ $? -eq 0 ]; then /tmp/leo_say "$L/voice/sd_failed.adp"; sync; sleep 2; APP_STOPPED=0; reboot; exit 2; fi
    /tmp/leo_say "$L/voice/sd_fatal.adp"; exit 3
fi

if [ "${UNKNOWN_SYSTEM:-0}" = "1" ]; then say sd_unsupported; exit 1; fi

IMG=""
for f in "$L"/firmware/ak_rtsp_firmware_*.squashfs; do [ -f "$f" ] && IMG="$f"; done
CUR=$(md5sum /dev/mtdblock7 | awk '{print $1}')
echo "current APP md5 $CUR"
BK="$L/backup/$MAC"
mkdir -p "$BK"
# the original firmware was restored from this card and is still running -> do not reinstall
# (the SD page deletes this marker when the card is written for installing again)
if [ -f "$L/state/$MAC.restored" ] && [ "$(cat "$L/state/$MAC.restored")" = "$CUR" ]; then
    echo "original firmware restored from this card - not installing again"; exit 0
fi

if [ -n "$IMG" ]; then
    # ---------- prebuilt image on the card (private kit) ----------
    if [ "$(stat -c%s "$IMG")" != "5242880" ]; then echo "prebuilt image has wrong size"; say sd_unsupported; exit 1; fi
    NEW=$(md5sum "$IMG" | awk '{print $1}')
    echo "prebuilt image $IMG md5 $NEW"
    if [ "$CUR" = "$NEW" ]; then
        if [ -f "$L/state/$MAC.$NEW" ]; then echo "already installed (announced before) - silent"; exit 0; fi
        touch "$L/state/$MAC.$NEW"; sync; echo "already installed"; say sd_already; exit 0
    fi
    stop_app
    say sd_hello
    KIND=""
    grep -qi "^$CUR" "$L/stock_md5.txt" 2>/dev/null && KIND=stock
    grep -qi "^$CUR" "$L/custom_md5.txt" 2>/dev/null && KIND=custom
    [ -n "$KIND" ] || { echo "current APP unknown - nothing written"; say sd_unsupported; exit 1; }
    if [ ! -f "$BK/md5sums.txt" ]; then
        say sd_backup
        for i in 0 1 2 3 4 5 6 7; do dd if=/dev/mtdblock$i of="$BK/mtdblock$i.bin" bs=64k 2>/dev/null; done
        (cd "$BK" && md5sum mtdblock*.bin > md5sums.txt); sync
    fi
else
    # ---------- build the image on the camera from its own original firmware ----------
    [ -f "$L/payload/ak_rtsp" ] && [ -f "$L/leo_build_fw.sh" ] || { echo "no firmware image and no payload"; say sd_unsupported; exit 1; }
    PAY=$(md5sum "$L/payload/ak_rtsp" | awk '{print $1}')
    if [ -f "$L/state/$MAC.built" ] && [ "$(cat "$L/state/$MAC.built")" = "$CUR $PAY" ]; then
        echo "this build is already installed - silent"; exit 0
    fi
    # the SD card only converts the ORIGINAL firmware; a camera that already runs leotech firmware is
    # updated over its web page, never rebuilt from the card (so an inserted card cannot overwrite it)
    if ! grep -qi "^$CUR" "$L/stock_md5.txt" 2>/dev/null; then
        if [ -x /usr/bin/ak_rtsp ]; then
            echo "leotech firmware already installed - nothing to do (updates: camera web page)"
            if [ ! -f "$L/state/$MAC.already.$CUR" ]; then touch "$L/state/$MAC.already.$CUR"; sync; say sd_already; fi
            exit 0
        fi
        echo "current APP is neither original nor leotech - nothing written"; say sd_unsupported; exit 1
    fi
    stop_app
    say sd_hello
    STOCKSRC=""
    if grep -qi "^$CUR" "$L/stock_md5.txt" 2>/dev/null; then
        # original firmware running: full backup first (also the build source)
        if [ ! -f "$BK/md5sums.txt" ] || ! grep -q "^$CUR .*mtdblock7.bin" "$BK/md5sums.txt"; then
            say sd_backup
            for i in 0 1 2 3 4 5 6 7; do dd if=/dev/mtdblock$i of="$BK/mtdblock$i.bin" bs=64k 2>/dev/null; done
            (cd "$BK" && md5sum mtdblock*.bin > md5sums.txt); sync
        fi
        STOCKSRC="$BK/mtdblock7.bin"
    fi
    if [ -z "$STOCKSRC" ]; then echo "no original firmware backup for this camera - nothing written"; say sd_unsupported; exit 1; fi
    [ "$(md5sum "$STOCKSRC" | awk '{print $1}')" = "$(grep 'mtdblock7.bin' "$BK/md5sums.txt" | awk '{print $1}')" ] || { echo "backup does not verify"; say sd_unsupported; exit 1; }
    mkdir -p "$L/build"
    IMG="$L/build/ak_rtsp_firmware_built.squashfs"
    say sd_building
    L="$L" sh "$L/leo_build_fw.sh" "$STOCKSRC" "$IMG"
    BRC=$?
    if [ "$BRC" -ne 0 ]; then
        echo "build failed ($BRC) - nothing written"
        if [ "$BRC" -eq 4 ]; then say sd_nomem; else say sd_buildfail; fi
        exit 1
    fi
    NEW=$(md5sum "$IMG" | awk '{print $1}')
    KIND=built
fi
echo "flashing ($KIND) $IMG md5 $NEW"
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
tail -n 3 /tmp/leo_flash.log
if [ "$RC" -eq 0 ]; then
    touch "$L/state/$MAC.$NEW"
    [ "$KIND" = "built" ] && echo "$NEW $PAY" > "$L/state/$MAC.built"
    sync
    rm -f /etc/config/leo_fw_seen   # the new firmware announces itself after the restart
    sync
    echo "FLASH OK -> reboot"
    /tmp/leo_say "$L/voice/sd_done.adp"
    sync; sleep 2; APP_STOPPED=0; reboot; exit 0
fi
echo "flash failed ($RC) - restoring previous APP"
/tmp/leo_flash_tool "$PREV" /dev/mtd7 >/tmp/leo_flash.log 2>&1
RRC=$?
tail -n 3 /tmp/leo_flash.log
if [ "$RRC" -eq 0 ]; then
    /tmp/leo_say "$L/voice/sd_failed.adp"; sync; sleep 2; APP_STOPPED=0; reboot; exit 2
fi
echo "RESTORE FAILED ($RRC) - keep the camera powered"
/tmp/leo_say "$L/voice/sd_fatal.adp"
exit 3
