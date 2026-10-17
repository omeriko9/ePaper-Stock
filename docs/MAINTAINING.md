# Maintainer notes

This document tells how the firmware saves power, how a release works, and how the web installer works. It also lists the hardware facts that are not easy to see in the schematic.

## 1. Repository history

The history of this repository starts on 2026-10-17 with one clean commit. The older history had an API token in it, so it is not public.

Do not push branches from an older clone of this project into this repository.

## 2. Hardware target

The firmware supports the two versions of the **Waveshare ESP32-S3-ePaper-1.54** board:

| Item | V1 | V2 |
|---|---|---|
| Chip | ESP32-S3FH4R2 (QFN56) | ESP32-S3-PICO-1-N8R8 |
| Flash | 4 MB | 8 MB |
| PSRAM | 2 MB, **quad** | 8 MB, **octal** |
| Build | `sdkconfig.defaults` | `sdkconfig.defaults` + `boards/v2.defaults` |

Both versions have the same pins (the Waveshare V1 and V2 examples have the same `user_config.h`), and the same parts: a 200 x 200 SSD1681 display, a PCF85063 RTC at I2C 0x51, an SHTC3 at 0x70 (not used) and an ES8311 codec with an amplifier. The only config difference is `CONFIG_SPIRAM_MODE_OCT` for V2. A V2 board has "V2" on the back or in the top-left corner of the PCB.

The V2 build keeps the 4 MB flash layout. Thus both builds have the same partition table, and an update from an older firmware keeps NVS on both boards.

The hardware facts below were found on a V1 board. Waveshare says that V2 has a lower sleep current, so the power circuit of V2 can be different. The latch fault detector (3.2) also covers V2.

### Hardware facts that cost debug time

- **Battery latch (GPIO17).** GPIO17 drives T2 (1 kΩ base resistor, 10 kΩ to GND), and T2 holds the battery switch on. A strong high level costs about 2.5 mA. The internal pull-up costs about 0.06 mA.
- **The codec is on the I2C bus.** The ES8311 shares GPIO47 (SDA) and GPIO48 (SCL) with the RTC. When its power rail is off, the codec pulls both lines low. Then all I2C transfers fail, and the 4.7 kΩ pull-ups leak about 1 mA. So the audio rail must stay on, also in deep sleep.
- **GPIO42 (audio rail, low = on)** starts in its JTAG function (MTMS). Call `esp_rom_gpio_pad_select_gpio(42)` before you drive it.
- **GPIO6 (display power)** and GPIO42 have external pull-ups to 3V3. A floating pin means "off".
- **SNI.** Do not set `skip_cert_common_name_check` in `esp_http_client`. With this flag, esp-tls sends no SNI, and `api.open-meteo.com` then closes the connection (`ESP_ERR_HTTP_FETCH_HEADER`).
- **USB in deep sleep.** In deep sleep the USB Serial/JTAG port does not exist. To flash the device, wake it with the BOOT button first. In web server mode the device stays awake.

## 3. Power design

The device wakes on a timer, updates the screen, and goes back to deep sleep. The intervals are settings (for example 5 minutes when the market is open, 3 hours when it is closed). Thus the sleep current sets most of the battery life.

### 3.1 Deep sleep (`components/user_app/power_mgmt.cpp`)

`power_enter_deep_sleep()` is the only path into deep sleep. It does these steps:

1. It isolates the display, SPI and other unused pads (`k_isolate_in_sleep`).
2. It pulls down the floating pads 14, 15 (RTC pads) and 38, 45, 46 (digital pads, held).
3. It holds GPIO42 low, so the audio rail stays on and the codec does not pull the I2C bus low.
4. It sets the battery latch (GPIO17) to the weak pull-up, or to a strong high level (see 3.2).
5. It enables the timer wake and the BOOT button wake (ext1, GPIO0 low).

At a cold boot, `power_quiet_i2c_peripherals()` puts the SHTC3 in sleep (command 0xB098) and stops the RTC clock output (COF = 111).

Estimated sleep current, not measured:

| Item | Old firmware | Now |
|---|---|---|
| I2C pull-ups into the codec | about 1 mA | 0 |
| Battery latch | 0.06 or 2.5 mA | 0.06 mA |
| SHTC3 idle | about 45 µA | less than 1 µA |
| Display and other pads | 0.1 to 0.2 mA | about 0 |
| ESP32-S3, battery divider, regulator | about 30 µA | about 30 µA |

### 3.2 Latch fault detector

Maybe the weak pull-up cannot hold the battery switch on all boards. If it cannot, the board turns off in deep sleep. The firmware finds this and changes to the strong mode:

1. At a cold boot in pull-up mode, `power_latch_check_on_cold_boot()` writes the marker 0xA5 to the RAM byte of the PCF85063 (register 0x03). The RTC chip keeps this byte while the ESP32-S3 has no power.
2. Deep sleep wakes do not read or change the marker.
3. A power-on reset with the marker set, in pull-up mode, and with the battery above 3.5 V, means that the board turned off. The firmware then saves `LATCH_STRONG` in NVS (namespace `power`, key `latch`), uses it from then on, and writes the marker to 0.
4. If the battery is at 3.5 V or less, the firmware keeps the pull-up mode, because an empty battery can also cause the power-on.
5. If the RTC chip does not answer, or the firmware cannot write the marker, the firmware uses the strong mode.

For the user: if the screen stops changing, push PWR one time.

### 3.3 Short awake time

Measured on the V1 board:

| Wake type | Awake time |
|---|---|
| No network | about 1.1 s (screen refresh starts at about 0.5 s) |
| WiFi and weather | about 3 s (WiFi connect about 1.1 s) |

The parts that make the wake short:

