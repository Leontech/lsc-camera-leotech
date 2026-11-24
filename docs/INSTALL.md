# Installation / Instalace

> ⚠️ At your own risk – no warranty, no liability. / Na vlastní riziko – bez záruky, bez odpovědnosti.

## Camera / Kamera
Action **LSC Smart Connect** indoor IP camera 1080p, item **3215672**, label "No." **3215672.3** (Anyka AK3918AV130).

## A) SD card installer (recommended once you have a firmware image)
1. Build the firmware image from your camera's backup – [BUILD.md](BUILD.md) (Linux/WSL).
2. Format a microSD card (≤ 32 GB) as **FAT32**.
3. Open **https://leontech.github.io/lsc-camera-leotech/** in Chrome/Edge, tick the disclaimer, choose the image and the card – the page writes everything.
4. Camera off → insert card → camera on. After ~30 s: backup of the original firmware to `leo/backup/<MAC>` on the card, flash (~30 s), restart. Unknown camera/firmware → nothing is written.
5. Keep the backup from the card! Wi-Fi: hold reset 2 s and show the phone's Wi-Fi QR code.

## B) First install with the Linux wizard
The upstream wizard [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) makes the backup and installs the base ak_rtsp firmware; afterwards upload the leotech image on the camera's web page (System → Firmware update) or via `ota_push.py`.

## Updates
Camera web page → System → Firmware update (MD5 check, automatic rollback).

## Reset button
2 s = Wi-Fi via QR · 5 s = clear web password · 15 s = defaults (image, MQTT, password) + restart. Wi-Fi is never erased.

---
**CZ:** Postup A – sestav obraz ze zálohy své kamery ([BUILD.md](BUILD.md)), kartu naformátuj na FAT32, na stránce **https://leontech.github.io/lsc-camera-leotech/** ji připrav, vlož do vypnuté kamery a zapni. Kamera nejdřív zálohuje originál na kartu (`leo/backup/<MAC>`), pak zapíše nový firmware a restartuje se. Neznámou kameru/verzi odmítne. Zálohu si ulož! Wi-Fi nastavíš QR kódem (reset 2 s).
