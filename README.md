# HaloWatch

**English** · [Русский](README.ru.md)

A watch firmware for the round **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (466 × 466 AMOLED, touch, speaker,
microphones, battery) with a **voice assistant**: hold BOOT, ask, let go, and the answer is spoken. It runs on
**Gemini Live**, Google's speech-in, speech-out streaming API, over Wi-Fi. The look and the motion are modeled on
**Wear OS**, using only open sources (see [Credits](#credits)).

Everything is set up from a **web page over USB** in Chrome: Wi-Fi, the API key, the voice and the prompt, the weather
cities, the clock, plus a live log. No app, no rebuild.

It runs on **Halo**, a small UI library written from scratch for this board: its own display driver, its own touch
driver, its own drawing. No LVGL, no Arduino_GFX. Animations run at **60 fps with no dropped frames and no tearing**,
and when nothing moves, no frames are drawn at all. Left alone, it dims to an **Always On** clock and sleeps until you
touch it, press a button or pick it up.

<p align="center"><a href="https://www.youtube.com/watch?v=7rBIQl6EG08"><img src="https://img.youtube.com/vi/7rBIQl6EG08/hqdefault.jpg" width="480" alt="HaloWatch on the board — video review"></a><br><sub>▶ Video review on YouTube (version 1.1, before the assistant)</sub></p>

| Watch face | Weather | Weather, page 2 | Settings |
| :---: | :---: | :---: | :---: |
| ![](docs/screenshots/face.png) | ![](docs/screenshots/weather.png) | ![](docs/screenshots/weather_ny.png) | ![](docs/screenshots/settings.png) |
| **Display** | **Battery** | **Locked** | **Always On** |
| ![](docs/screenshots/display.png) | ![](docs/screenshots/battery.png) | ![](docs/screenshots/lock.png) | ![](docs/screenshots/aod.png) |

<sub>Screenshots are from version 1.1: the face and the weather now show real data, and the gallery holds
Assistant, Weather and Settings.</sub>

## What it does

- **Voice assistant.** Hold **BOOT** anywhere (on any screen, in Always On, even asleep) and talk; let go and the
  answer is spoken through the watch's speaker. You can start talking at once: the watch records while Wi-Fi and the
  session come up. On the watch face or the gallery, BOOT opens the **Assistant** screen: an orb that listens
  (blue), thinks (violet) and speaks (teal), following your voice and its own, with what you said and the answer as
  live captions. Press BOOT while it speaks to interrupt it with a new question. The conversation stays open for two
  minutes after the last answer (follow-ups are instant), and resumes where it left off after that. Answers can use
  **Google Search** (news, scores, facts) and know your local time and city.
- **Watch face**: digits and letters change with a rolling animation, like a mechanical counter. The time comes from
  the internet (or from the web controller). The pill shows the real weather of your first city; tap it to open the
  weather.
- **Weather** for up to four places, from [Open-Meteo](https://open-meteo.com) (no key needed): now, today's high and
  low, the next three hours. Updated every 30 minutes.
- **Gallery** of cards, as on a Pixel Watch: Assistant, Weather, Settings. Cards shrink near the edge of the circle,
  the list flies on with inertia and bounces back.
- **Transitions**: Fade between the face and the gallery, the Android 16 transition into screens, swipe back from
  the left edge that follows the finger.
- **Settings** on the watch: a brightness slider (remembered after a restart) and a battery screen with a 24-hour chart.
- **Web controller** (Chrome or Edge, over USB): Wi-Fi with a network scan, the Gemini API key, model, voice, language
  and system prompt, speaker volume and microphone gain with a mic test, weather places and units, time zone and
  24-hour clock, the watch's status, and a live, filterable log you can download.
- **Wi-Fi only when needed**: on for the assistant, a weather update or a time sync, off a minute later.
- **Always On**: after 15 s without touches or motion the screen fades out and only the face's date and clock stay,
  dimmed, on black. The board sleeps and wakes once a minute for a moment to change the digits (and shift them a few
  pixels, so the AMOLED does not burn in). **Touch the screen, press BOOT or pick the watch up**: you are back where
  you were, at full brightness, at once; the waking touch presses nothing. On our battery Always On used about
  **10 % in 8 hours** (100 % → 90 % overnight; measured on version 1.1, with Wi-Fi off).
- **Buttons**: BOOT is push-to-talk; PWR locks the touch screen (a lock icon appears at the top), a long press unlocks.

### Gestures and buttons

| Where | Gesture | What happens |
| --- | --- | --- |
| Anywhere | hold BOOT, talk, release | the assistant answers aloud |
| While it answers | press BOOT again | it stops; ask the next question |
| Watch face | swipe up | gallery |
| Watch face | tap the weather pill | weather (swipe up from the bottom edge to go back) |
| Gallery | swipe down from the top edge | watch face |
| Any screen | swipe right from the left edge | back |
| Any screen | swipe up from the bottom edge | watch face |
| Always On | touch, press BOOT or pick the watch up | back where you were, at once |

## Set it up

1. **Flash** the firmware (see [Install](#install)).
2. **Open the web controller** in Chrome or Edge on a computer:
   [`docs/controller/index.html`](docs/controller/index.html). Download it and open the file, or serve the repository's
   `docs` folder with GitHub Pages (Settings → Pages → Branch, folder `/docs`), then open `…/controller/`.
3. Plug the watch in over USB, press **Connect watch** and pick the *USB JTAG/serial* port. The watch's clock is set from
   your computer right away.
4. **Wi-Fi**: press *Scan networks*, pick yours, type the password, *Save*. (2.4 GHz only.)
5. **Assistant**: get a free API key at [Google AI Studio](https://aistudio.google.com/apikey), paste it, *Save*.
   Optionally pick a voice, a language and edit the system prompt. Type a question into *Ask* to test it without
   talking.
6. **Weather**: enter up to four places ("Paris", "Portland, Oregon", or "48.85, 2.35"), choose °F or °C, *Save*.
7. Hold BOOT and ask something.

If the assistant does not hear you well, try *Mic test* (it records 3 s, plays them back and logs the level), then
adjust the microphone gain or pick a single microphone.

## The board

<p align="center"><img src="docs/board.webp" width="460" alt="Waveshare ESP32-S3-Touch-AMOLED-1.75C"><br><sub>Image: Waveshare</sub></p>

**[Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm)** is a small
round development board in an aluminum case: a 1.75-inch AMOLED touch screen, sound, a motion sensor and a battery
connector. Documentation — [docs.waveshare.com](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C), examples
and schematics — [GitHub](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C).

| Part | What it is | In HaloWatch |
| --- | --- | --- |
| Processor | ESP32-S3R8: two Xtensa LX7 cores up to 240 MHz, 512 KB SRAM, 8 MB PSRAM | both cores draw every frame |
| Flash | 32 MB | firmware and settings |
| Radio | Wi-Fi 2.4 GHz (802.11 b/g/n), Bluetooth 5 LE, onboard antenna | Wi-Fi: the assistant, weather, time |
| Screen | 1.75" AMOLED, 466 × 466, 16.7M colors, 700 cd/m², CO5300 driver over QSPI, TE signal | own driver |
| Touch | capacitive, CST9217 (I2C) | own driver |
| Motion sensor | QMI8658, 6 axes: accelerometer and gyroscope | wakes the watch when you pick it up (its wake-on-motion pin, INT1, goes to GPIO21) |
| Power | AXP2101 power chip, connector for a 3.7 V Li-ion battery (MX1.25), charging over USB | battery screen, PWR button |
| Sound | ES8311 codec with a speaker; ES7210 with two microphones and echo cancellation | the assistant: speaker and both microphones, one I2S bus in full duplex |
| Buttons | PWR and BOOT, both programmable | lock; push-to-talk |
| USB | Type-C, the ESP32-S3's own USB: flashing and a serial port | flashing, the web controller |
| Case | one-piece CNC aluminum, anodized, lanyard holes | — |

**About the RTC.** The introduction of Waveshare's documentation mentions an RTC, but the board has **no separate
real-time clock chip**. There is none on the [schematic](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/blob/main/hardware/schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf), and an I2C scan of the board finds only `0x18`
ES8311, `0x34` AXP2101, `0x40` ES7210, `0x5A` CST9217 and `0x6B` QMI8658 (plus an unidentified `0x7E`, an address
reserved by the I2C standard) — no RTC address such as `0x51` or `0x68`. The time is kept by the ESP32-S3 itself, with
its built-in RTC timer, only while the board has power; after power-off it is lost, and the watch gets it back from the
internet (SNTP) or from the web controller. There is no microSD slot either.

## How it works

- **Only what changed is drawn.** Every element knows where it was and where it is now; only those parts of the
  screen are redrawn and sent. A new minute on the face is 1–2 % of the screen, 1–4 ms.
- **Own display driver.** The screen is drawn in bands of 16 rows. While one band goes out over QSPI by DMA, the next
  one is already being drawn.
- **Both cores draw each band**, left and right halves, with a moving border so they finish together.
- **No tearing.** The display reports each refresh with its TE signal; a light frame is sent ahead of the refresh,
  a heavy one right behind it.
- **Own touch driver.** The touch chip raises a pin on every report; a task reads it and keeps the finger's speed,
  so lists fly on and transitions decide whether to finish.
- **Steady motion.** The animation clock is the moment the screen will show the frame (taken from the TE signal,
  like Android's Choreographer), so every frame moves by the same step; and a moving screen does not switch between
  "ahead of the refresh" and "behind it" mid-way — that used to show a frame twice or drop one.
- **The finger, resampled.** The touch chip reports ~100 times a second, out of step with the 60 Hz screen. The
  finger is taken at a fixed moment before each frame is shown, between two reports, as Android does: content under
  the finger moves by even steps (on a real drag: uneven by 0.56 px instead of 3.2 px).
- **Light sleep.** Memory and program stay, so waking takes ~20–35 ms to a picture — no boot. The AMOLED keeps the
  Always On picture by itself while the processor sleeps. Time in sleep is counted by the fast RC oscillator / 256:
  the default slow one ran 0.5 % fast (half a minute per hour asleep), this one 0.03–0.06 %.
- **The assistant streams both ways.** A WebSocket over TLS to Gemini Live (`HaloWatch/gemini.h`, written on
  `esp_tls`): the microphones go out as 16-bit PCM at 24 kHz, 100 ms per message, while BOOT is held (push-to-talk:
  `activityStart` / `activityEnd`); the spoken answer comes back as 24 kHz PCM and plays as it arrives, after 200 ms of
  buffer. Both directions go through 1 MB rings in PSRAM, so a slow network or a slow start never drops what you said.
  Speaker and microphones share one I2S bus in full duplex (`halo_mic_*` in Halo).
- **Springs, as in Wear OS.** Stiffness, damping, durations and how cards shrink at the edge come from the open
  source code of Compose for Wear OS. A spring is solved exactly, so a bounce looks the same at any frame rate.

| What | Frame time | Missed frames |
| --- | --- | --- |
| Idle | no frames | — |
| New minute on the face | 1–4 ms | 0 |
| Paging | median 3.3 ms | 0 |
| Scrolling the card list | median 9–11 ms | 0 |
| Always On | one frame a minute, the rest asleep | — |

The frame budget is 16.7 ms.

## Install

### Ready file (easiest)

Download **`HaloWatch-v1.2-full.bin`** from [Releases](../../releases) and write it to the board at address **0x0**.
Writing the full image at 0x0 also clears the watch's settings (Wi-Fi, API key); to update without losing them, write
**`HaloWatch-v1.2-app.bin`** at **0x10000** instead.

**In the browser** (Chrome or Edge, any OS): open [ESP Tool](https://espressif.github.io/esptool-js/), connect the
board by USB, press **Connect**, set the flash address, choose the file and press **Program**.

**From the command line** (macOS, Linux, Windows; needs Python 3):

```bash
pip install esptool
```

```bash
esptool --chip esp32s3 write-flash 0x0 HaloWatch-v1.2-full.bin
```

esptool finds the board itself; if not, add `--port` with the board's port: `/dev/cu.usbmodem101` on macOS,
`/dev/ttyACM0` on Linux, `COM5` on Windows. On Linux you may need to add yourself to the `dialout` group first.

If the board doesn't connect: hold **BOOT**, plug in the cable, release **BOOT**, and try again. After writing, press
**PWR** or replug the cable. Then set the watch up with the [web controller](#set-it-up).

### From source

You need [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/) (macOS: `brew install arduino-cli`)
and the ESP32 core **3.3.11**:

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

```bash
arduino-cli core update-index
```

```bash
arduino-cli core install esp32:esp32@3.3.11
```

Build and upload (replace the port with yours):

```bash
arduino-cli compile --upload -p /dev/cu.usbmodem101 --libraries libraries --fqbn "esp32:esp32:esp32s3:FlashSize=32M,PSRAM=opi,FlashMode=qio,PartitionScheme=app5M_fat24M_32MB,USBMode=hwcdc,CDCOnBoot=cdc" HaloWatch
```

No other libraries are needed: Wi-Fi, TLS and the WebSocket come from the ESP32 core (ESP-IDF).

Fonts, icons and images are already built into `HaloWatch/src/halo_assets.*`. To change them, edit
`HaloWatch/assets/halo.conf` and run `python3 tools/halo_assets.py HaloWatch` (needs `pip install numpy pillow`; the
fonts are downloaded from their original repositories on first run).

### USB commands

The firmware listens on the USB serial port (115200); the web controller uses the same commands, and its command line
can send any of them.

| Command | What it does |
| --- | --- |
| `cfg` | all settings as `CFG {json}` (secrets masked) |
| `set KEY VALUE` | change a setting (`wifi_ssid`, `wifi_pass`, `ai_key`, `ai_model`, `ai_voice`, `ai_lang`, `ai_prompt`, `ai_search`, `ai_auto_vad`, `ai_keep`, `volume`, `mic_gain`, `mic_channel`, `wx_cities`, `wx_fahrenheit`, `wx_every`, `tz`, `clock24`, `net_idle`); saved in flash |
| `status` | Wi-Fi, clock, assistant, weather, battery and memory as `STATUS {json}` |
| `time EPOCH [MINUTES]` | set the clock (the controller sends the computer's time and UTC offset) |
| `wifi`, `wifi scan`, `wifi on`, `wifi off` | Wi-Fi state, a scan as `SCAN [...]`, connect now, turn off |
| `ask TEXT` | ask the assistant by text; the answer is spoken |
| `mic test` | record 3 s, play them back, log the level |
| `ai`, `ai close` | the assistant's state; close its session |
| `weather` | update the weather now |
| `reboot` | restart |

From before: `face`, `face off`, `open 0`…`2`, `back`, `tap X Y`, `swipe up|down|left|right [edge]`, `pwr`,
`pwr long`, `batdemo`, `log`, `sys`; Always On: `sleep`, `wake`, `aod on|off`, `idle N` (seconds, 0 — never).

On USB power the watch does not really sleep (the USB port would go quiet and the board could not be flashed): it
dims and wakes the same way, the processor just keeps running. On battery it really sleeps (not while Wi-Fi is on or
the assistant is busy). `usb on` makes it sleep on USB too — then wake it by hand before flashing.

## Project layout

| Folder | What's inside |
| --- | --- |
| `HaloWatch/` | The firmware: `HaloWatch.ino` (gallery), `gemini.h` (Gemini Live client), `assistant.h` (Assistant screen, push-to-talk), `net.h` (Wi-Fi, time, HTTPS), `config.h` (settings, serial protocol), `status.h`, `face.h`, `weather.h` (screen and Open-Meteo), `settings.h`, `display.h`, `battery.h`, `power.h` (Always On and sleep); `assets/` — fonts and icons sources |
| `libraries/Halo/` | The Halo library: display and touch drivers, frame engine, drawing, springs, lists, screen transitions, audio (speaker and microphones), sleep and wake-up |
| `docs/controller/` | The web controller: one HTML file, no dependencies (Web Serial) |
| `tools/` | `halo_assets.py` — fonts, icons and images for Halo |

## Versions

| Version | What's new |
| --- | --- |
| 1.2 | Voice assistant (Gemini Live, hold BOOT to talk), the web controller over USB, Wi-Fi, time from the internet, real weather (Open-Meteo), microphones in Halo; the music player and demo screens removed |
| [1.1](../../releases/tag/v1.1) | Always On and sleep (wake by touch, BOOT or picking the watch up), steadier motion, smoother dragging, BOOT puts the board to sleep |
| [1.0](../../releases/tag/v1.0) | First release |

## Credits

**The interface is modeled on Wear OS, using only open sources.** The code is written from scratch. No firmware or
app was decompiled or reverse-engineered, and no code was taken from GPL projects. The motion numbers and ideas
come from:

| Source | License | What we used |
| --- | --- | --- |
| [androidx](https://github.com/androidx/androidx) — Compose for Wear OS | Apache 2.0 | spring stiffness and damping, durations and easing curves, how cards shrink at the edge, swipe back, indicators |
| [wear-os-samples](https://github.com/android/wear-os-samples) | Apache 2.0 | how a screen is put together from components |
| [PebbleOS](https://github.com/coredevices/PebbleOS) | Apache 2.0 | screen transition techniques on a microcontroller |
| [Material Design 3](https://m3.material.io) and Wear OS design guidelines | public guidelines | overall style: colors, corner radii, sizes |

Services the watch talks to:

| Service | Terms | What for |
| --- | --- | --- |
| [Gemini API](https://ai.google.dev) — Live API | your own API key, [Gemini API terms](https://ai.google.dev/gemini-api/terms) | the assistant (the protocol's message fields follow Google's open-source [python-genai](https://github.com/googleapis/python-genai) SDK, Apache 2.0; no code taken) |
| [Open-Meteo](https://open-meteo.com) | free for non-commercial use; data [CC BY 4.0](https://open-meteo.com/en/license) | weather and geocoding. Weather data by Open-Meteo.com |
| pool.ntp.org, time.google.com | public NTP | the time |

Third-party parts included in this repository:

| What | Author | License |
| --- | --- | --- |
| Google Sans Flex font (glyphs rendered into `HaloWatch/src/halo_assets.cpp`) | Google | [SIL OFL 1.1](https://openfontlicense.org) |
| Font Awesome Free icons (rendered into `HaloWatch/src/halo_assets.cpp`) | Fonticons, Inc. | icons [CC BY 4.0](https://fontawesome.com/license/free) |
| ES8311 codec register sequence (`libraries/Halo/src/halo_audio.cpp`) | based on Espressif's es8311 driver | Apache 2.0 |
| ES7210 microphone ADC register sequence (`libraries/Halo/src/halo_audio.cpp`) | based on [Waveshare's es7210 example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) for this board | Apache 2.0 |
| Board pins and display start-up sequence | based on [Waveshare examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) | Apache 2.0 |
| Board picture (`docs/board.webp`) | Waveshare, from the [product page](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm) | shown only to identify the board |

## License

The code of this repository is under the **MIT** license ([LICENSE](LICENSE)): you may use it for anything,
including commercial projects. The third-party parts above keep their own licenses.

This is an independent project. It is not affiliated with or endorsed by Google, Espressif or Waveshare. Wear OS,
Pixel Watch, Android, Material Design and Gemini are trademarks of Google LLC; ESP32 is a trademark of Espressif Systems; Waveshare is a trademark of Waveshare Electronics;
the names are used only to describe what the interface looks like and what it runs on.
