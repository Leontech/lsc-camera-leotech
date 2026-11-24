#!/bin/bash
# Generates the bilingual (EN + CZ) voice prompts as IMA-ADPCM .adp files.
# Uses edge-tts (pip install edge-tts) + ffmpeg. The generated audio is NOT part of this
# repository - check the terms of the TTS service you use, or record your own voice.
set -e
cd "$(dirname "$0")"
EN=${EN_VOICE:-en-GB-SoniaNeural}; CZ=${CZ_VOICE:-cs-CZ-VlastaNeural}
say(){ edge-tts --voice $EN --text "$2" --write-media en.mp3 >/dev/null; edge-tts --voice $CZ --text "$3" --write-media cz.mp3 >/dev/null
 ffmpeg -v error -y -i en.mp3 -f lavfi -t 0.35 -i anullsrc=r=24000:cl=mono -i cz.mp3 -filter_complex "[0:a]aresample=24000,aformat=channel_layouts=mono[a];[2:a]aresample=24000,aformat=channel_layouts=mono[c];[a][1:a][c]concat=n=3:v=0:a=1,aresample=8000,loudnorm=I=-14:TP=-1.5,aresample=8000" -ac 1 -ar 8000 -f s16le $1.s16
 python3 adp.py $1.s16 $1.adp && rm -f $1.s16; echo "$1.adp"; }
# SD installer prompts
say sd_hello "Memory card detected. Checking the camera." "Paměťová karta nalezena. Kontroluji kameru."
say sd_backup "Backing up the original firmware." "Zálohuji původní firmware."
say sd_unsupported "Unsupported camera or firmware version. Nothing was changed. Remove the card." "Nepodporovaná kamera nebo verze firmwaru. Nic nebylo změněno. Vyjměte kartu."
say sd_flashing "Writing new firmware. Do not unplug the camera." "Zapisuji nový firmware. Kameru neodpojujte."
say sd_done "Done. Remove the card. The camera will restart." "Hotovo. Vyjměte kartu, kamera se restartuje."
say sd_already "This firmware is already installed. Remove the card." "Tento firmware už je nainstalovaný. Vyjměte kartu."
say sd_failed "Writing failed. The previous firmware was restored." "Zápis selhal. Původní firmware byl obnoven."
say sd_fatal "Restore failed. Keep the camera powered on." "Obnova selhala. Nechte kameru zapnutou."
# firmware system prompts (copy to src/ak_rtsp/leo_sounds/sys/)
say qr_start "Wi-Fi setup. Show the network QR code to the camera." "Nastavení Wi-Fi. Ukažte kameře QR kód sítě."
say qr_read "QR code read. Connecting to the network." "QR kód načten. Připojuji se k síti."
say wifi_ok "Connected to Wi-Fi." "Připojeno k Wi-Fi."
say wifi_fail "Connection failed. Restoring the previous network." "Připojení se nezdařilo. Vracím původní síť."
say qr_timeout "Time is up. Wi-Fi setup finished." "Čas vypršel. Nastavení Wi-Fi ukončeno."
say qr_bad "This QR code does not contain Wi-Fi." "Tento QR kód neobsahuje Wi-Fi."
say pass_reset "The web password has been cleared." "Heslo webu bylo smazáno."
say factory "Restoring default settings and restarting." "Obnovuji výchozí nastavení a restartuji."
rm -f en.mp3 cz.mp3
