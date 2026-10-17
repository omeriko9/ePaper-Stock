# ePaper Stock Ticker

![estock](https://github.com/user-attachments/assets/1e4f0128-4e37-4afa-8466-f28dcfecbc61)

A WiFi stock ticker on a 1.54" e-paper display. It shows stock prices, the US market status and the weather. It runs on the [Waveshare ESP32-S3-ePaper-1.54](https://docs.waveshare.com/ESP32-S3-ePaper-1.54) board and works for weeks on its battery.

## Install or update (no tools needed)

Open **[omeriko9.github.io/ePaper-Stock](https://omeriko9.github.io/ePaper-Stock/)** in Google Chrome or Microsoft Edge on a computer, then follow the steps on the page. The page writes the latest release to the device through USB. It keeps the WiFi password, the stocks and the Finnhub token.

The firmware is for the board version with 4 MB flash and 2 MB PSRAM. The page reads the board data and stops if the board is a different version.

## Features

* **Stock prices** from [Finnhub](https://finnhub.io/), one stock or a carousel of more stocks.
* **Market status**: open, closed, pre-market or post-market.
* **Weather** from [Open-Meteo](https://open-meteo.com/).
* **Web interface** to set WiFi, the API token, stocks, sounds and sleep intervals.
* **Low power**: deep sleep between updates, partial screen refresh, a fast WiFi reconnect, and all unused parts of the board off.

## Setup

1. **Connect to WiFi.** At the first start, the device makes a WiFi access point. The screen shows its name. Connect to it and open `http://192.168.4.1`. Then enter the name and password of your WiFi network.
2. **Open the web interface.** Push the BOOT button one time. The screen shows the IP address of the device. Open this address in a browser on the same network.
3. **Set the device:**
   * **Finnhub token:** get a free API key from [finnhub.io](https://finnhub.io/).
   * **Location:** the latitude, longitude and time zone, for the weather and the clock.
   * **Stocks:** the symbols to show, for example `AAPL` or `MSFT`.

## Buttons

| Action | Result |
|---|---|
| BOOT, one push | Wake up and start (or stop) the web server |
| BOOT, two pushes | Restart |
| BOOT, long push | Deep sleep until the next BOOT push |
| PWR, one push | Power on (when the device is off) |

## Build from source

* ESP-IDF v5.5, target `esp32s3`, with the Arduino component (installed by the component manager).
* `idf.py build`, then `idf.py -p PORT flash`.
* The project uses `-flto`. A build in a folder path with spaces fails at the link step, so use a path without spaces.
* To make a release, push a tag such as `v1.2.0`. GitHub Actions builds the firmware, attaches the images to the release and updates the install page.

## License

[MIT License](LICENSE). The folders in `components/` that come from other projects (SensorLib, WiFiManager, codec_board, multi_button) keep their own licenses.
