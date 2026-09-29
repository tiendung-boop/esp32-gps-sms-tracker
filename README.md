# ESP32 GPS Tracking over WiFi and SMS

An end-to-end **GPS tracking system**: ESP32 field devices read their position from a NEO-6M GPS and report it either **over the internet (HTTP, HMAC-signed)** or **over SMS through a 4G module when there is no WiFi or internet**. Browser dashboards and a small Node.js server plot every device on a live map. The dashboards work **fully offline** with embedded vector map tiles.

```
            ┌──────────── WiFi / Internet path ────────────┐
[ESP32 + GPS] --HTTP GET /api/pos (HMAC-SHA256)--> [vi-tri-server (Node.js)] --WebSocket--> [Dashboard "thu"]
 GpsPosSender                                           ▲
                                                        └── [Phone browser "phat" beacon] (WebSocket)

            ┌──────────── SMS path (no internet) ─────────────┐
[ESP32 + GPS + A7680C 4G] --SMS "Toa do: lat,lon;..."--> [ESP32 + A7680C] --USB serial--> [Node.js reader] --> [Offline map page]
 GpsSmsSender                                             SmsReceiverUSB                  nhan-sms-tu-dong-*
```

## Firmware (`*/…ino`, Arduino / ESP32)

| Sketch | Description |
|---|---|
| `GpsPosSender` | Reads GPS (TinyGPSPlus) and sends the position to the server over HTTPS every 5 s. Requests are **HMAC-SHA256 signed** with a shared secret so a guessed room code can't be used to inject fake positions |
| `GpsSmsSender` | GPS → batches the samples taken each minute into one SMS (`Toa do: lat1,lon1;lat2,lon2;…`) → sends it through a SIMCom **A7680C** 4G module using raw AT commands |
| `SmsReceiver` | Receives those SMS on a second A7680C, parses the coordinates and forwards them to the server over WiFi |
| `SmsReceiverUSB` / `SmsReceiverUSB38` | Receiver with no WiFi: prints `TOADO\|deviceId\|name\|lat\|lon` over USB for the PC to read |
| `SmsSender` | Minimal SMS sender (AT command driver written from scratch, no library) |
| `ATBridge` | Transparent Serial ↔ modem bridge for debugging AT commands by hand |
| `GPS_NEO_6M_V2_OLED_0.91` | GPS fix shown on a 128×32 SSD1306 OLED |
| `GpsWifiTest`, `WifiWebTest`, `WifiApTest`, `Blink`, `BlinkTest38` | Bring-up tests for the board, WiFi STA/AP and the local web server |

**Secrets** (WiFi credentials, API secret, destination phone number) live in `secrets.h`, which is git-ignored. Copy `secrets.h.example` → `secrets.h` in each sketch folder and fill in your own values.

## Server & web tools (`lam_viec/`)

| File | Description |
|---|---|
| `vi-tri-server/server.js` | Node.js relay (HTTP + WebSocket `ws`) that groups devices into **rooms** by room code. It accepts positions from browsers (WebSocket) and from microcontrollers (`GET/POST /api/pos` with HMAC verification), keeps state in RAM only, and serves offline `.pmtiles` map tiles |
| `thu-vi-tri-tool.html` | Multi-device live dashboard (Leaflet + marker clustering) |
| `phat-vi-tri-tool.html` | Turns any phone browser into a GPS beacon |
| `nhan-sms-tu-dong-server.js` + `nhan-sms-tu-dong-tool.html` | Automatic SMS intake from the USB receiver (`serialport`) or an Android phone over ADB, plotted on an **offline** map with one-way-street-aware routing |
| `doc-toa-do-sms-tool.html` | Paste or read SMS text, extract the coordinates and show them on the map |
| `gui-dinh-vi-sms-tool.html` | Periodically sends your own location by SMS |
| `shortest-path-tool.html` | Offline shortest-path routing (Dijkstra) on GeoJSON road data |
| `dinh-vi-tool.html`, `dinh-vi-ggmap-tool.html` | OpenStreetMap place search and geolocation, with a hand-off to Google Maps |
| `thong-tin-thiet-bi-tool.html` | Device registry (phone/ID → display name) |

Offline map data for Da Nang (`*.pmtiles`, road and tile bundles of 26–150 MB) is **not committed** because of its size. `lam_viec/HUONG_DAN.txt` explains how to rebuild it with the `pmtiles` CLI from Protomaps builds and OpenStreetMap/Overpass.

## Getting started

```bash
# Server
cd lam_viec/vi-tri-server
npm install
API_SECRET=<same value as in secrets.h> node server.js
#   dashboard: http://localhost:8877/thu    beacon: http://localhost:8877/phat
```

Firmware: Arduino IDE + the ESP32 core, plus the libraries **TinyGPSPlus**, **Adafruit SSD1306 / GFX** (OLED sketch only). Wiring and the full walkthroughs are in the Vietnamese guides:

- [`HUONG_DAN.txt`](HUONG_DAN.txt): quick start for the WiFi tracking path
- [`lam_viec/HUONG_DAN.txt`](lam_viec/HUONG_DAN.txt): the full manual (all tools, HMAC setup, HTTPS/tunnel deployment, offline maps)
- [`Wyse 5070/HUONG_DAN_SU_DUNG.txt`](Wyse%205070/HUONG_DAN_SU_DUNG.txt): deploying the SMS-only system on a Dell Wyse thin client (Windows 10 IoT)

## Tech stack

C++ (Arduino/ESP32), TinyGPSPlus, SIMCom AT commands, HMAC-SHA256 (mbedTLS), Node.js, WebSocket (`ws`), `serialport`, ADB, Leaflet, PMTiles / Protomaps, OpenStreetMap
