# Building the firmware / Sestavení firmwaru

Requirements: Linux or WSL2 (Ubuntu), `git`, `python3`, `squashfs-tools`, internet. The compiler (zig 0.14.1) is downloaded by the upstream wizard.

1. Make a backup of your camera's flash and unpack the APP partition (mtdblock7) – the upstream wizard
   [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) does this
   (`~/.cache/lsc-3215672-decloud-wizard/…`), or by hand: `unsquashfs -d app_extracted mtdblock7.bin`.
2. Clone this repository and smolrtsp:
   ```sh
   git clone https://github.com/Leontech/lsc-camera-leotech && cd lsc-camera-leotech
   git clone https://github.com/OpenIPC/smolrtsp src/smolrtsp   # see upstream docs for the build of smolrtsp
   ```
3. Voice prompts – ready-made ones are in `docs/kit/voice/` (SD installer) and `docs/kit/payload/leo_sounds/sys/`
   (firmware). To regenerate them (Piper TTS, open voices): `sdkit/voice/make_voice.sh`. Copy the `out/sys/*.adp` files to
   `src/ak_rtsp/leo_sounds/sys/` (+ `beep.s16`, `ding3.s16` short beeps) and your own sounds as `.s16`
   (8 kHz mono s16le) to `src/ak_rtsp/leo_sounds/`.
4. Build:
   ```sh
   cd src/ak_rtsp
   make                                   # needs zig on PATH
   APP_EXTRACTED=/path/to/app_extracted FW_DIR=/tmp/out bash ./build_firmware.sh
   ```
   Result: `/tmp/out/ak_rtsp_firmware_<date>_<time>.squashfs` (5 242 880 B).
5. Install: SD card page (see [INSTALL.md](INSTALL.md)) or OTA from the camera's web page.

The image contains parts of the original vendor firmware – **do not publish it**.
Upstream technical documentation: the other files in this `docs/` folder (from FluxAlchemist).
