# ESP32 Pool & Tank Gateway

One ESP32 that connects an **Astral Pool Viron eQuilibrium** salt chlorinator (over Bluetooth LE) and a **water tank level sensor** (4–20 mA pressure transducer on an ADS1115) to **Home Assistant** over MQTT, with a built-in web dashboard for setup and control.

Home Assistant discovers the two as **separate devices**:

- **Chlorinator `<name>`** – mode, pump speed, timers, chlorine output, statistics, diagnostics
- **Water Tank Monitor** – level height, volume, percent full, raw ADC, health

Current version: **v1.18**

---

## Features

### Pool chlorinator (BLE)
- Authenticates to the chlorinator with its 4-character access code and decrypts its state (AES-128, protocol ported from pychlorinator).
- Reads mode, pump speed, active timer, info messages (Low Salt, No Water Flow …), chlorine status, flags and clock.
- Control from Home Assistant: **Off / Auto / Manual**, **Low / Medium / High** pump speed, Pool / Spa, chlorine output setpoint.
- **Reads and writes all 4 pump timers** (start, stop, speed, enabled) from HA or the web page. Changes are merged and queued, written in one read-modify-write, then read back and checked.
- "Setting to …" feedback in HA until the chlorinator confirms a mode or speed change.
- Selecting **Manual** runs the pump at Timer 1's speed.
- Cell running hours, low-salt hours, cell reversal count and a Low Salt problem sensor.
- **App-friendly**: polls every 5 minutes by default and backs off if the official phone app looks connected, so both can be used. Commands from HA or the web page are still sent at once.

### Water tank
- Submersible pressure sensor read through an ADS1115 at 8 samples/s.
- Up to 32 samples per measurement, trimmed mean, plus exponential smoothing between readings with a step bypass so real fills/draws show up quickly.
- Converts to height (mm), volume (L) and percent full from your tank's geometry and calibration.
- Sensor power can be left always on or switched by a transistor between readings.

### Reliability
- Wi-Fi reconnect, then restart if offline for 10 minutes.
- Hardware task watchdog, loop-hang supervisor, stuck-BLE and stuck-MQTT watchdogs, low-memory restart.
- Diagnostics in HA: uptime, free heap, lowest free heap, Wi-Fi signal, Wi-Fi drops, last restart reason.
- OTA firmware updates.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32 board | Wemos LOLIN32 or any ESP32-WROOM dev board |
| ADS1115 ADC module | I²C |
| 4–20 mA submersible pressure transducer | Powered via a DC-DC boost converter (~14 V) |
| Shunt resistor | Turns the loop current into a voltage for the ADS1115 |
| Transistor (optional) | Switches sensor power from GPIO23 |
| Astral Pool Viron eQuilibrium chlorinator | Within BLE range of the ESP32 |

### Default wiring

| Signal | ESP32 GPIO |
|---|---|
| ADS1115 SDA | 21 |
| ADS1115 SCL | 22 |
| Sensor power transistor | 23 |
| Onboard LED (active low, 1 s heartbeat) | 5 |

The transducer signal goes to ADS1115 **A0** (A1 optional, for a differential reading – see `TANK_USE_DIFFERENTIAL`).

---

## Building

**Arduino IDE** with the ESP32 core.

1. Board: **Wemos LOLIN32** (or **ESP32 Dev Module**).
2. **Tools → Partition Scheme → "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"** – the sketch is too large for the default partition.
3. Install these libraries:
   - WiFiManager (tzapu)
   - PubSubClient (Nick O'Leary)
   - ArduinoJson (Benoit Blanchon) **v7**
   - NimBLE-Arduino (h2zero) **v2.x**
   - Adafruit ADS1X15
   
   LittleFS, ArduinoOTA and mbedTLS come with the ESP32 core.
4. Flash over USB the first time (and after any partition scheme change). After that, OTA works with hostname `pool-tank-gateway`.

---

## First-time setup

1. After flashing, the ESP32 opens a Wi-Fi hotspot called **`ESP32-PoolTank-AP`**. Join it and pick your Wi-Fi network in the captive portal.
2. Browse to the gateway's IP address.
3. **/mqtt** – enter your MQTT broker address, port, username and password.
4. **/chlorinator** – enter the chlorinator's BLE name (e.g. `POOL01`), MAC address (optional) and its **4-character access code** (shown on the chlorinator).
5. **/tank** – enter tank volume, usable height, unusable height (below the sensor), ADC counts per mm and the ADC reading out of water.

All settings are stored on the ESP32 in `/config.json` (LittleFS). **No credentials are stored in the source code.**

---

## Web dashboard

| Page | Purpose |
|---|---|
| `/` | Live status of tank and chlorinator, system info |
| `/tank` | Tank geometry, calibration, measurement settings |
| `/chlorinator` | BLE name/MAC, access code, poll interval, scan time, app back-off |
| `/timers` | View and change the 4 pump timers |
| `/mqtt` | Broker settings, tank topic prefix |
| `/wifi` | Scan, change network, forget Wi-Fi |
| `/data.json` | Live values (used by the pages, refreshed every 3 s) |

---

## MQTT topics

`<name>` is the chlorinator name in lower case.

### Chlorinator
| Topic | Direction | Content |
|---|---|---|
| `chlorinator/<name>/state` | out | State JSON |
| `chlorinator/<name>/extra` | out | Timers, statistics, setpoint |
| `chlorinator/<name>/health` | out | BLE fault, seconds since good poll, diagnostics |
| `chlorinator/<name>/error` | out | BLE error messages |
| `chlorinator/<name>/action` | in | Action (Off, Auto, Manual, Low, Medium, High, Pool, Spa …) |
| `chlorinator/<name>/chlorine_set` | in | Chlorine output setpoint |
| `chlorinator/<name>/timer_set` | in | Timer change JSON (below) |
| `chlorinator/<name>/ble_pause` | in | Minutes to stay off BLE (0 = resume) |

Timer change example – only `timer` is required; any field left out is kept:

```json
{"timer":1,"start":"08:50","stop":"15:10","speed":"medium","enabled":true}
```

`speed` is `low`, `medium`, `high` (or 0–2). Timer 1 is always enabled.

### Water tank
Default prefix `homeassistant/sensor/tanksensor` (changeable on `/mqtt`):

| Topic | Content |
|---|---|
| `<prefix>/state` | Level, volume, percent, ADC |
| `<prefix>/health` | Fault status, seconds since good reading |
| `<prefix>/availability` | `online` / `offline` |

Home Assistant MQTT discovery is published automatically for both devices.

---

## Notes and limitations

- Tested on a Viron eQuilibrium **without** pH/ORP probes; pH is not shown on the web page.
- The ESP32 must keep Wi-Fi modem sleep on while BLE and Wi-Fi run together.
- The chlorinator accepts only one BLE connection at a time. If the phone app is connected, the gateway waits (default 10 minutes) and tries again.
- The chlorinator sometimes changes a stop-minute when a timer is written; the gateway detects this and re-writes once.

### Security
This is meant for a trusted home network:
- The web dashboard has **no login** – anyone on your network can change settings.
- OTA updates have **no password**.
- Keep the gateway off networks you don't control.

---

## Credits

- BLE protocol and encryption ported from **pychlorinator**.
- Chlorinator side based on **[chlorinator-gateway-esp32](https://github.com/hwmaier/chlorinator-gateway-esp32)** by hwmaier.

Astral Pool and Viron are trademarks of their owners. This project is not affiliated with or endorsed by Astral Pool. Use at your own risk.

## Licence

See [LICENSE](LICENSE).
