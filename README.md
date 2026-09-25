# airgroup

Turns a Raspberry Pi Zero 2 W into an AirPlay speaker that plays through a Google Home / Nest
**speaker group**. Pick it from the AirPlay menu in Apple Music on your iPhone, and all the
speakers in the group play in sync.

```
iPhone ──AirPlay──▶ shairport-sync ──PCM (FIFO)──▶ airgroup ──MP3 over HTTP──▶ Google Home group
                          │                           ▲
                          └── start / stop / volume ──┘  (session hooks → local HTTP API)
```

- **shairport-sync** is the AirPlay receiver. It writes raw audio into `/run/airgroup/audio.fifo`.
- **airgroup** (`airgroup.py`) paces that audio in real time and fills pauses with silence, so the
  stream never stalls. It encodes the audio to 320 kbps MP3 with ffmpeg, serves it at
  `http://<pi>:8090/stream.mp3`, and uses the Cast protocol to tell the group to play that URL.
- When you pick the speaker on the phone, casting starts. The phone's volume slider controls the
  group's volume. About 30 s after you disconnect, the speakers are released. If you cast something
  else to the group or say "Hey Google, stop", airgroup stops casting until the next AirPlay session.

## Install

1. Flash the SD card with **Raspberry Pi Imager**: device *Raspberry Pi Zero 2 W*, OS
   *Raspberry Pi OS Lite (64-bit)*. Under **Edit settings**:
   - **General:** set the hostname (e.g. `airgroup`), a username and password, and your Wi-Fi.
     The Zero 2 W only does **2.4 GHz**, and network names are case-sensitive.
   - **Services:** turn on **SSH** with public-key authentication and paste your Mac's public key.

   Put the card in the Pi and power it through the **PWR IN** port. The first boot takes a few minutes.
2. Copy this folder to the Pi and run the installer with your group's name exactly as it appears
   in the Google Home app. `sudo` will ask for the Pi password you set in Imager.

   ```bash
   scp -r airgroup adam@airgroup.local:~
   ssh -t adam@airgroup.local 'cd airgroup && sudo ./install.sh "All Speakers"'
   ```

   A second argument sets a different AirPlay name: `sudo ./install.sh "All Speakers" "Pi Speakers"`.

3. On the iPhone, open Apple Music, tap the AirPlay icon, and choose the new speaker.

Tested on a Pi Zero 2 W running Raspberry Pi OS 13 (Trixie) with shairport-sync 4.3.7, playing
to a group of Nest Audio and Home Mini speakers.

If you're not sure of the group's exact name, list what the Pi can see:

```bash
/opt/airgroup/venv/bin/python /opt/airgroup/airgroup.py --list
```

## Status page and settings

- Open `http://<pi-ip>:8090/` to see the current state, with **Cast now** and **Stop** buttons.
- Settings are in `/etc/default/airgroup`: group name, bitrate, volume sync, a max-volume cap and
  timeouts. Run `sudo systemctl restart airgroup` after you edit it.
- To watch the logs: `journalctl -u airgroup -u shairport-sync -f`

## Things to know

- **Delay:** Expect roughly 3–5 seconds between the phone and the speakers. Most of this comes from
  AirPlay's buffer plus the Cast group's own buffer, which keeps 4 speakers in sync. Music plays
  fine, but pause and skip take a few seconds to be heard. The first second or two of the first
  track can be cut off while the group starts up.
- **AirPlay 1:** Raspberry Pi OS's `shairport-sync` package is AirPlay 1. It works from Apple
  Music, Control Center and any app. It can't join multi-room AirPlay 2 groups with other AirPlay
  speakers, but you don't need that here because the Cast group does the multi-room part.
- **Offline speakers:** If one speaker in the group is offline or unplugged, Google sometimes won't
  start the group. Check the Home app first.
- **Wi-Fi power saving:** The installer turns off Wi-Fi power saving on the Pi. On a Zero, power
  saving is the usual cause of dropouts and of speakers that are hard to find.

## Troubleshooting

| Symptom | Check |
|---|---|
| Pi doesn't appear in the AirPlay list | `systemctl status shairport-sync`; phone and Pi must be on the same network |
| `No Cast device or group named ...` | Run `--list`. The name must match exactly. The Pi and speakers need mDNS/multicast between them (watch out for guest networks and "AP isolation") |
| Casting starts but nothing plays | The speakers must be able to reach `http://<pi-ip>:8090`. If the Pi has several addresses, set `ADVERTISE_HOST` |
| Status shows `preempted` | Something else took over the group. Choose the AirPlay speaker again, or press **Cast now** |

## Pico W version (experimental)

[`pico/`](pico/) holds an in-progress port to the Raspberry Pi Pico W microcontroller. It can
already drive a Cast group, but it can't receive AirPlay yet. See its README.

## Uninstall

```bash
sudo systemctl disable --now airgroup
sudo rm -rf /opt/airgroup /etc/systemd/system/airgroup.service \
  /etc/systemd/system/shairport-sync.service.d/airgroup.conf /usr/local/bin/airgroup-volume /etc/default/airgroup
sudo mv /etc/shairport-sync.conf.orig /etc/shairport-sync.conf && sudo systemctl daemon-reload && sudo systemctl restart shairport-sync
```
