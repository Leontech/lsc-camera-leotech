<p align="center"><a href="https://leotech.cz"><img src="docs/web/leotech_logo.svg" alt="leotech.cz" width="420"></a></p>

# LSC Smart Connect 3215672.3 – local firmware (ak_rtsp · leotech edition)

**English** · [Česky](#česky)

| ✅ Compatible camera / Kompatibilní kamera | |
|---|---|
| **Action – LSC Smart Connect indoor IP camera 1080p**<br>**Art. No. 3215672.3** (printed on the box)<br>Anyka AK3918AV130 · 2.4 GHz Wi-Fi (WPA/WPA2) | <img src="images/box/art_no_3215672_3.jpg" alt="Art. No. 3215672.3 on the Action box" width="300"> |

> Check the **Art. No.** on the box before you start. Plain `3215672` (no suffix) is a different camera (Ingenic) and is **not** supported here.
> Zkontroluj **Art. No.** na krabici. Samotné `3215672` (bez přípony) je jiná kamera (Ingenic) a **není** podporovaná.

> ⚠️ **USE AT YOUR OWN RISK.** Flashing custom firmware can permanently brick the camera, void the warranty and
> violate the terms of the original app/cloud. Everything here is provided **"AS IS", without any warranty**, and
> **the authors accept no responsibility or liability** for any damage, data loss or other consequences. Make your
> own backup and only proceed if you understand what you are doing.

Cloud-free firmware for the cheap **Action "LSC Smart Connect" indoor IP camera** – no Tuya cloud, no app,
just your local network: RTSP for Frigate/VLC, a web page, MQTT with Home Assistant auto-discovery, speaker,
Wi-Fi setup by QR code and over-the-air updates.

## Which camera?

| | |
|---|---|
| Product | **LSC Smart Connect – Inteligentní vnitřní IP kamera 1080p** (Action, item **3215672**) |
| **Required version** | "Art. No." on the box must be **3215672.3** (tested). `.1` / `.2` use the same Anyka SoC and will probably work; the plain **3215672** (no suffix, Ingenic T23) is a **different camera** – use [Thingino](https://github.com/themactep/thingino-firmware/wiki/Camera:-LSC-3215672) for it |
| SoC | Anyka **AK3918AV130**, 64 MB RAM, 8 MB SPI NOR flash |
| Shop | e.g. [action.com – 3215672](https://www.action.com/pl-pl/p/3215672/inteligentna-wewnetrzna-kamera-ip-lsc-smart-connect/) (sold in Action stores across the EU, 2026) |

The installer checks the flash layout and the exact MD5 of the original firmware; an unknown camera or firmware
version is refused and **nothing is written**.

## Features

- **RTSP** 1920×1080 H.264 (+ audio) – works with Frigate (incl. Intel VAAPI), go2rtc, VLC
- **Web page** (`http://<camera-ip>/`), Czech/English by browser language, optional password:
  day/night mode, IR LED, status LED, flip/mirror, brightness/contrast/saturation/sharpness, snapshot preview,
  sounds (upload any MP3/WAV – converted in the browser), MQTT settings, camera name, firmware update
- **MQTT + Home Assistant discovery** – selects, switches, sliders, a button for every sound, restart
- **Speaker** – built-in and user sounds, play from HA (`mqtt.publish … /play`), live stream to TCP 8093
- **Wi-Fi by QR code** – hold the reset button 2 s and show the phone's "share Wi-Fi" QR code; falls back to the
  previous network if the new one does not connect
- **Reset button** – 2 s QR Wi-Fi · 5 s clear web password · 15 s defaults (Wi-Fi is never erased)
- **Voice prompts** (English + Czech) during installation and setup
- **OTA** firmware updates from the web page (MD5 verified, automatic rollback)
- Persistent settings, IR LED independent of day/night, status LED control

## How it works / installation

| Step | Needs |
|---|---|
| First install on a stock camera | microSD card (FAT32), see [docs/INSTALL.md](docs/INSTALL.md) |
| Updates | the camera's web page (System → Firmware update) |
| Building from source | Linux (or WSL), see [docs/BUILD.md](docs/BUILD.md) |

**Why is there no ready-made firmware image to download?** The image is built from the camera's *own* original
firmware (kernel modules, Anyka libraries) which belongs to the manufacturer and cannot be redistributed. This
repository only contains open-source code; you build the image from the backup of your own camera, exactly like
the upstream project does.

## Credits & licences

| Part | Author | Licence |
|---|---|---|
| ak_rtsp firmware (base of this project) | [FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware) | MIT |
| Installer wizard (first flash from Linux) | [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) | MIT |
| SD-card boot hook toolkit | [tasarren/lsc-tuya-toolkit](https://github.com/tasarren/lsc-tuya-toolkit) | not bundled here – downloaded from its own repository |
| RTSP library | [OpenIPC/smolrtsp](https://github.com/OpenIPC/smolrtsp) | MIT |
| QR decoder | [dlbeer/quirc](https://github.com/dlbeer/quirc) | ISC |
| leotech additions (web, MQTT, speaker, QR Wi-Fi, OTA, button, SD installer, voice tool) | Leoš Vilím / [leotech.cz](https://leotech.cz) | MIT |

This project is not affiliated with Action, LSC, Tuya or Anyka. All trademarks belong to their owners.

If this saved you some time: [☕ ko-fi.com/leotechcz](https://ko-fi.com/leotechcz)

---

## Česky

> ⚠️ **VŠE NA VLASTNÍ RIZIKO.** Nahrání vlastního firmwaru může kameru trvale zničit, ukončit záruku a porušit
> podmínky původní aplikace/cloudu. Vše je poskytováno **„tak jak je“, bez jakékoli záruky** a **autoři nenesou
> žádnou odpovědnost** za škody, ztrátu dat ani jiné následky. Udělej si zálohu a pokračuj jen pokud víš, co děláš.

Firmware bez cloudu pro levnou **vnitřní IP kameru LSC Smart Connect z Action** – bez Tuya cloudu a bez aplikace,
jen v lokální síti: RTSP pro Frigate/VLC, webová stránka, MQTT s automatickým přidáním do Home Assistant,
reproduktor, nastavení Wi-Fi QR kódem a aktualizace po síti.

**Kamera:** LSC Smart Connect vnitřní IP kamera 1080p z Action, **Art. No. 3215672.3** (na krabici, viz fotka nahoře).
Verze `.1`/`.2` mají stejný čip Anyka a pravděpodobně budou fungovat; kamera s číslem jen **3215672** (bez přípony,
čip Ingenic) je **jiná** – pro ni je [Thingino](https://github.com/themactep/thingino-firmware/wiki/Camera:-LSC-3215672).
Instalátor kontroluje rozložení flash a přesné MD5 původního firmwaru – neznámou kameru/verzi odmítne a **nic nezapíše**.

- **Instalace:** microSD karta (FAT32) – [docs/INSTALL.md](docs/INSTALL.md)
- **Aktualizace:** webová stránka kamery (Systém → Aktualizace firmwaru)
- **Sestavení ze zdrojáků:** Linux / WSL – [docs/BUILD.md](docs/BUILD.md)
- **Proč tu není hotový obraz ke stažení?** Obraz obsahuje části původního firmwaru výrobce (moduly jádra, knihovny
  Anyka), které nesmíme šířit. Každý si ho sestaví ze zálohy své vlastní kamery.

Projekt nemá nic společného s firmami Action, LSC, Tuya ani Anyka.
Pokud ti to ušetřilo čas: [☕ ko-fi.com/leotechcz](https://ko-fi.com/leotechcz) · [leotech.cz](https://leotech.cz)
