# NightDriverStrip (lawrencek52 fork) <!-- omit in toc -->

<!-- markdownlint-disable MD033 /no-inline-html -->
<img src="assets/NightDriverLogo-small.png" width="400" alt="NightDriver logo" />

NightDriverStrip is ESP32 firmware that drives WS2812B-style LED strips and
HUB75 LED matrices: it runs its own effects, including audio-reactive ones,
and it can show frames streamed to it over WiFi. It was created by
[Dave Plummer](https://github.com/davepl) and is developed by the
NightDriverStrip community at
[PlummersSoftwareLLC/NightDriverStrip](https://github.com/PlummersSoftwareLLC/NightDriverStrip).

This repository is a fork of that project. It adds support for the Seeed XIAO
ESP32-C5 and ESP32-S3 and the Waveshare ESP32-S3-RGB-Matrix boards, makes
every output channel its own strip or matrix, and was developed alongside
[LED-Central](https://github.com/lawrencek52/LED-Central), a server that
drives many controllers as one display. Everything here builds on the upstream
project; see [Credits](#credits).

## Table of Contents <!-- omit in toc -->

- [Credits](#credits)
- [What this fork adds](#what-this-fork-adds)
- [Tested hardware](#tested-hardware)
- [Getting started](#getting-started)
- [Configuring a controller](#configuring-a-controller)
- [Using it with LED-Central](#using-it-with-led-central)
- [Diagnostics](#diagnostics)
- [Effects](#effects)
- [Device web UI and API](#device-web-ui-and-api)
- [Feature defines](#feature-defines)
- [IR remote control](#ir-remote-control)
- [Adding new effects](#adding-new-effects)
- [Notes on JSON persistence](#notes-on-json-persistence)
- [Resetting the effect list](#resetting-the-effect-list)
- [Fetching things from the Internet](#fetching-things-from-the-internet)
- [Build pointers](#build-pointers)
- [File system](#file-system)
- [Tools](#tools)
- [COM port problems](#com-port-problems)
- [Bonus exercise](#bonus-exercise)
- [Super Bonus Exercise](#super-bonus-exercise)
- [Sample parts (Plummer's Software LLC Amazon affiliate links)](#sample-parts-plummers-software-llc-amazon-affiliate-links)
- [Contributing, and the BlinkenPerBit metric](#contributing-and-the-blinkenperbit-metric)

## Credits

- **Dave Plummer** ([Dave's Garage](https://www.youtube.com/@DavesGarage))
  created NightDriverStrip in 2021 and continues to lead it. The core design
  (the effect manager, the GFX layer, timestamped WiFi frames with NTP so
  effects can span many ESP32s, the web UI and the effect library) is his and
  the community's work. Dave's Mesmerizer kits, the project's HUB75 hardware,
  are [sold on eBay](https://www.ebay.com/sch/i.html?_ssn=davepl).
- **The NightDriverStrip contributors**: more than thirty people, most of all
  Rutger van Bergen and Robert Lipe, plus keiranhines and many others. See the
  [upstream contributors](https://github.com/PlummersSoftwareLLC/NightDriverStrip/graphs/contributors).
- **Effect sources** credited in the code, among them
  [Aurora](https://github.com/pixelmatix/aurora) by Jason Coon (Pixelmatix),
  Stefan Petrick's noise effects, the
  [SoulMate Lights](https://editor.soulmatelights.com/) community gallery
  (including Stepko), and [FastLED](https://github.com/FastLED/FastLED).
- **This fork** (Lawrence King, from August 2026) is the work described below.
  Its Halloween effect's eyes and lightning are inspired by
  [WLED](https://github.com/wled/WLED)'s "Halloween Eyes" and "Lightning"
  (which build on Harm Aldick's WS2812FX); no code was taken from them.

NightDriverStrip is free software under the GNU General Public License,
version 3 or later; see [COPYING.txt](COPYING.txt). Questions about the
upstream project belong in its
[Discussions](https://github.com/PlummersSoftwareLLC/NightDriverStrip/discussions).

## What this fork adds

**Boards**

- **Seeed XIAO ESP32-C5** (`xiao_c5`): a single-core RISC-V chip with
  dual-band (5 GHz) WiFi. Its RMT peripheral can't stream long strips
  reliably alongside WiFi, so four WS2812 strips are driven in parallel by its
  PARLIO peripheral straight from a DMA buffer in PSRAM.
- **Seeed XIAO ESP32-S3 / Sense on a custom carrier** (`xiao_sense`): four
  strip outputs, the Sense's PDM microphone, IR receiver, coredump partition.
- **Waveshare ESP32-S3-RGB-Matrix** (`waveshare_esp32s3_rgb_matrix_64x64`,
  `_128x64`): chained 64 x 64 HUB75 panels (up to three), the onboard ES7210
  microphone array and the ES8311 speaker. Fixes for its RAM limits, row
  alignment, USB serial and a 32 fps ceiling.

**Strips and topology**

- Each output channel is its own strip (with its own length) or matrix,
  configured in the web UI and applied live, without a reboot.
- A per-channel LED density (LEDs per meter), so effects can be sized in
  real-world units.
- Shortening a strip blanks the LEDs it gives up; effects draw each strip at
  its own length (several heap overruns on uneven strips fixed).
- A **Length Test** effect for finding each strip's exact LED count: the last
  LED is white, with ruler ticks every 5 and 10 LEDs, and it follows the strip
  length setting as you change it.

**LED-Central and multi-controller sync**

- Several senders at once, and per-channel remote feeds: strips fed by
  LED-Central show its frames while the others keep running local effects,
  without mixing the two.
- Per-strip effects and frame rates (pin an effect to one strip).
- **Clock sync for multi-controller effects.** A proper NTP client (round-trip
  compensation, millisecond reply timing, quarter-offset steering) that syncs
  to LED-Central's host while it sends frames and to the configured server
  otherwise, and that keeps running while the strips are remote-driven. All
  controllers stay within a few milliseconds of each other, and frames are
  shown within about a millisecond of their timestamp.
- The **Fire**, **CandyCane** and **Halloween** effects ported from
  LED-Central, so a controller shows the same looks on its own.

**Running unattended**

- A nightly brightness schedule (dim, off and on times, with sunrise and
  sunset worked out from a postal code without an API key).
- WiFi watchdogs: rebuild the connection when the gateway goes silent while
  WiFi still reports "connected", and reboot after a sustained WiFi loss, with
  the reason in `/statistics`.
- Crash reports over WiFi (`/api/v1/crash`), with every build's ELF archived
  so a report can always be decoded.
- IR remote power on/off; a speaker "warble" for alerts on the Waveshare
  board; a true-random / pseudo-random setting for repeatable testing.

**Web UI and tools**

- Per-strip length and topology settings, multi-strip frame preview, audio
  telemetry (`/ws/audio` with per-band FFT magnitudes), per-effect brightness
  and frame-rate overrides, network statistics, and the build time on the
  Statistics tab.
- `ota_all.sh` flashes every controller over WiFi; `tools/archive_elf.py`
  keeps each build's ELF in `elf-archive/`.

## Tested hardware

| Board | Build environment | Used as | What's attached |
| - | - | - | - |
| Seeed XIAO ESP32-C5 (8 MB flash, 8 MB PSRAM) | `xiao_c5`, `xiao_c5_ota` | deck1, deck2, garage1 | Up to 4 WS2812B-type strips on D7-D10 (GPIO 12, 8, 9, 10); deck strips about 216 LEDs at 60 LED/m, garage 2 x 120 |
| Seeed XIAO ESP32-S3 Sense on a custom carrier (8 MB flash, 8 MB PSRAM) | `xiao_sense`, `xiao_sense_ota` | deck3, matrix-16x16 | Up to 4 strips on D7-D10 (GPIO 44, 7, 8, 9); deck3 3 x 108 LEDs at 30 LED/m; matrix-16x16 a 256-LED serpentine matrix; PDM microphone; IR receiver on D3 |
| Waveshare ESP32-S3-RGB-Matrix (32 MB flash, 16 MB PSRAM) | `waveshare_esp32s3_rgb_matrix_64x64`, `_128x64` | Matrix-64x64, Matrix-128x64 | One or two chained 64 x 64 HUB75 panels; onboard microphones and speaker |

Strip builds are compiled for up to 1220 LEDs per channel and a 150 W power
limit across the four outputs. The controllers run on 5 GHz (C5) and 2.4 GHz
WiFi behind a Google Nest mesh, driven by LED-Central on a Raspberry Pi 4.

The other ~60 build environments come from upstream and are built, but not
tested, in this fork. Dave's web installer
(<https://plummerssoftwarellc.github.io/NightDriverStrip>) flashes upstream
builds, not this fork's; see upstream's README for it.

## Getting started

1. Install [PlatformIO](https://platformio.org/) (the VS Code extension or
   PlatformIO Core). Nothing else is needed: the web UI is plain HTML/JS that
   the build gzips and embeds.
2. Copy `include/secrets.example.h` to `include/secrets.h` and put your WiFi
   name and password in it.
3. Build and flash over USB the first time, for example:

   ```shell
   pio run -e xiao_c5 -t upload
   pio device monitor
   ```

4. Open the controller's IP address (or `http://<hostname>.local`) in a
   browser. Set the hostname, location, strip count and each strip's length
   on the Settings tab.
5. After that, flash over WiFi: `pio run -e xiao_c5_ota -t upload` (set
   `upload_port` in `platformio.ini`, or pass `--upload-port`), or
   `./ota_all.sh` to build and flash every controller listed in it. A board
   that is still USB-attached is safest flashed over USB.

## Configuring a controller

The Settings tab groups the device settings. The ones that matter most for
these builds:

- **Topology**: strip channel count, and per channel its shape (strip or
  matrix), strip length or matrix size and wiring, and LEDs per meter.
  Applied live. While the *Length Test* effect is playing, a strip length
  change applies on every up/down arrow click.
- **Output**: driver, color order and data pins (compiled defaults shown).
- **Nightly schedule**: *Enable nightly schedule*, *Dim time*, *Dim to*,
  *Off time* and *On time*. Times can follow sunrise and sunset worked out
  from *Location* (a postal code is fine) or a fixed time.
- **NTP server address**: the internet fallback; LED-Central's host is used
  automatically while it is sending frames.
- **True random numbers**: off makes local effects repeat exactly.
- **Brightness**, **Power limit**, **Remember current effect** and the
  remote's global colors.

## Using it with LED-Central

LED-Central sends timestamped frames to port 49152; each channel it feeds
shows those frames, and channels it doesn't feed keep running local effects.
When LED-Central stops, the controller falls back to its own effects. The
controller reports its strip lengths, LED densities, microphone, nightly
schedule state and clock sync in `/statistics`, which LED-Central reads. Run
`setup-linux.sh` from LED-Central on its host so the controllers can sync
their clocks to it.

## Diagnostics

- **`/statistics`** (JSON) includes, besides memory, CPU and frame rates:
  `ACTIVE_STRIP_LENGTHS`, `ACTIVE_LEDS_PER_METER`, `RESET_REASON`,
  `UPTIME_SECONDS`, `WIFI_RSSI`, `WIFI_DISCONNECTS`, `WIFI_STALL_RECONNECTS`,
  `WIFI_WATCHDOG_REBOOTS` and `WIFI_WATCHDOG_LAST_REASON`, `SCHEDULE_STATE`,
  `NTP_SOURCE`, `NTP_SERVER`, `NTP_OFFSET_MS`, `NTP_ROUND_TRIP_MS`,
  `CRASH_PRESENT` and `BUILD_TIMESTAMP`. See [REST_API.md](REST_API.md).
- **Crash reports.** After a panic, `GET /api/v1/crash` gives the reset
  reason and a summary (an Xtensa backtrace on the S3), `/api/v1/crash/raw`
  the whole core dump, and `POST /api/v1/crash/clear` removes it. Decode a
  dump with the ELF in `elf-archive/` whose name matches the summary's
  `elfSha256` (`addr2line` on the S3; `esp_coredump` with the RISC-V GDB on
  the C5). In this fork's experience a panic inside the output code
  (`PostProcessFrame`, `Show`) usually means something wrote past a strip's
  pixel buffer: look for the writer, not the victim.

## Effects

[EFFECTS.md](EFFECTS.md) describes the effect sets, the effects in the DEMO
set these boards run, and the matrix effects.

## Device web UI and API

On devices with WiFi, NightDriverStrip can start a webserver that hosts the web UI that is part of the project. It can be used to view and change what effect is running, and get live performance statistics of the device.

When the device is started with the webserver enabled, the web UI can be accessed by opening a web browser and typing the IP address of your device in the address bar. Once loaded, the icons at the left of the screen can be used to toggle views within the UI on and off.

Two live data feeds are available in the UI when the build supports them: a preview of the pixels being sent to the LEDs (over the `/ws/frames` WebSocket) and, on builds with audio, a telemetry view of the FFT band peaks, VU levels and beat detection (over `/ws/audio`). Both have their own Connect/Disconnect buttons, and the Statistics tab reports whether each socket is compiled into the running firmware.

More information about the web UI can be found [in its own README.md](site/README.md).

Besides the web UI, the webserver also publishes a REST-like API. Amongst others, a range of configuration settings can be read and changed using it.
More information about the API is available in [REST_API.md](./REST_API.md).

## Feature defines

These defines enable the major features of NightDriverStrip. Define them in platformio.ini's build_flags or in globals.h.
Note: Some defines are board specific, this is noted below.

| Feature Define        | Description                                                        |
| --------------------- | ------------------------------------------------------------------ |
| ENABLE_WIFI           | Connect to WiFi                                                    |
| INCOMING_WIFI_ENABLED | Accepting incoming color data and commands                         |
| ENABLE_WEBSERVER      | Turn on the internal webserver                                     |
| TIME_BEFORE_LOCAL     | How many seconds before the lamp times out and shows local content |
| ENABLE_NTP            | Set the clock from the web                                         |
| ENABLE_OTA            | Accept over the air flash updates                                  |
| COLORDATA_SERVER_ENABLED | Turn on the internal color data server; allows TCP clients to receive updates on what's being displayed on the LEDs that the device is driving. |

| Hardware Specific | Description                                         | Supported Boards             |
| ----------------- | --------------------------------------------------- | ---------------------------- |
| USE_SCREEN        | Enable the on-device status/stats display           | Any board with a screen      |
| ENABLE_AUDIO      | Listen for audio from the microphone and process it | Boards with an analog, I2S or PDM mic - e.g. M5Stick-C/C Plus, Mesmerizer, XIAO ESP32-S3 Sense |
| ENABLE_REMOTE     | IR Remote Control                                   | Requires IR Hardware         |

`USE_SCREEN` is the only display flag you set yourself. Which driver gets used is then derived automatically in `globals.h` from the board definition that `platformio.ini` selects, so you do **not** set these:

| Derived define | Driver selected                              | Chosen for                     |
| -------------- | -------------------------------------------- | ------------------------------ |
| USE_M5DISPLAY  | Stats display on the built-in LCD            | M5Stick-C and M5Stick-C Plus   |
| USE_OLED       | Stats display on the built-in monochrome OLED | Heltec WiFi Kit 32             |
| USE_LCD        | Stats display on the onboard ILI9341 LCD     | Wrover32                       |
| USE_TFTSPI     | Stats display via TFT_eSPI                   | TTGO, Feather TFT, LilyGO S3   |

<!-- markdownlint-disable MD033 -->
The webserver, which can be enabled using ENABLE_WEBSERVER as indicated, comes with a number of capabilities that can themselves be enabled or disabled individually. Each of them comes with a default enabled state that depends on the values of other defines. The defaults can be overridden by setting the defines explicitly. Do note that enabling a feature is only sensible if its prerequisites are met - those are the conditions that enable the feature by default.<br>The following table discusses this.
<!-- markdownlint-enable MD033 -->

| Webserver feature | Description | Enabled by default if... |
| - | - | - |
| ENABLE_WEB_UI | Enable the on-board web UI | ENABLE_WEBSERVER is 1 |
| COLORDATA_WEB_SOCKET_ENABLED | Enable the WebSocket for sending color data to connected web UI clients (browsers); required for the effect preview feature of the web UI | ENABLE_WIFI is 1 and ENABLE_WEBSERVER is 1 and COLORDATA_SERVER_ENABLED is 1 |
| EFFECTS_WEB_SOCKET_ENABLED | Enable the WebSocket for pushing updates to connected web UI clients (browsers) about effects, both activation state and configuration. | ENABLE_WIFI is 1 and ENABLE_WEBSERVER is 1 |
| AUDIO_WEB_SOCKET_ENABLED | Enable the `/ws/audio` WebSocket, which streams audio telemetry (FFT band peaks, VU levels and beat detection state) to connected clients as JSON. | ENABLE_WIFI is 1 and ENABLE_WEBSERVER is 1 and ENABLE_AUDIO is 1 |

Example in platformio.ini (prefix the flags with `-D`, e.g. `ENABLE_WIFI=1` becomes `-DENABLE_WIFI=1`):

```INI
build_flags = -DENABLE_WIFI=1
```

Example in globals.h:

```C++
#define ENABLE_WIFI 1
```

## IR remote control

Builds with `ENABLE_REMOTE=1` listen for NEC-protocol IR codes on `IR_REMOTE_PIN` (default 25; most environments override it - see `platformio.ini`). Two of the common cheap LED remotes are supported: the 24-key black/color remote by default, and the 44-key white remote with DIY keys if you build with `-DREMOTE_KEY44=1`.

| Button | What it does |
| - | - |
| Power ON | Resumes output, clears any global color set from the remote, restarts effect rotation and sets brightness to maximum |
| Power OFF | On HUB75 builds, steps the brightness down. On strip builds, blanks every channel and suspends local effects - see the note below |
| Bright/Speed + / - | Next / previous effect, or clears the global color if one is active. Also resets the effect interval when the `RemoteEffectButtonsResetInterval` device setting is on |
| SMOOTH | Clears the global color and switches to the fast "smooth" effect rotation interval |
| STROBE / FLASH | Next / previous palette |
| FADE | Toggles the VU meter overlay |
| Color keys | Sets a global color, which is drawn as a fill over the current effect |

Note that Power OFF on strip builds is a local-effect power state, not a hardware cutoff: channels that are being fed frames by an external sender (LED Central) keep drawing right over the blanked buffer, so a remotely driven strip lights back up even while "off". The state is held in memory only, so a reboot comes back on, and it is not currently exposed through the web UI or REST API - only the remote can toggle it.

## Adding new effects

To add new effects, you:

1. Create a header for your effect in `include/effects/strip/` (or `include/effects/matrix/` for HUB75 effects), and derive from `EffectWithId<YourEffect>` - the CRTP base - or from an existing effect class. The good stuff happens in the only important function, `Draw()`.
Check out what the built-in effects do, but in short you're basically drawing into an array of CRGB objects that each represent a 24-bit color triplet. Once you're done, the CRGB array is sent to the LEDs. The draw loop calls `Draw()` at the rate your effect asks for by overriding `DesiredFramesPerSecond()` (30 by default), so there is no need to `delay()` inside it. Draw each channel at its own size (`GetLEDCount()` on that channel's GFX object): in individual-strips mode the channels can have different lengths, and writing channel 0's length into a shorter channel overruns its buffer.
2. You do not need to pick an effect number. `EffectWithId<YourEffect>` derives a stable `EffectId` automatically by hashing your class's type name at compile time (see `_effect_id_detail::token_id_for_type` in `include/ledstripeffect.h`), so IDs can't collide by accident and there is nothing to register in `effects.h`. That file now only holds the shared JSON property-name defines.
3. Register your class in the `LoadEffectFactories()` function in `src/effects.cpp`, inside the `EFFECTS_*` effect-set block(s) that should include it. Registration uses `RegisterAll()` with one `Effect<YourEffect>(...)` entry per instance; anything you pass is forwarded to the effect's constructor:

    ```C++
    #if defined(EFFECTS_SIMPLE)
        RegisterAll(*g_ptrEffectFactories,
            Effect<FireEffect>("Medium Fire", NUM_LEDS, 1, 3, 100, 3, 4, true, true),
            Effect<YourEffect>(/* constructor arguments */)
        );
    #endif
    ```

    Wrap an entry in `Disabled(...)` if it should be registered but not enabled by default. Which `EFFECTS_*` set a given build gets is decided by the `-DEFFECTS_...` flag in that environment's `build_src_flags` in `platformio.ini`.

There is a global `EffectManager` instance that creates the effect table from a JSON file on SPIFFS, if one is present and was written for the same effect set; if the registered effects have changed (new firmware with a different list), the saved table is discarded and the defaults from `LoadEffectFactories()` are used. It then rotates amongst the enabled effects at a rate controlled by `DEFAULT_EFFECT_INTERVAL`, skipping manual-only effects (those whose `IsManualOnly()` returns true, like Length Test). Effects get a `Start()` call when they become active and are then asked to draw when needed.

Each channel of LEDs has its own `GFXBase`-derived instance (a `WS281xGFX` for WS281x strips, `HUB75GFX` for matrices). Channel 0 is the one associated with `LED_PIN0`, and so on. Inside an effect you reach them through the effect manager: `g_ptrSystem->GetEffectManager().g(0)` is channel 0's graphics object, its pixel buffer is the public `leds` member, and it holds `GetLEDCount()` pixels. You can draw into the buffer without ever touching the raw bytes by calling `fill_solid`, `fill_rainbow`, `setPixel`, and other drawing functions.

The `DEMO` effect set, which the XIAO builds in this fork use, registers about twenty strip effects: fires, bouncing balls, meteors, twinkling stars, palettes, white light, the LED-Central ports and Length Test (see [EFFECTS.md](EFFECTS.md)). The simplest set, `MINIMAL`, is just a status indicator and a rainbow fill.

The `DEMO` configuration is provided by a number of board specific build environments. The list of such environments can be seen by running 'python3 tools/show_envs.py', which would tell the reader that, as of this writing, hardware specific variations of 'DEMO' include:

- demo
- m5demo
- m5plusdemo
- heltecdemo
- heltecv2demo
- heltecv3demo
- lilygo-tdisplay-s3-demo

These [build types](#build-pointers) may be chosen by the '-e' argument
to pio or in a menu option inside the PlatformIO IDE/VS Code.

## Notes on JSON persistence

The effects table is persisted to a JSON file on SPIFFS at regular intervals, to retain the state of effects (and in fact the whole effect list) across reboots. The device web application uses this to let you enable, disable, reorder, copy, rename and configure effects (see [Device web UI and API](#device-web-ui-and-api)).

This makes that an override of `SerializeToJSON()` and a corresponding deserializing constructor must be provided for effects that need (or want) to persist more than the properties that are (de)serialized from/to JSON by `EffectWithId` (which derives from `LEDStripEffect`) by default. Don't reuse the base class's keys for your own properties: `LEDStripEffect` writes `en`, `ce`, `fn`, `es` (enabled), `mt`, `mi`, `bo` and `fo`, and a clash silently corrupts that property on the next boot (this fork's Halloween effect once saved its eye size as `es` and came back disabled after every reboot).

Throughout the project, the library used for JSON handling and (de)serialization is [ArduinoJson](https://arduinojson.org/). Among others, this means that `SerializeToJSON()` functions _must_ return `true` _except_ when an ArduinoJson function (like `JsonObject::set()`) returns `false` to indicate it ran out of buffer memory.

To get a better understanding of the specifics related to JSON (de)serialization, you could consider taking a look at the respective tutorials in the ["First contact" section](https://arduinojson.org/v7/doc/#first-contact) of the ArduinoJson documentation.

## Resetting the effect list

For instance during development, the (JSON-persisted) effect list on your board can get out of sync with the effects you add in effects.cpp (in the function `LoadEffectFactories()` specifically) to a point it becomes messy or annoying. If this happens, you can reset the effect list on the board to the default, via the network. For this to work, the board has to be connected to WiFi and the webserver has to be running.

The reset can be done by performing an HTTP form POST to http://&lt;device_IP&gt;/reset with the following fields set: effectsConfig=1 and board=1. On systems with "regular" curl available, the following command should do the trick:

```shell
curl -d "effectsConfig=1&board=1" -X POST http://<device_IP>/reset
```

The web UI's **Reset Effect Config** button does the same.

Furthermore, it's also possible to "ignore" the persisted effect list altogether and always load the standard effects list at startup. Documentation on how to do this is available towards the top of the aforementioned `LoadEffectFactories()` function.

## Fetching things from the Internet

If you develop an effect that requires data to be pulled in from the Internet then you can register a network reader function with the `NetworkReader` class, which is available via the `g_ptrSystem->NetworkReader()` global reference. You can use either the `PatternSubscribers` or `PatternWeather` effects as sources of inspiration.  PatternStocks pulls live (15-min delay) stock quotes from a private server.

## Build pointers

### Build tools

The project can be built using [PlatformIO](https://platformio.org/). There's a [PlatformIO IDE](https://platformio.org/platformio-ide) available, which is built on top of Visual Studio Code. Included in it are the command-line [PlatformIO Core](https://platformio.org/install/cli) tools. They can also be installed on their own if you prefer not using the IDE.

PlatformIO is the only prerequisite. The web UI is plain HTML/CSS/JavaScript with no bundler, so **NodeJS and NPM are not needed to build the firmware**. A pre-build hook (`tools/bake_site.py`) simply gzips `site/index.html`, `site/styles.css` and `site/app.js` into `site/dist/`, and those compressed files are embedded in flash. That hook - like the other build scripts in `tools/` - runs on the Python interpreter PlatformIO already ships with, so there is nothing extra to install.

NodeJS is only useful if you want to lint the front-end while editing it (`npx eslint site/app.js`); it plays no part in a normal build.

For details on working with the front-end application see [site/README.md](./site/README.md).

### Build commands

When either the IDE or Core are installed, NightDriverStrip can be built from a command shell by entering the project/repository directory and issuing the following command:

```ShellConsole
pio run -e demo
```

This will build the `demo` config.

> If you get an error that `pio` is not found, you might need to [add it to your path](https://docs.platformio.org/en/stable/core/installation/shell-commands.html).

To see the full list of build environments (there are around sixty), use:

```ShellConsole
python tools/show_envs.py
```

Note that running `pio run` **without** `-e` does *not* build everything: `platformio.ini` sets `default_envs` in its `[platformio]` section, and that single environment is what gets built. Change `default_envs` if you'd rather have a different project be your default, or always pass `-e`.

To actually build every available configuration (this takes a while; upstream's README lists build times on various machines), use:

```ShellConsole
python tools/build_all.py
```

Uploading and monitoring work per environment as usual:

```ShellConsole
pio run -e demo -t upload
pio device monitor
```

You can keep your own board and environment definitions out of the repository by putting them in a `custom_*.ini` file in the project root; `platformio.ini` pulls those in automatically and they are gitignored.

### Optional audit policy

Builds run coding-standard audits from `tools/pio_audit.py` in soft-fail mode by default, meaning violations are reported but do not fail the build.

To enable strict (hard-fail) mode locally:

1. Copy `config/audit.example.ini` to `config/audit.ini`
2. Set `[audit] hard_fail = true` in `config/audit.ini`

`config/audit.ini` is intentionally gitignored so this remains a local/CI policy choice.

## File system

To build and upload the file system that can be used by effects (although currently none do), you will need to build and upload the SPIFFS image to your board's flash using PlatformIO. You can do this using the platformio user interface, or using the `pio` command line tool:

```ShellConsole
pio run --target buildfs --environment <project name>
pio run --target uploadfs --environment <project name>
```

## Tools

This repository includes a number of scrips to perform various tasks during or after building projects. They are included in the [`tools`](tools) directory. Please note that the scripts expect to be started from the project's main directory. So, using:

```shell
tools/buddybuild.sh
```

Instead of:

```shell
cd tools
./buddybuild.sh
```

## COM port problems

Specifically on the Windows platform, there have been cases where people have experienced problems communicating to the serial port that the ESP32 device is connected to from PlatformIO, usually in the shape of PlatformIO showing messages that the COM port in question doesn't exist where it absolutely did before. This tends to be caused by the COM port being used by another process within PlatformIO (the monitor function being a known cause), or some other application effectively blocking the COM port in question. A confirmed example of the latter is Malwarebytes.

If you experience such difficulties accessing serial ports, the tools in the Sysinternals toolkit (particularly Process Explorer and Portmon, available [from Microsoft](https://learn.microsoft.com/en-us/sysinternals/)) may be helpful towards finding the culprit.

## Bonus exercise

Write something simple to send color data to the socket. The format is very basic: which channel, how many LEDs you're drawing, when to draw it, and the color data itself. You can send uncompressed data with a zero timestamp as long as you send the correct header before your data, which is very simple. Data with a zero timestamp will just be drawn immediately with no buffering.

| BYTES   | FUNCTION  |                                                                               |
| ------- | --------- | ----------------------------------------------------------------------------- |
| 0, 1    | CommandID | _(Set it to 3, which is `WIFI_COMMAND_PIXELDATA64`)_                          |
| 2, 3    | ChannelID | _(Set it to 1 for single channel, though 0 works too for historical reasons)_ |
| 4 - 7   | Length    | _(Number of 24-bit `PIXELS` being set)_                                       |
| 8 - 15  | Seconds   | _(Set it to 0)_                                                               |
| 16 - 23 | Micros    | _(Set it to 0)_                                                               |
| 24+     | RGB       | _(24-bit RGB color data, one per `PIXEL` specified in Length above)_          |

If built with `ENABLE_WIFI` and `INCOMING_WIFI_ENABLED`, if the chip is able to get a WiFi connection and DHCP address it will open a socket on port 49152 and wait for packets formed as described above.

## Super Bonus Exercise

Generate a series of 24 frames per second (or 30 if under 500 LEDs) and set the timestamp to "Now" plus 1/2 a second. Send them to the chip over WiFi and they will be drawn 1/2 second from now in a steady stream as the timestamps you gave each packet come due.

## Sample parts (Plummer's Software LLC Amazon affiliate links)

_This section and the next are Dave Plummer's, from the upstream README._

- BTF-Lighting WS2812B Strip, 144 pixels per meter, white: [Amazon.com](https://amzn.to/3CtZW2g)
- BTF-Lighting WS2812B Strip, 144 pixels per meter, black: [Amazon.com](https://amzn.to/39ljqcO)
- MakerFocus ESP32 Module with 8M Flash (not PSRAM) and built-in blue OLED: [Amazon](https://amzn.to/3ApdF9H)
- M5StickCPlus ESP32 Module with built-in LCD, Mic, buttons, battery, PSRAM, and more: [Amazon.com](https://amzn.to/3CrvCFh)
- 16x16 LED Matrix panels (3 per Spectrum Analyzer) [Amazon.com](https://amzn.to/3ABs5DK)
- Infinity Mirror for use with the MAGICMIRROR config: [Amazon.com](https://amzn.to/3lEZo2D)
- Super-handy breakout board for ESP32-DevKitC: [Amazon.com](https://amzn.to/3nKX7Wt)

Full Disclosure: As an Amazon Associate, PlummersSoftwareLLC earns commission from qualifying purchases. It's not added to the purchase price, and does not increase your cost at all. Plus, all 2021 profits from the Dave's Garage Channel, which includes these sales, will go to the UW Autism Center.

## Contributing, and the BlinkenPerBit metric

Rather than produce a complex set of guidelines, here's what I hope open-source collaboration will bring to the project: that folks will add important features and fix defects and shortcomings in the code. When they're adding features, they'll do it in a way consistent with the way things are done in the existing code. They resist the urge to rearchitect and rewrite everything in their own image and instead put their efforts towards maximizing functional improvement while reducing source code thrash and change.

Let's consider the inconsistent naming, which should be fixed. Some is camelCase, some is pszHungarian, and so on, depending on the source. I'd prefer it were all updated to a single standard TBD. Until the TBD is determined, I lean towards [the Win32 standard](https://docs.microsoft.com/en-us/windows/win32/stg/coding-style-conventions?redirectedfrom=MSDN).

When working in a function, work in the style of the function. When working on a class, work in the style of the class. When working on a file, work in the style of the file. If those are inconsistent, do whatever minimizes changes. Stylistic changes should only be introduced after discussion in the group, and generally should entain owning that style change across the entire project.

Next, let's consider `#define`s to control the build. There may be better and more elegant ways of doing things. There could be entire configuration platforms. But I'd prefer to keep it simple. And I define simplest to be "the least that an experienced C++ programmer needs to learn before being constructive with the code in question". I don't want to learn a new class library if I can avoid it!

A lifetime of coding has taught me to err on the side of simplicity, so please don't introduce variadic template constructs unless they demonstrably shrink the source code. Anything that grows the complexity AND length of the code should be suspect.

Add whatever you want and/or need to make your LED dreams come true. Fix my blunders. Fill in the obvious gaps in my knowledge. Whatever has the most blinken for the fewest bits get my vote. You only get so much additional cool blinken for every byte of code and program. That return is measured in BlinkenPerBit, the amount of blinking awesomeness the code adds divided by the impact on the source (and binary).
