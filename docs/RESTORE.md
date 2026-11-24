# Restoring the original firmware / Návrat na původní firmware

> ⚠️ At your own risk – no warranty, no liability. / Na vlastní riziko – bez záruky, bez odpovědnosti.

The installer always saves a full backup of the original flash to the SD card:
`leo/backup/<camera MAC>/mtdblock0.bin … mtdblock7.bin` + `md5sums.txt`.
**Keep this folder on your PC** – `mtdblock7.bin` is your original firmware (APP partition).

## Method 1 – camera web page (easiest)
1. Open `http://<camera-ip>/` → **System → Firmware update**.
2. Choose **`mtdblock7.bin`** from your backup folder → *Upload and update*.
3. The image is verified (size, squashfs, MD5), written, and the camera restarts with the **original Tuya firmware**.

## Method 2 – SD card (works even if the web page is unreachable)
1. Prepare an SD card with the installer (the SD card page, or the SD kit).
2. Create the folder **`leo/restore/`** on the card and copy **`mtdblock7.bin`** and **`md5sums.txt`** from your backup into it.
3. Camera off → insert card → camera on. After ~30 s the camera says *"Restoring the original firmware"*,
   writes it, says *"Original firmware restored"* and restarts. Remove the card.
   (The restore folder is renamed to `leo/restore.done.*` so it is not repeated.)

## After restoring
- The camera runs the original **LSC Smart Connect / Tuya** firmware again.
- To use the **LSC Smart Connect app**: reset the camera the original way (see the original manual – usually hold the reset button for about 5 s) and add it in the app again.
- Your Wi-Fi settings from the custom firmware stay in the configuration partition until you reset the camera.

---

## Česky
Instalátor vždy uloží úplnou zálohu původní flash na kartu: `leo/backup/<MAC kamery>/` (`mtdblock0–7.bin` + `md5sums.txt`).
**Tuto složku si ulož do PC** – `mtdblock7.bin` je tvůj původní firmware.

**Způsob 1 – webová stránka kamery (nejjednodušší):** `http://<IP kamery>/` → **Systém → Aktualizace firmwaru** → vyber **`mtdblock7.bin`** ze zálohy → Nahrát. Obraz se ověří, zapíše a kamera naběhne s **původním firmwarem Tuya**.

**Způsob 2 – SD karta (i když web nejde):** připrav kartu s instalátorem, vytvoř na ní složku **`leo/restore/`** a nakopíruj do ní **`mtdblock7.bin`** a **`md5sums.txt`** ze zálohy. Vypnout → vložit kartu → zapnout. Kamera řekne „Obnovuji původní firmware“, zapíše ho, řekne „Původní firmware obnoven“ a restartuje se. Kartu vyndej.

**Potom:** kamera má zase původní firmware LSC Smart Connect / Tuya. Pro aplikaci LSC Smart Connect kameru resetuj původním způsobem (podle původního návodu – obvykle podržet reset asi 5 s) a přidej ji v aplikaci znovu.
