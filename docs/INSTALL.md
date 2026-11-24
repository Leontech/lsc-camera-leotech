# Installation / Instalace

> ⚠️ At your own risk – no warranty, no liability. / Na vlastní riziko – bez záruky, bez odpovědnosti.
>
> 📵 After installing, the **original LSC Smart Connect app and Tuya cloud stop working** with the camera. Back to original: [RESTORE.md](RESTORE.md).
> 📵 Po instalaci **přestane fungovat původní aplikace LSC Smart Connect i cloud Tuya**. Návrat: [RESTORE.md](RESTORE.md).

## Camera / Kamera
Action **LSC Smart Connect** indoor IP camera 1080p, item **3215672**, label "No." **3215672.3** (Anyka AK3918AV130).

## A) SD card installer (recommended)
> 💡 Do not connect a new camera to the LSC app / cloud first – an online update could install an untested firmware.
1. Format a microSD card (≤ 32 GB) as **FAT32**.
2. Open **https://leontech.github.io/lsc-camera-leotech/** in Chrome/Edge on a computer, tick the disclaimer and choose the card – the page writes everything, including the EN + CZ voice prompts. The camera talks during the whole installation and tells you each step.
3. Camera off → insert card → camera on. After ~30 s: backup of the original firmware to `leo/backup/<MAC>` on the card, **the camera builds the new firmware itself** (a few minutes), writes it (~30 s) and restarts. Unknown camera/firmware → nothing is written.
4. Keep the backup from the card! Wi-Fi: hold reset 2 s and show the phone's Wi-Fi QR code. Short press = the camera says its IP.

## B) First install with the Linux wizard
The upstream wizard [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) makes the backup and installs the base ak_rtsp firmware; afterwards upload the leotech image on the camera's web page (System → Firmware update) or via `ota_push.py`.

## Updates
Camera web page → System → Firmware update (MD5 check, automatic rollback).

## Reset button
short press = says the IP address (English + Czech) · 2 s = Wi-Fi via QR · 5 s = clear web password · 15 s = defaults (image, MQTT, password) + restart. Wi-Fi is never erased.

---
**CZ:** Novou kameru před instalací nepřipojuj k aplikaci LSC. Postup A – kartu naformátuj na FAT32, na stránce **https://leontech.github.io/lsc-camera-leotech/** ji připrav, vlož do vypnuté kamery a zapni. Kamera zálohuje originál na kartu (`leo/backup/<MAC>`), sama si sestaví nový firmware (několik minut), zapíše ho a restartuje se. Neznámou kameru/verzi odmítne. Zálohu si ulož! Wi-Fi nastavíš QR kódem (reset 2 s).
