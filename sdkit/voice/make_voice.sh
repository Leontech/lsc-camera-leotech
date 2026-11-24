#!/bin/bash
# Generates all voice prompts (bilingual EN + CZ) as IMA-ADPCM .adp files (8 kHz mono).
# Uses Piper TTS (https://github.com/rhasspy/piper, pip install piper-tts) with open voices:
#   English: en_GB-cori-high       (dataset: LibriVox, public domain)
#   Czech:   cs_CZ-kasandra-medium (dataset: Ondřej Šimek, CC BY 4.0)
# Voice models: https://huggingface.co/rhasspy/piper-voices  (put the .onnx + .onnx.json into $VOICES)
# Output: out/sd/*.adp (SD installer, -> docs/kit/voice/) and out/sys/*.adp (firmware, -> payload/leo_sounds/sys/)
set -e
cd "$(dirname "$0")"
PIPER=${PIPER:-piper}; VOICES=${VOICES:-$HOME/piper-voices}
EN=$VOICES/en_GB-cori-high.onnx; CZ=$VOICES/cs_CZ-kasandra-medium.onnx
mkdir -p out/sd out/sys; T=$(mktemp -d)
tts(){ echo "$2" | $PIPER -m "$1" -f "$3" >/dev/null 2>&1; }
fin(){ ffmpeg -v error -y -i "$1" -af "aresample=8000,loudnorm=I=-14:TP=-1.5,aresample=8000" -ac 1 -ar 8000 -f s16le $T/x.s16
 python3 adp.py $T/x.s16 "$2"; echo "$2"; }
# bilingual prompt: English, short pause, Czech
say(){ tts $EN "$3" $T/en.wav; tts $CZ "$4" $T/cz.wav
 ffmpeg -v error -y -i $T/en.wav -f lavfi -t 0.4 -i anullsrc=r=22050:cl=mono -i $T/cz.wav -filter_complex \
  "[0:a]aresample=22050,aformat=channel_layouts=mono[a];[2:a]aresample=22050,aformat=channel_layouts=mono[c];[a][1:a][c]concat=n=3:v=0:a=1" $T/both.wav
 fin $T/both.wav out/$1/$2.adp; }
# single-language clip
one(){ tts "$1" "$3" $T/one.wav
 ffmpeg -v error -y -i $T/one.wav -af "silenceremove=start_periods=1:start_threshold=-45dB,areverse,silenceremove=start_periods=1:start_threshold=-45dB,areverse" $T/trim.wav
 fin $T/trim.wav out/sys/$2.adp; }

# ---- SD installer
say sd sd_hello "Memory card detected. Checking the camera." "Paměťová karta nalezena. Kontroluji kameru."
say sd sd_backup "Backing up the original firmware." "Zálohuji původní firmware."
say sd sd_building "Building the new firmware. This takes a few minutes. Do not unplug the camera." "Sestavuji nový firmware. Potrvá to několik minut. Kameru neodpojujte."
say sd sd_nomem "Not enough memory. Nothing was changed. Remove the card." "Nedostatek paměti. Nic nebylo změněno. Vyjměte kartu."
say sd sd_buildfail "Building the firmware failed. Nothing was changed. Remove the card." "Sestavení firmwaru selhalo. Nic nebylo změněno. Vyjměte kartu."
say sd sd_unsupported "Unsupported camera or firmware version. Nothing was changed. Remove the card." "Nepodporovaná kamera nebo verze firmwaru. Nic nebylo změněno. Vyjměte kartu."
say sd sd_flashing "Writing the new firmware. Do not unplug the camera." "Zapisuji nový firmware. Kameru neodpojujte."
say sd sd_done "Done. Remove the card. The camera will restart." "Hotovo. Vyjměte kartu, kamera se restartuje."
say sd sd_already "This firmware is already installed. Remove the card." "Tento firmware už je nainstalovaný. Vyjměte kartu."
say sd sd_failed "Writing failed. The previous firmware was restored." "Zápis selhal. Původní firmware byl obnoven."
say sd sd_fatal "Restore failed. Keep the camera powered on." "Obnova selhala. Nechte kameru zapnutou."
say sd sd_restore "Restoring the original firmware. Do not unplug the camera." "Obnovuji původní firmware. Kameru neodpojujte."
say sd sd_restored "The original firmware was restored. Remove the card. The camera will restart." "Původní firmware byl obnoven. Vyjměte kartu, kamera se restartuje."
# ---- firmware (button, QR Wi-Fi)
say sys qr_start "Wi-Fi setup. Show the network QR code to the camera." "Nastavení Wi-Fi. Ukažte kameře QR kód sítě."
say sys qr_read "QR code read. Connecting to the network." "QR kód načten. Připojuji se k síti."
say sys qr_cancel "Wi-Fi setup cancelled." "Nastavení Wi-Fi zrušeno."
say sys wifi_ok "Connected to Wi-Fi." "Připojeno k Wi-Fi."
say sys wifi_fail "Connection failed. Restoring the previous network." "Připojení se nezdařilo. Vracím původní síť."
say sys qr_timeout "Time is up. Wi-Fi setup finished." "Čas vypršel. Nastavení Wi-Fi ukončeno."
say sys qr_bad "This QR code does not contain Wi-Fi." "Tento QR kód neobsahuje Wi-Fi."
say sys pass_reset "The web password has been cleared." "Heslo webu bylo smazáno."
say sys factory "Restoring default settings and restarting." "Obnovuji výchozí nastavení a restartuji."
say sys ip_none "The camera is not connected to a network." "Kamera není připojena k síti."
# ---- IP address clips (spoken in English, then in Czech)
one $EN ip_en_intro "My IP address is"; one $EN ip_en_dot "dot"
one $CZ ip_cz_intro "Moje IP adresa je"; one $CZ ip_cz_dot "tečka"
D_EN=(zero one two three four five six seven eight nine); D_CZ=(nula jedna dva tři čtyři pět šest sedm osm devět)
for i in 0 1 2 3 4 5 6 7 8 9; do one $EN ip_en_$i "${D_EN[$i]}"; one $CZ ip_cz_$i "${D_CZ[$i]}"; done
rm -rf $T