- **No fixed delays** in the wake path.
- **Partial refresh.** The last frame is kept in RTC memory (`s_shown_frame`). The driver writes the old frame to RAM 0x26 and the new frame to RAM 0x24. After 12 partial refreshes, a full refresh clears the ghost image. If the frame did not change, the screen does not refresh.
- **Font in flash.** A 19 KB subset of NotoSans (`components/user_app/fonts/NotoSans-Subset.ttf`) is embedded, and FreeType reads it in place.
- **Fast WiFi.** The BSSID and the channel of the access point are kept in RTC memory. The first try uses them, with a 4 s limit. The total limit is 10 s.
- **Fewer requests.** The weather is fetched only when the screen shows it (market closed), at most every 15 minutes. After a failure, the next try is after 15 minutes. The market open and close times are fetched only when the cached value expires.
- **Audio** starts only when a sound plays (`AudioPlayer::play()` calls `init()`). Sounds play only on battery power: `Utilities::is_on_battery_power()` reads VBAT, and 4.1 V or more means USB. The AudioPlayer mutex is recursive, because `play()` holds it when it calls `init()`. With a normal mutex, the first sound after a wake blocks the task forever.
- **Logs** go to USB Serial/JTAG only. The bootloader log level is WARN.

## 4. Build

- ESP-IDF **v5.5**, target `esp32s3`. The Arduino component and the other dependencies come from the component manager.
- `sdkconfig` is not in git. `sdkconfig.defaults` is the source of the config.
- `dependencies.lock` pins the component versions. Be careful when you change `main/idf_component.yml`. A change in the manifest makes the component manager solve the versions again, and newer versions can break the build. For example, `esp_codec_dev` 1.5 renamed the `ES7210_SEL_MIC*` constants.
- The project uses `-flto`. The LTO link step fails if the project path has spaces in it. Use a path without spaces.
- To flash by hand without loss of the settings, write only the files in `flash-command.txt` of a release. Do not erase the chip.

## 5. Release

To make a release:

1. Commit and push the changes to `main`.
2. Push a tag: `git tag v1.1.0 && git push origin v1.1.0`.

The tag starts `.github/workflows/release.yml`:

1. **build** writes `version.txt` from the tag and builds two times with `espressif/esp-idf-ci-action` (IDF v5.5): V1 in `build/`, and V2 in `build_v2/` with the `boards/v2.defaults` overlay. A check step makes sure that V1 has quad PSRAM and V2 has octal PSRAM.
2. `tools/package_firmware.py` copies the images to `dist/` as `<name>-v1.bin` and `<name>-v2.bin`. It writes `manifest.json` (for each board: the flash size, the PSRAM size, and the offset, size and SHA-256 of each image) and `flash-command.txt`.
3. **release** makes the GitHub Release and attaches the files.
4. **pages** calls `.github/workflows/pages.yml`. It copies `web/` and the images of the latest release to the Pages site.

A push to `main` that changes `web/` also starts `pages.yml`, so a page change does not need a new release.

After a release, make sure that the live manifest has the new version:

```
curl -s https://omeriko9.github.io/ePaper-Stock/firmware/manifest.json | grep '"version"'
```

Two facts cause problems here:
- A push to `main` with page changes deploys the new page with the images of the **previous** release.
- On 2026-10-17, GitHub Pages kept the earlier deploy after the tag deploy reported "success".

If the manifest is old 10 minutes after the release, run `gh workflow run pages.yml --ref main`. `selectBoard()` also accepts the v1.0.x manifest format, so a mismatch gives a clear message and not a crash.

Setup on GitHub (done one time):

- **Settings → Pages → Source:** "GitHub Actions".
- **Settings → Environments → github-pages:** a deployment rule for the tags `v*`. Without this rule, the Pages deploy from a tag fails.

The page cannot download release assets from GitHub (CORS), so the Pages site keeps its own copy in `firmware/`.

## 6. Web installer

The installer is at https://omeriko9.github.io/ePaper-Stock/. The files are:

| File | Function |
|---|---|
| `web/index.html` | The page and the steps for the user |
| `web/flasher.js` | Board check, download, write and reset |
| `web/vendor/esptool-js/esptool-0.7.0.js` | esptool-js 0.7.0, a local copy (Apache-2.0) |

The installer does these steps:

1. It reads `firmware/manifest.json`.
2. The user selects the port (Web Serial; Chrome and Edge only).
3. It loads the esptool stub and reads the chip, the flash size and the PSRAM size.
4. `selectBoard()` selects the build in the manifest with the same flash size (4 MB = V1, 8 MB = V2). It stops if the chip is not an ESP32-S3, if no build has that flash size, or if the PSRAM size in eFuse does not agree with the board.
5. It downloads the images and checks their SHA-256.
6. It compares the partition table on the device with the new one. If they are different, the user must accept that the settings can be lost.
7. It writes 0x0, 0x8000, 0x10000, 0x210000 and 0x350000 only. NVS (0x9000) does not change. esptool-js checks the MD5 of each image.
8. It resets the chip.

**Reset quirk.** `ESPLoader.after("hard_reset")` in esptool-js only releases RTS and does not pulse it, so the chip stays in download mode. `Device.reset()` in `flasher.js` does the same steps as esptool.py: DTR low, then RTS high for 200 ms, then RTS low.

### Test the installer without a browser

`tools/flasher-test/` runs `web/flasher.js` in Node, with the `serialport` package as Web Serial:

```
cd tools/flasher-test
npm install
python -m http.server 8765 --directory <site folder>      # web/ plus firmware/
node test.mjs COM28 check     # read the board, change nothing
node test.mjs COM28 flash     # full update
```

The site folder is a copy of `web/` with the files of a release (or of `tools/package_firmware.py`) in `firmware/`. Wake the device with BOOT before the test.
