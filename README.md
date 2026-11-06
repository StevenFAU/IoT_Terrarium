# IoT Terrarium

An off-grid terrarium monitor built from two **Heltec WiFi LoRa 32 (V2)** boards and an **ESP32-CAM**. The board at the
terrarium reads humidity, temperature, light, hydrogen and water, takes photos, and sends everything over **LoRa
(915 MHz)** to a receiver up to ~500 ft away inside the building. The receiver hosts a **dashboard** on its own WiFi
network, so a phone or laptop can check on the terrarium with no router, campus network or internet.

Group 8 · Embedded Systems · Steven Cohen, Seth Neptune, Gary Jr. Marroquin Irizarry

**Try the dashboard without the hardware:** [stevenfau.github.io/IoT_Terrarium](https://stevenfau.github.io/IoT_Terrarium/)
(demo mode, simulated data).

```
 Sensors ──wires──▶ Heltec TX ◀──LoRa 915 MHz──▶ Heltec RX ──USB (optional)──▶ computer: save_images.py
 ESP32-CAM ─serial─┘                                 │
     │                                               └── WiFi "Terrarium" ──▶ phone / laptop: dashboard
     └── WiFi "Terrarium-Cam" (near the terrarium) ──▶ phone: live video
```

## Dashboard

Join the **Terrarium** WiFi network and open **http://192.168.4.1**. It works the same on a phone and a desktop.

- **Overview** – live readings with in-range / low / high status and 2-hour sparklines, alerts (too dry, too hot,
  low water, hydrogen detected, sensor or link failures), LoRa signal quality, and the latest photo with
  **Take photo now** and a progress bar while it travels over LoRa.
- **History** – 1 h / 6 h / 24 h charts for every sensor with the target range shaded, a cursor that reads out the
  same minute across all charts, and **Export CSV**.
- **Camera** – the last 40 photos with timestamps, a full-screen viewer and downloads.
- **Settings** – terrarium name, °C / °F and the alert ranges. Saved on the board, so every viewer sees the same
  settings. Also shows the board's uptime, storage and clock.

The board keeps one averaged reading per minute for 24 hours in memory (it starts over if the board restarts) and
the last 40 photos and the settings in flash. It has no clock of its own: the first viewer's device sets it.

For live video and full-size photos, stand near the terrarium, join **Terrarium-Cam** and open 192.168.4.1.

## Hardware

| Sensor | Pins on the TX board | Notes |
|---|---|---|
| DHT11 humidity/temperature | + → 3V3, OUT → GPIO17, − → GND | module has its own pull-up |
| Photoresistor | 3V3 → LDR → GPIO33, pull-down GPIO33 → GND | more light = higher voltage |
| MQ-8 hydrogen | VCC → 5V, AO → 10 kΩ → GPIO32, 20 kΩ GPIO32 → GND | divider keeps the 5 V output under 3.3 V |
| Water sensor | + → 3V3, S → GPIO38, − → GND | calibrated 150 mV dry, 1,450 mV submerged |
| ESP32-CAM | IO14 → GPIO23, IO15 ← GPIO22, GND, 5V | serial link at 115200 baud; leave IO0 unconnected |

Pins to avoid on the V2: 16 (OLED reset), 4/15 (OLED), 5/14/18/19/26/27/34/35 (LoRa), 25 (LED), 21 (Vext), and any
ADC2 pin for analog readings (they stop working when WiFi is on).

## Repository

| Path | What it is |
|---|---|
| `terrarium_sender/` | TX board: reads sensors, sends readings every 10 s, relays a photo every 5 min or on request |
| `terrarium_receiver/` | RX board: receives over LoRa, stores history and photos, serves the dashboard and its API |
| `terrarium_cam/` | ESP32-CAM: photos for LoRa, plus its own WiFi with live video |
| `dashboard/` | Dashboard source: plain HTML, CSS and JavaScript, no build tools or libraries |
| `tools/build_dashboard.py` | Packs `dashboard/` into `terrarium_receiver/web_assets.h` |
| `save_images.py` | Saves photos from the RX board's USB port as .jpg files |
| `board_test/` | Hardware self-test for a Heltec V2 |

## Setting up

1. **Arduino IDE 2.3.10.** In File → Preferences → *Additional boards manager URLs*, add both:
   ```
   https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/3.3.8/package_heltec_esp32_index.json
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. **Boards Manager:** install *Heltec ESP32 Series* 3.3.8 (for the two Heltecs) and *esp32* 3.3.12 (for the CAM).
3. **Library Manager:** install *Heltec ESP32 Dev-Boards* **1.1.5** (pick it from the version list; 2.x doesn't compile
   for the V2, so decline update prompts) and *DHT sensor library*.
4. **WiFi passwords:** in `terrarium_receiver/` and `terrarium_cam/`, copy `wifi_secrets.example.h` to
   `wifi_secrets.h` and set a password of 8–63 characters. `wifi_secrets.h` is git-ignored.
5. **Upload:** board *Heltec WiFi LoRa 32(V2)* for the sender and receiver; *AI Thinker ESP32-CAM* for the camera
   (flashed on its ESP32-CAM-MB base board).

`LORA_TX_POWER` is 2 dBm in both Heltec sketches for desk testing, because boards inches apart overload each other.
**Set it to 17 before spreading the boards out.**

## Working on the dashboard

The dashboard runs in **demo mode** on localhost, from a file, or on GitHub Pages, so you can work on it without any
boards:

```bash
python3 -m http.server 8000 --directory dashboard
```

Then open http://localhost:8000. To try your local copy against a real board, join the Terrarium WiFi and open
http://localhost:8000/?board=192.168.4.1.

After changing anything in `dashboard/`, pack it into the firmware and re-upload the receiver:

```bash
python3 tools/build_dashboard.py
```

### Receiver API

| Request | Returns |
|---|---|
| `GET /api/status` | current reading, link quality, photo progress, storage |
| `GET /api/history?after=T` | one row per minute, `[uptime, humidity, temperature, light, water, h2, rssi]`, after uptime `T` |
| `GET /api/photos` | saved photos with their timestamps |
| `GET /photo.jpg?n=N` | one photo |
| `POST /api/snap` | ask the TX board for a photo now |
| `GET` / `POST /api/settings` | dashboard settings (JSON) |
| `POST /api/time` | set the board's clock (Unix seconds) |

The GET requests also work over the receiver's USB serial port: send `API /api/status` and it answers with one line,
`APIRESULT {...}`.

## Saving photos on a computer

Plug the RX board into a computer and run:

```bash
python3 save_images.py /dev/ttyUSB0
```

It saves each photo that arrives into `images/` and prints the sensor readings in between. Requires `pyserial`.
