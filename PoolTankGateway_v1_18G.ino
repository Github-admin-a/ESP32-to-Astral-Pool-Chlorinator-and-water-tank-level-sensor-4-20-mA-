/*
   ================================================================================
   ESP32 Pool & Tank Gateway  (v1.18 - merge of ChlorinatorGatewayWeb v1.02 and
                               Watertanksensor v15.1)
   ================================================================================
   Board: Wemos LOLIN32 (or any ESP32-WROOM dev board).
   Tools > Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
   - the combined sketch is too big for the default 1.2MB app partition.
   (If your board entry has no Partition Scheme menu, use "ESP32 Dev Module".)

   WHAT IT DOES
     One ESP32 running two independent jobs:
       1. POOL CHLORINATOR: connects to an Astral Pool Viron eQuilibrium
          chlorinator over BLE, authenticates, reads/decrypts its state every
          poll interval, and accepts actions (mode, speed, pool/spa ...) from
          Home Assistant via a "select" entity.
       2. WATER TANK: measures tank level with a submersible pressure sensor on
          an ADS1115 ADC and converts it to height / volume / percent full.

     Both publish to the same MQTT broker over ONE connection, but with
     separate Home Assistant discovery device IDs - so HA shows them as TWO
     SEPARATE DEVICES:
       - "Chlorinator <name>"   (device id chlorinator_<name>)
       - "Water Tank Monitor"   (device id esp32_tank_<chipid>)
     v1.1: chlorinator device also gets three HA buttons - Auto, Manual, Off.
     v1.2: faster actions - sent immediately, retried if BLE is busy, then two
           quick confirm polls so HA shows the new mode within seconds.
     v1.3: reads the 4 pump timers, cell running hours, low-salt running hours
           and cell reversal count (every 5 min + after actions), plus a
           "Low Salt" problem sensor. Read-only.
     v1.4: "Chlorine Output" number entity - shows and sets the chlorine
           output setpoint (range read from the chlorinator, default 0-8).
     v1.5: "Info Message" sensor in HA (NoMessage / LowSalt / NoWaterFlow ...).
     v1.6: WRITE the pump timers. Publish JSON to chlorinator/<name>/timer_set:
             {"timer":1,"start":"08:50","stop":"15:10","speed":"medium","enabled":true}
           Only "timer" is required - any field left out is kept as it is.
           speed: low / medium / high (or 0-2). The write is
           read-modify-write (other timers are untouched), then read back and
           verified. The chlorinator sometimes changes a stop-minute byte on
           write (seen in BLE captures of the phone app); if that happens the
           gateway compensates and re-writes once, and logs the result.
     v1.7: pH reading removed from the web page (not fitted on this model).
     v1.7: Timer 1 is always on (can't be disabled from the web page or MQTT).
           After saving a timer, the page shows the NEW values straight away
           (marked "saving...") and refreshes itself until the chlorinator
           confirms; if the write fails it goes back to the chlorinator's values.
     v1.7: reads the chlorinator straight after boot (no 5-minute blank page);
           connection interval and app back-off time are settings on the
           Chlorinator Setup page.
     v1.7: "APP FRIENDLY" BLE - stays out of the way of the phone app:
           - Poll interval default is now 300 s (5 min); timers/stats/setpoint
             are read on every poll. Commands (web page, HA) still go out at
             once - they never wait for the poll.
           - Before each scheduled poll the gateway does a short scan. If the
             chlorinator isn't advertising (usually because the phone app is
             connected) or a scheduled connect fails, it backs off for
             app_backoff_min minutes (setting, default 10). Skipped polls are not counted as faults
             unless they go on for over an hour.
           - Manual pause via MQTT .../ble_pause with a number of minutes
             (0 = resume). Any command from the web page or HA cancels a
             pause or back-off and runs immediately.
           - New /timers web page to view and change the 4 pump timers.
     v1.8: BLE errors are published to chlorinator/<name>/error instead of
           the state topic. Sending {"error":...} on the state topic made
           every HA entity's value_template fail ("dict object has no
           attribute ...") and blanked the entities. The state topic now
           only ever carries the full state JSON; a failed poll leaves the
           last good values in place and is reported by the BLE Fault /
           Seconds Since Good Poll entities (health topic) as before.
     v1.9: SELF-RECOVERY (the gateway dropped off Wi-Fi after ~20 h and
           stayed off):
           - Wi-Fi: if the connection is lost, force a reconnect every 30 s;
             if still offline after 10 minutes, restart the ESP32.
           - Loop supervisor task: if loop() stops running for 3 minutes
             (a hang), restart the ESP32.
           - Low memory: if free heap stays under 15 kB for 90 s, restart.
           - Diagnostics: the reason for the last restart, uptime and
             lowest free heap are shown on the web page (System) and added
             to the chlorinator health JSON (resetReason, uptimeSec,
             freeHeap, minFreeHeap, wifiDrops) so the cause of the next
             drop-out can be seen in Home Assistant / MQTT.
     v1.10: QUIETER TANK READINGS
           - ADS1115 runs at 8 samples/s (was the 128 SPS default) so the chip
             filters each conversion itself. Conversions are started and
             collected without blocking, so loop() never waits on the ADC.
           - Up to 32 samples per measurement (was 10). The highest and lowest
             quarter are thrown away and the middle half averaged (trimmed
             mean), so a single spike can't drag the result.
           - Results kept as floats (no more whole-count truncation).
           - Exponential smoothing ACROSS measurements. The time constant is
             set automatically from the measurement interval (about 10
             readings, kept between 2 and 15 minutes) and uses the real time
             between readings,
             with a step bypass: if two readings in a row differ from the
             smoothed value by more than TANK_STEP_MM, the filter jumps straight
             to the new level, so real fills/draws still show up promptly.
           - Sensor can be left powered all the time (Sensor Power Mode on
             the /tank page, default Always on), avoiding the power-up
             settling of the boost converter/transducer.
           - Optional: calculate level from the differential A0-A1 reading
             (TANK_USE_DIFFERENTIAL) - only if A1 is the low side of the shunt.
     v1.11: MORE SELF-RECOVERY (went dark overnight, LED off, no setup AP -
           only a power cycle brought it back):
           - Hardware task watchdog (ESP32 TWDT) on loop() and the supervisor
             task: if either stops for HW_WDT_TIMEOUT_S (60 s) the chip
             resets itself, even if every other task is stuck. Restart
             reason then shows "Task watchdog".
           - BLE task watchdog: the supervisor restarts the ESP32 if a single
             BLE job (scan/connect/read/write) runs for over 5 minutes
             (a stuck BLE stack).
           - MQTT watchdog: if Wi-Fi says "connected" but MQTT has been down
             for 5 minutes, Wi-Fi is rejoined (repeated every 5 min); after
             20 minutes the ESP32 restarts.
           - OTA flag timeout: if an OTA/restart "quiet period" shows no
             progress for 2 minutes, the ESP32 restarts (a stuck flag used to
             switch off all the other recovery checks).
           - Diagnostics in Home Assistant (chlorinator device, Diagnostic
             section): Uptime, Free Heap, Lowest Free Heap, Wi-Fi Signal,
             Wi-Fi Drops, Last Restart Reason. Published every 60 s, so HA
             keeps a history of what led up to any future drop-out.
     v1.12: "SETTING TO ..." FEEDBACK - when Off / Auto / Manual is chosen in
           Home Assistant, the Mode sensor shows "Setting to Manual..." (etc.)
           straight away, until a BLE read confirms the chlorinator is in
           that mode. If it isn't confirmed within 90 s the real mode is
           shown again. The status web page shows the same text.
     v1.13: MANUAL USES TIMER 1's SPEED - when Manual is selected (HA select
           or button), the gateway reads Timer 1 in the same BLE session,
           sends Manual, then sends the matching Low / Medium / High speed
           action, so the pump runs at Timer 1's speed. If Timer 1 can't be
           read (or is set to AI) the chlorinator's own default is left.
     v1.14: PUMP SPEED SHOWS "Off" WHEN IDLE - the chlorinator keeps reporting
           a stored speed even when the pump isn't running (e.g. Auto between
           timers). Pump Speed now shows "Off" unless the Pump is running.
     v1.15: TANK "STATUS" NO LONGER STUCK ON DISCONNECTED - after a dropped
           connection, the old MQTT session's Last Will ("offline", retained)
           could reach the broker AFTER the new connection's "online", so HA
           showed the tank as disconnected while it was working.
           - MQTT client ID is now fixed (from the chip ID) instead of random,
             so the broker drops the old session as soon as the new one
             connects - the old will can't arrive after "online".
           - "online" is re-published every 60 s with the diagnostics, so
             any stray "offline" is corrected within a minute.
     v1.16: "SETTING TO ..." FEEDBACK FOR PUMP SPEED - when Low / Medium /
           High speed is chosen in Home Assistant, the Pump Speed sensor
           shows "Setting to Medium..." (etc.) straight away, until a BLE
           read confirms the chlorinator is at that speed. Choosing Manual
           does the same for Timer 1's speed (the speed v1.13 sends). If it
           isn't confirmed within 90 s the real speed is shown again. The
           status web page's Pump row shows the same text.
     v1.17: PUMP TIMER CONTROLS IN HOME ASSISTANT - each timer gets a Speed
           dropdown, Start and Stop time boxes (HH:MM) and, for timers 2-4,
           an Enabled switch. They write through the existing timer_set
           topic (same read-modify-write + verify as the web page). The
           extra topic now also carries t1_speed / t1_start / t1_stop /
           t1_on ... t4_*, which fixes the "dict object has no attribute
           't1_speed'" template warnings from the earlier timer entities
           (same unique_ids, so those entities are taken over, not doubled).
           HA shows a new value straight away while it is being saved.
     v1.18: RELIABLE TIMER CHANGES FROM HOME ASSISTANT - a change made
           while another timer save was still running used to be thrown
           away (the HA switch/dropdown flipped back). Changes are now
           merged and queued: any number of them, on any timers, are
           written together in one read-modify-write and never dropped.
           - Verification only checks the fields that were changed, so a
             stop-minute tweak by the chlorinator on another timer no
             longer marks a good save as failed.
           - If a save can't be confirmed, HA and the web page show what
             the chlorinator really holds (read back), not the old value.
           - A timer change no longer overwrites a mode/speed action
             waiting to be sent (and vice versa).
     Entity unique_ids and MQTT topics are the same as the original sketches,
     so existing HA entities / automations keep working.

   WEB DASHBOARD (one site, port 80)
     /            - live status of both tank and chlorinator + system info
     /tank        - tank geometry, ADC calibration, measurement settings
     /chlorinator - BLE MAC/name, access code, poll interval, scan duration
     /mqtt        - broker settings (shared) + tank topic prefix
     /wifi        - Wi-Fi scan / change network / forget & restart AP
     /data.json   - live values polled by the pages every 3 s

   TASKS / TIMING
     - ble_task (FreeRTOS task) does all BLE work, so a slow chlorinator never
       blocks the web UI, MQTT or tank readings.
     - loop() handles WiFi/MQTT/OTA/web and the tank sensor. Tank sampling is a
       non-blocking state machine (v1.10: ADS1115 conversions are started
       and collected without waiting, ~4 samples/s),
       so the web UI stays responsive while the tank is being measured.
     - WiFi modem sleep stays ON - the ESP32 requires it when WiFi and BLE
       run at the same time.

   OTA UPDATES
     Arduino OTA hostname "pool-tank-gateway". First flash (and any partition
     scheme change) must be done over USB.

   DEFAULT WIRING
     ADS1115 SDA  -> GPIO21
     ADS1115 SCL  -> GPIO22
     Tank sensor power transistor -> GPIO23
     Onboard LED (LOLIN32) -> GPIO5, active low (1 s heartbeat)

   LIBRARIES TO INSTALL
     WiFiManager (tzapu), PubSubClient (Nick O'Leary), ArduinoJson (Benoit
     Blanchon, v7), NimBLE-Arduino (h2zero, ^2.0), Adafruit ADS1X15.
     LittleFS and ArduinoOTA are built into the ESP32 Arduino core.
   ================================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <mbedtls/aes.h>
#include <atomic>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <esp_system.h>   // v1.9: esp_reset_reason()
#include <esp_task_wdt.h> // v1.11: hardware task watchdog
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ─── Hardware ────────────────────────────────────────────────────────────────
#define PIN_LED          5     // Wemos LOLIN32: onboard blue LED, active low
#define SENSOR_POWER_PIN 23    // Tank sensor supply transistor
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22

// ─── Gateway identity ────────────────────────────────────────────────────────
#define GATEWAY_HOSTNAME "pool-tank-gateway"     // OTA hostname
#define GATEWAY_AP_NAME  "ESP32-PoolTank-AP"     // WiFiManager setup hotspot

// ═════════════════════════════════════════════════════════════════════════════
// CRYPTO: AES-128-ECB helpers for the Astral Pool Viron BLE protocol
// ═════════════════════════════════════════════════════════════════════════════
#include <stdint.h>
#include <stddef.h>

// All BLE characteristics for this device are 20 bytes on the wire.
#define CHAR_DATA_LEN 20

/**
 * Derive the 16-byte authentication key and store it in `output`.
 *
 * Algorithm (mirrors Python encrypt_mac_key):
 *   xored  = XOR(session_key[16], access_code[N], zero-pad shorter)
 *   output = AES-128-ECB-encrypt(xored, SECRET_KEY)
 *
 * @param session_key  16-byte random key read from UUID_SLAVE_SESSION_KEY
 * @param access_code  ASCII bytes of the 4-character device code (e.g. "ABCD")
 * @param code_len     Length of access_code in bytes
 * @param output       16-byte output buffer (write to UUID_MASTER_AUTHENTICATION)
 */
void encrypt_mac_key(const uint8_t *session_key,
                     const uint8_t *access_code, size_t code_len,
                     uint8_t *output);

/**
 * Encrypt a 20-byte characteristic payload for writing (e.g. action command).
 *
 * @param data        20-byte plaintext payload
 * @param session_key 16-byte session key
 * @param output      20-byte encrypted output
 */
void encrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output);

/**
 * Decrypt a 20-byte characteristic payload read from the device (e.g. state).
 *
 * @param data        20-byte encrypted payload from BLE read
 * @param session_key 16-byte session key
 * @param output      20-byte decrypted output (parse first 11 bytes as state)
 */
void decrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output);

/*
 * AES-128-ECB encryption/decryption for the Astral Pool Viron BLE protocol.
 *
 * The algorithm is a port of pychlorinator/chlorinator.py:
 *   encrypt_mac_key(), encrypt_characteristic(), decrypt_characteristic()
 *
 * mbedTLS is included with the ESP32 Arduino framework – no extra library needed.
 */
#include <mbedtls/aes.h>

// Shared secret hardcoded in all Viron eQuilibrium devices.
static const uint8_t SECRET_KEY[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

// XOR two byte arrays of potentially different lengths.
// The shorter one is effectively zero-padded on the right.
// result must be at least max(len_a, len_b) bytes.
static void xor_bytes(const uint8_t *a, size_t len_a,
                      const uint8_t *b, size_t len_b,
                      uint8_t *result) {
    size_t len = (len_a > len_b) ? len_a : len_b;
    for (size_t i = 0; i < len; i++) {
        uint8_t va = (i < len_a) ? a[i] : 0;
        uint8_t vb = (i < len_b) ? b[i] : 0;
        result[i]  = va ^ vb;
    }
}

// AES-128-ECB encrypt exactly 16 bytes with SECRET_KEY.
static void aes_encrypt(const uint8_t *in, uint8_t *out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, SECRET_KEY, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// AES-128-ECB decrypt exactly 16 bytes with SECRET_KEY.
static void aes_decrypt(const uint8_t *in, uint8_t *out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, SECRET_KEY, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// ─────────────────────────────────────────────────────────────────────────────

void encrypt_mac_key(const uint8_t *session_key,
                     const uint8_t *access_code, size_t code_len,
                     uint8_t *output) {
    // Python: xored = xor_bytes(session_key[16], access_code[4], zero-pad)
    //         return AES_ECB_encrypt(xored)
    uint8_t xored[16];
    xor_bytes(session_key, 16, access_code, code_len, xored);
    aes_encrypt(xored, output);
}

void encrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output) {
    // Python:
    //   xored = xor_bytes(data[20], session_key[16])          → 20 bytes
    //   array = encrypt(xored[:16]) + xored[16:]              → 20 bytes
    //   return array[:4] + encrypt(array[4:])                 → 20 bytes

    uint8_t xored[CHAR_DATA_LEN];
    xor_bytes(data, CHAR_DATA_LEN, session_key, 16, xored);

    uint8_t array[CHAR_DATA_LEN];
    aes_encrypt(xored, array);                 // array[0:16] = encrypt(xored[0:16])
    memcpy(array + 16, xored + 16, 4);         // array[16:20] = xored[16:20]

    uint8_t enc_tail[16];
    aes_encrypt(array + 4, enc_tail);          // encrypt array[4:20]

    memcpy(output,     array,    4);           // output[0:4]  = array[0:4]
    memcpy(output + 4, enc_tail, 16);          // output[4:20] = encrypted
}

void decrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output) {
    // Python:
    //   array = data[:4] + decrypt(data[4:])                  → 20 bytes
    //   array = decrypt(array[:16]) + array[16:]              → 20 bytes
    //   return xor_bytes(array, session_key)

    uint8_t dec_tail[16];
    aes_decrypt(data + 4, dec_tail);           // decrypt data[4:20]

    uint8_t array[CHAR_DATA_LEN];
    memcpy(array,     data,     4);            // array[0:4]  = data[0:4]
    memcpy(array + 4, dec_tail, 16);           // array[4:20] = decrypted

    uint8_t dec_head[16];
    aes_decrypt(array, dec_head);              // decrypt array[0:16]

    uint8_t combined[CHAR_DATA_LEN];
    memcpy(combined,      dec_head,    16);    // combined[0:16] = decrypted
    memcpy(combined + 16, array + 16,   4);    // combined[16:20] = array[16:20]

    xor_bytes(combined, CHAR_DATA_LEN, session_key, 16, output);
}

// ═════════════════════════════════════════════════════════════════════════════
// CHLORINATOR: BLE UUIDs, state/action structs, parsing
// (fixed protocol constants - not user-configurable, see header comment)
// ═════════════════════════════════════════════════════════════════════════════
#include <stdint.h>
#include <stdbool.h>

// ─── BLE Service & Characteristic UUIDs ─────────────────────────────────────
#define UUID_ASTRALPOOL_SERVICE     "45000001-98b7-4e29-a03f-160174643001"
#define UUID_SLAVE_SESSION_KEY      "45000002-98b7-4e29-a03f-160174643001"
#define UUID_MASTER_AUTHENTICATION  "45000003-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_STATE      "45000200-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_APP_ACTION "45000203-98b7-4e29-a03f-160174643001"

// ─── Enumerations ────────────────────────────────────────────────────────────

typedef enum {
    MODE_BLE_ERROR = -1,  // sentinel: BLE operation failed, posted via state queue
    MODE_OFF       = 0,
    MODE_MANUAL    = 1,
    MODE_AUTO      = 2,
} ChlorinatorMode;

typedef enum {
    SPEED_LOW    = 0,
    SPEED_MEDIUM = 1,
    SPEED_HIGH   = 2,
    SPEED_AI     = 3,
    SPEED_NOTSET = 255,   // Python uses -1; stored as 255 in uint8
} SpeedLevel;

typedef enum {
    INFO_NONE                        = 0,
    INFO_PH_PROBE_NO_COMMS           = 1,
    INFO_PH_PROBE_OTHER_ERROR        = 2,
    INFO_PH_PROBE_CLEAN_CALIBRATE    = 3,
    INFO_ORP_PROBE_NO_COMMS          = 4,
    INFO_ORP_PROBE_OTHER_ERROR       = 5,
    INFO_ORP_PROBE_CLEAN_CALIBRATE   = 6,
    INFO_G4_COMMS_FAILURE            = 7,
    INFO_NO_WATER_FLOW               = 8,
    INFO_RTCC_FAULT                  = 128,
    INFO_ORP_PROBE_PH_PROBE_MISSING  = 129,
    INFO_AI_PUMP_SPEED               = 130,
    INFO_LOW_SALT                    = 131,
    INFO_UNSPECIFIED                 = 132,
} InfoMessage;

typedef enum {
    CHLORINE_UNKNOWN       = 255,  // -1 in Python
    CHLORINE_INVALID       = 0,
    CHLORINE_VERY_VERY_LOW = 1,
    CHLORINE_VERY_LOW      = 2,
    CHLORINE_LOW           = 3,
    CHLORINE_OK            = 4,
    CHLORINE_HIGH          = 5,
    CHLORINE_VERY_HIGH     = 6,
    CHLORINE_VERY_VERY_HIGH= 7,
} ChlorineStatus;

typedef enum {
    ACTION_NO_ACTION                 = 0,
    ACTION_OFF                       = 1,
    ACTION_AUTO                      = 2,
    ACTION_MANUAL                    = 3,
    ACTION_LOW                       = 4,
    ACTION_MEDIUM                    = 5,
    ACTION_HIGH                      = 6,
    ACTION_POOL                      = 7,
    ACTION_SPA                       = 8,
    ACTION_DISMISS_INFO              = 9,
    ACTION_DISABLE_ACID_INDEFINITELY = 10,
    ACTION_DISABLE_ACID_FOR_PERIOD   = 11,
    ACTION_RESET_STATISTICS          = 12,
    ACTION_TRIGGER_CELL_REVERSAL     = 13,
} ChlorinatorAction;

// ─── State Flags (byte 5 of the decrypted state packet) ──────────────────────
#define FLAG_CHEMISTRY_CURRENT    0x01
#define FLAG_CHEMISTRY_VALID      0x02
#define FLAG_SPA_SELECTION        0x04
#define FLAG_PUMP_PRIMING         0x08
#define FLAG_PUMP_OPERATING       0x10
#define FLAG_CELL_OPERATING       0x20
#define FLAG_SETTINGS_CHANGED     0x40
#define FLAG_SANITISING_TOMORROW  0x80

// ─── Parsed Device State ─────────────────────────────────────────────────────
typedef struct {
    ChlorinatorMode  mode;
    SpeedLevel       pump_speed;
    uint8_t          active_timer;
    InfoMessage      info_message;
    uint8_t          flags;
    float            ph_measurement;       // divided by 10 (e.g. 72 → 7.2)
    ChlorineStatus   chlorine_status;
    uint8_t          time_hours;
    uint8_t          time_minutes;
    uint8_t          time_seconds;
    // Pre-decoded flag bits for convenience
    bool chemistry_current;
    bool chemistry_valid;
    bool spa_selection;
    bool pump_priming;
    bool pump_operating;
    bool cell_operating;
    bool sanitising_tomorrow;
} ChlorinatorState;

// ─── Action Command Packet ────────────────────────────────────────────────────
// Binary layout mirrors Python: struct.pack("=B i 15x", action, period_minutes)
// Total 20 bytes – matches CHAR_DATA_LEN.
typedef struct __attribute__((packed)) {
    uint8_t  action;           // ChlorinatorAction
    int32_t  period_minutes;   // Only used with ACTION_DISABLE_ACID_FOR_PERIOD
    uint8_t  _padding[15];
} ChlorinatorActionPacket;

// ─── v1.3: timers + statistics (layouts from pychlorinator) ──────────────────
#define UUID_CHLORINATOR_TIMERS     "45000204-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_STATISTICS "45000205-98b7-4e29-a03f-160174643001"
#define UUID_CHLORINATOR_CAPABILITIES "45000201-98b7-4e29-a03f-160174643001"   // v1.4
#define UUID_CHLORINATOR_SETUP        "45000202-98b7-4e29-a03f-160174643001"   // v1.4
#define SETPOINT_CMD_BASE 1000   // action-queue code: 1000 + level = set chlorine output
#define TIMER_CMD_CODE    3000   // v1.6: action-queue code: wake the BLE task for timer writes (v1.18: requests are in g_timer_req)
#define NUM_PUMP_TIMERS 4

typedef struct {
    bool    enabled;
    uint8_t start_h, start_m, stop_h, stop_m;
    uint8_t speed;              // SpeedLevel: 0 Low, 1 Medium, 2 High, 3 AI
} PumpTimer;

// v1.6: a pending timer write from MQTT. Only fields with has_* set change.
typedef struct {
    uint8_t index;              // 0-3
    bool    has_start, has_stop, has_speed, has_enabled;
    uint8_t start_h, start_m, stop_h, stop_m, speed;
    bool    enabled;
    bool    active;             // v1.18: this slot holds a change
    uint16_t seq;               // v1.18: request number (latest merged)
} TimerSetCmd;

typedef struct {
    bool      timers_valid;
    bool      stats_valid;
    PumpTimer timers[NUM_PUMP_TIMERS];
    uint16_t  cell_reversal_count;
    uint32_t  cell_hours;           // total cell running time, hours
    uint32_t  low_salt_hours;       // cell running time while salt was low, hours
    // v1.4: chlorine output setpoint (Setup characteristic) + its allowed range
    bool      setup_valid;
    uint16_t  chlorine_setpoint;
    bool      caps_valid;
    uint8_t   chlor_min, chlor_max;     // manual chlorine setpoint range
    uint8_t   chlor_control_type;       // 0 none, 1 manual, 2 automatic (ORP)
    // v1.18: timer writes finished in this BLE pass (request seq per timer, 0 = none)
    uint16_t  timer_done_seq[NUM_PUMP_TIMERS];
    bool      timer_batch_failed;       // true = could not be written/confirmed
} ChlorinatorExtra;

// ─── Function Declarations ────────────────────────────────────────────────────

// Parse the first 11 bytes of a 20-byte decrypted characteristic into `state`.
void parse_chlorinator_state(const uint8_t *data, ChlorinatorState *s);

// Zero-fill `pkt` and set action / period fields.
void build_action_packet(ChlorinatorAction action, int32_t period_minutes,
                         ChlorinatorActionPacket *pkt);

// Return a human-readable string for an InfoMessage value.
const char *info_message_name(InfoMessage msg);

void parse_chlorinator_state(const uint8_t *data, ChlorinatorState *s) {
    // Python struct format: @BBBBBBBBBBB (11 bytes, native byte order, all uint8)
    // Offsets:
    //   [0] mode
    //   [1] pump_speed
    //   [2] active_timer
    //   [3] info_message
    //   [4] reserved
    //   [5] flags
    //   [6] ph_measurement  (raw, divide by 10)
    //   [7] chlorine_control_status
    //   [8] time_hours
    //   [9] time_minutes
    //  [10] time_seconds

    s->mode             = (ChlorinatorMode)data[0];
    s->pump_speed       = (SpeedLevel)data[1];
    s->active_timer     = data[2];
    s->info_message     = (InfoMessage)data[3];
    // data[4] is reserved
    s->flags            = data[5];
    s->ph_measurement   = data[6] / 10.0f;
    s->chlorine_status  = (ChlorineStatus)data[7];
    s->time_hours       = data[8];
    s->time_minutes     = data[9];
    s->time_seconds     = data[10];

    s->chemistry_current  = (s->flags & FLAG_CHEMISTRY_CURRENT)   != 0;
    s->chemistry_valid    = (s->flags & FLAG_CHEMISTRY_VALID)      != 0;
    s->spa_selection      = (s->flags & FLAG_SPA_SELECTION)        != 0;
    s->pump_priming       = (s->flags & FLAG_PUMP_PRIMING)         != 0;
    s->pump_operating     = (s->flags & FLAG_PUMP_OPERATING)       != 0;
    s->cell_operating     = (s->flags & FLAG_CELL_OPERATING)       != 0;
    s->sanitising_tomorrow= (s->flags & FLAG_SANITISING_TOMORROW)  != 0;
}

void build_action_packet(ChlorinatorAction action, int32_t period_minutes,
                         ChlorinatorActionPacket *pkt) {
    memset(pkt, 0, sizeof(ChlorinatorActionPacket));
    pkt->action         = (uint8_t)action;
    pkt->period_minutes = period_minutes;
}

const char *info_message_name(InfoMessage msg) {
    switch (msg) {
        case INFO_NONE:                       return "NoMessage";
        case INFO_PH_PROBE_NO_COMMS:          return "PhProbeNoComms";
        case INFO_PH_PROBE_OTHER_ERROR:       return "PhProbeOtherError";
        case INFO_PH_PROBE_CLEAN_CALIBRATE:   return "PhProbeCleanCalibrate";
        case INFO_ORP_PROBE_NO_COMMS:         return "OrpProbeNoComms";
        case INFO_ORP_PROBE_OTHER_ERROR:      return "OrpProbeOtherError";
        case INFO_ORP_PROBE_CLEAN_CALIBRATE:  return "OrpProbeCleanCalibrate";
        case INFO_G4_COMMS_FAILURE:           return "G4CommsFailure";
        case INFO_NO_WATER_FLOW:              return "NoWaterFlow";
        case INFO_RTCC_FAULT:                 return "RtccFault";
        case INFO_ORP_PROBE_PH_PROBE_MISSING: return "OrpProbeFittedPhProbeMissing";
        case INFO_AI_PUMP_SPEED:              return "AiPumpSpeed";
        case INFO_LOW_SALT:                   return "LowSalt";
        case INFO_UNSPECIFIED:                return "Unspecified";
        default:                              return "Unknown";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// PERSISTENT CONFIGURATION (one LittleFS JSON file for everything)
// ═════════════════════════════════════════════════════════════════════════════
const char* CONFIG_FILE = "/config.json";

// ─── Shared MQTT broker ──────────────────────────────────────────────────────
char mqtt_server[64] = "";
int  mqtt_port       = 1883;
char mqtt_user[32]   = "";
char mqtt_pass[32]   = "";

// ─── Chlorinator ─────────────────────────────────────────────────────────────
char chlorinator_mac[24]  = "";          // blank = match by name only
char chlorinator_name[32] = "POOL01";
char chlorinator_code[8]  = "1234";
int  poll_interval_sec     = 300;   // v1.7: was 10 - leave the app room
int  ble_scan_duration_sec = 5;
int  app_backoff_min       = 10;    // v1.7: minutes to stay off BLE when the app seems connected

// ─── Water tank ──────────────────────────────────────────────────────────────
char  tank_topic[64]       = "homeassistant/sensor/tanksensor";  // saved as "mqtt_topic"
int   read_interval_sec    = 60;     // 1 - 86400 s
int   numReadingsToAverage = 10;     // 1 - 32 samples (v1.10: was 1 - 10)
bool  sensorAlwaysOn       = true;   // v1.10: true = sensor + boost always powered (set on /tank page)
int   tankVolumeLitres     = 5000;
int   usableHeightMm       = 1000;
int   unusableHeightMm     = 100;
float adcPerMm             = 10.0;
int   adcOutOfWater        = 205;

// ═════════════════════════════════════════════════════════════════════════════
// DERIVED CONFIGURATION
// ═════════════════════════════════════════════════════════════════════════════
// Chlorinator topics (rebuilt from chlorinator_name by recalcTopics())
char s_name_lower[32];
char s_topic_state[80];
char s_topic_action[80];
char s_topic_health[80];
char s_topic_error[80];         // v1.8: BLE error messages (not the state topic)
char s_topic_extra[80];         // v1.3: timers + statistics
char s_topic_chlorine_set[80];  // v1.4: HA -> chlorine output setpoint
char s_topic_timer_set[80];     // v1.6: JSON -> write a pump timer
char s_topic_discovery[128];
char s_device_id[48];

// Tank geometry (rebuilt by recalcTankDerived())
float volumePerMm        = 0;
float usableVolumeLitres = 0;

// ═════════════════════════════════════════════════════════════════════════════
// SYSTEM & NETWORKING OBJECTS
// ═════════════════════════════════════════════════════════════════════════════
WebServer        server(80);
WiFiClient       s_wifi;
PubSubClient     s_mqtt(s_wifi);    // ONE connection, used by both devices
Adafruit_ADS1115 ads;
bool             adsReady = false;

// ═════════════════════════════════════════════════════════════════════════════
// INTER-TASK SYNCHRONISATION (chlorinator BLE task <-> loop)
// ═════════════════════════════════════════════════════════════════════════════
SemaphoreHandle_t  g_log_mutex;
QueueHandle_t      g_action_queue;        // int (ChlorinatorAction): MQTT → BLE task
QueueHandle_t      g_state_queue;         // ChlorinatorState: BLE task → loop
QueueHandle_t      g_extra_queue;         // ChlorinatorExtra: BLE task → loop (v1.3)
std::atomic<bool>  g_mqtt_connected{false};
std::atomic<bool>  g_ota_in_progress{false};

// v1.9: self-recovery / diagnostics
#define WIFI_RETRY_MS          30000UL          // force a reconnect this often while offline
#define WIFI_RESTART_MS        (10UL * 60000UL) // offline this long -> restart
#define LOOP_HANG_RESTART_MS   (3UL * 60000UL)  // loop() silent this long -> restart
#define LOW_HEAP_BYTES         15000U           // free heap below this ...
#define LOW_HEAP_CHECKS        3                // ... for this many 30 s checks -> restart
std::atomic<unsigned long> g_loop_beat_ms{0};   // last time loop() ran
std::atomic<unsigned int>  g_wifi_drops{0};     // Wi-Fi disconnects since boot
const char *g_reset_reason = "Unknown";         // why the ESP32 last restarted

// v1.11: more self-recovery
#define HW_WDT_TIMEOUT_S       60               // hardware watchdog: loop()/supervisor silent this long -> reset
#define BLE_HANG_RESTART_MS    (5UL * 60000UL)  // one BLE job running this long -> restart
#define OTA_STALL_RESTART_MS   (2UL * 60000UL)  // OTA/quiet period with no progress this long -> restart
#define MQTT_REJOIN_MS         (5UL * 60000UL)  // Wi-Fi up but MQTT down this long -> rejoin Wi-Fi
#define MQTT_RESTART_MS        (20UL * 60000UL) // ... and this long -> restart
#define DIAG_PUBLISH_MS        60000UL          // diagnostics to HA this often
std::atomic<unsigned long> g_ble_busy_since{0}; // millis() when the current BLE job started, 0 = idle
std::atomic<unsigned long> g_ota_activity_ms{0};// last OTA start/progress (or restart request)

// Marks the BLE task as busy for the lifetime of the object (cleared on any
// exit path, including `continue`).
struct BleBusy {
    BleBusy()  { g_ble_busy_since.store(millis() | 1); }
    ~BleBusy() { g_ble_busy_since.store(0); }
};

// Sets the "quiet" flag that pauses the BLE task / recovery checks, and
// stamps the time so the supervisor can tell if it gets stuck.
void quiesce_for_restart() {
    g_ota_activity_ms.store(millis() | 1);
    g_ota_in_progress.store(true);
}
TaskHandle_t       g_ble_task_handle = nullptr;

const int SENSOR_FAULT_THRESHOLD = 3;     // bad cycles in a row = fault (both devices)
std::atomic<int>           g_ble_consecutive_failures{0};
std::atomic<unsigned long> g_ble_last_good_ms{0};

// v1.18: timer change requests (loop -> BLE task), merged per timer,
// guarded by g_timer_mux. g_timer_req_flag says there is something new.
TimerSetCmd       g_timer_req[NUM_PUMP_TIMERS] = {};
std::atomic<bool> g_timer_req_flag{false};

// v1.18: per-timer "saving..." display (loop()/web/MQTT only)
bool          g_tpend[NUM_PUMP_TIMERS]    = {};   // change sent, not yet confirmed
unsigned long g_tpend_ms[NUM_PUMP_TIMERS] = {};
uint16_t      g_tseq[NUM_PUMP_TIMERS]     = {};   // last request number per timer
PumpTimer     g_tdesired[NUM_PUMP_TIMERS] = {};   // what we asked for
PumpTimer     g_tactual[NUM_PUMP_TIMERS]  = {};   // last values read from the chlorinator
int           g_timer_done_idx   = -1;     // last finished save, for the page message
bool          g_timer_done_ok    = false;
const unsigned long TIMER_SAVE_TIMEOUT_MS = 90000;   // > 3 BLE attempts incl. re-scans

// v1.7: BLE pause / app back-off. g_ble_pause_until is a millis() deadline;
// g_ble_paused says whether it is active. g_ble_pause_reason is for the web page.
const int           SKIPS_BEFORE_FAULT = 6;    // ~1 h of back-offs before HA sees a fault
std::atomic<bool>          g_ble_paused{false};
std::atomic<unsigned long> g_ble_pause_until{0};
std::atomic<int>           g_ble_pause_reason{0};   // 0 none, 1 manual, 2 app detected
char s_topic_ble_pause[80];
void ble_pause(unsigned long minutes, int reason) {
    if (minutes == 0) { g_ble_paused.store(false); g_ble_pause_reason.store(0); return; }
    g_ble_pause_until.store(millis() + minutes * 60000UL);
    g_ble_pause_reason.store(reason);
    g_ble_paused.store(true);
}
long ble_pause_left_ms() {
    if (!g_ble_paused.load()) return 0;
    long left = (long)(g_ble_pause_until.load() - millis());
    if (left <= 0) { g_ble_paused.store(false); g_ble_pause_reason.store(0); return 0; }
    return left;
}
portMUX_TYPE  g_timer_mux = portMUX_INITIALIZER_UNLOCKED;

NimBLEAddress g_ble_address;              // BLE task only
bool          g_has_ble_address = false;

// Last chlorinator state for the dashboard (loop()/web handlers only)
ChlorinatorState g_last_state{};
bool             g_has_last_state = false;
bool             g_last_state_ok  = false;
unsigned long    g_last_state_ms  = 0;

// v1.12: mode change requested from HA - shown as "Setting to ..." until a
// BLE read confirms it (or it times out)
int           g_mode_pending    = -1;      // target ChlorinatorMode, -1 = none
unsigned long g_mode_pending_ms = 0;
const unsigned long MODE_PENDING_TIMEOUT_MS = 90000;

// v1.16: pump speed change requested from HA - shown as "Setting to ..."
// until a BLE read confirms it (or it times out, same 90 s as the mode)
int           g_speed_pending    = -1;     // target SpeedLevel (0-2), -1 = none
unsigned long g_speed_pending_ms = 0;

// Chlorinator result waiting to go out on MQTT (held if MQTT is down)
ChlorinatorState g_chlor_pending_state{};
bool             g_chlor_publish_pending = false;

// v1.3: timers + statistics (read every few minutes, not every poll)
ChlorinatorExtra g_last_extra{};
bool             g_has_extra           = false;
bool             g_extra_publish_pending = false;

// ═════════════════════════════════════════════════════════════════════════════
// WATER TANK RUNTIME STATE
// ═════════════════════════════════════════════════════════════════════════════
const unsigned long WARMUP_DELAY_MS            = 3000;  // sensor warm-up before reading
const unsigned long POWER_SAVE_MARGIN_MS = 5000;   // v1.10: sensor must get at least this long OFF to bother switching it
const float         ADC_LOW_FAULT_MARGIN       = 0.10f; // >10% below out-of-water = invalid
const unsigned long SAMPLE_SPACING_MS          = 0;     // v1.10: pacing now comes from the 8 SPS conversions

// v1.10: noise reduction settings
#define TANK_USE_DIFFERENTIAL   0      // 1 = level from A0-A1 (A1 must be the shunt's low side; recalibrate)
const int           TANK_MAX_SAMPLES     = 32;     // per measurement
const int           TANK_SMOOTH_READINGS = 10;     // v1.10: auto smoothing spans about this many readings ...
const int           TANK_SMOOTH_MIN_SEC  = 120;    // ... but never less than 2 minutes ...
const int           TANK_SMOOTH_MAX_SEC  = 900;    // ... or more than 15 minutes (limits lag on slow intervals)
unsigned long       lastSmoothMs         = 0;      // when the smoothed value was last updated
const float         TANK_STEP_MM         = 5.0f;   // a change bigger than this, twice in a row, bypasses smoothing
const unsigned long ADS_CONV_TIMEOUT_MS  = 500;    // one 8 SPS conversion takes ~125 ms

int   currentADC     = 0;     // smoothed value, rounded (web page / MQTT)
int   currentDiffADC = 0;
float rawMeasADC     = 0;     // v1.10: this measurement's trimmed mean (before smoothing)
float rawMeasDiffADC = 0;
float smoothADC      = 0;     // v1.10: across-measurement smoothed values
float smoothDiffADC  = 0;
bool  smoothValid    = false;
int   stepCount      = 0;     // consecutive measurements far from the smoothed value
float heightMm       = 0;
float volumeLitres   = 0;
float percentFull    = 0;

bool  keepSensorPowered = false;
int   tankConsecutiveFailures = 0;
unsigned long tankLastGoodMs = 0;

enum TankState { TANK_IDLE, TANK_WARMING, TANK_SAMPLING };
TankState tankState = TANK_IDLE;

unsigned long lastMeasurementMs = 0;
unsigned long sensorPowerOnMs   = 0;
unsigned long lastSampleMs      = 0;

// Samples for the measurement in progress (v1.10: kept for a trimmed mean)
int16_t tankSamples[TANK_MAX_SAMPLES];
int16_t tankDiffSamples[TANK_MAX_SAMPLES];
int  tankValidSamples = 0, tankSamplesTaken = 0, tankSamplesWanted = 1;

// v1.10: non-blocking ADS1115 conversion phase within TANK_SAMPLING
// 0 = start single-ended, 1 = waiting single-ended, 2 = waiting differential
int           adsPhase       = 0;
unsigned long adsConvStartMs = 0;
int16_t       adsPendingRaw  = 0;

// Result waiting to go out on MQTT
bool tankPublishPending = false;
bool tankLastValid      = false;

// ═════════════════════════════════════════════════════════════════════════════
// FUNCTION DECLARATIONS
// (explicit prototypes for anything using custom types - stops the Arduino
//  IDE's auto-generated prototypes landing above the type definitions)
// ═════════════════════════════════════════════════════════════════════════════
void parse_chlorinator_state(const uint8_t *data, ChlorinatorState *s);
void build_action_packet(ChlorinatorAction action, int32_t period_minutes,
                         ChlorinatorActionPacket *pkt);
const char *info_message_name(InfoMessage msg);
const char *mode_name(ChlorinatorMode m);
const char *speed_name(SpeedLevel s);
const char *chlorine_status_name(ChlorineStatus c);
void loadSettings();
void saveSettings();
void recalcTopics();
bool sensorFaulted();
unsigned long secondsSinceGoodPoll();
void publish_state(const ChlorinatorState &state);
void publish_error(const char *msg);
void publish_ble_health();
void publish_autodiscovery();
void on_mqtt_message(char *topic, byte *payload, unsigned int length);
void mqtt_reconnect();
bool ble_scan();
bool ble_operate(ChlorinatorAction action, ChlorinatorState *out_state, ChlorinatorExtra *out_extra, int set_chlorine, const TimerSetCmd *timer_cmd);
bool ble_write_timer(NimBLERemoteService *pSvc, const uint8_t *session_key, const TimerSetCmd *batch);
void merge_timer_cmd(TimerSetCmd &dst, const TimerSetCmd &src);   // v1.18
bool take_timer_requests(TimerSetCmd *batch);                      // v1.18
bool parse_timer_command(const byte *payload, unsigned int length, TimerSetCmd *out);
bool queue_timer_write(TimerSetCmd c);   // v1.7 (explicit prototype - see note above)
bool ble_write_chlorine_setpoint(NimBLERemoteService *pSvc, const uint8_t *session_key, int value);
void publish_setpoint_discovery();
void chlorineRange(int &mn, int &mx);
void ble_read_extra(NimBLERemoteService *pSvc, const uint8_t *session_key, ChlorinatorExtra *out);
String timer_text(const PumpTimer &t);
void publish_extra(const ChlorinatorExtra &x);
void ble_task(void *);
void setup_ota();
// tank
void recalcTankDerived();
void updatePowerMode();
void tankService();
void tankStartSampling();
void tankTakeSample();
void tankFinishMeasurement();
void tankEndCycle();
bool tankFaulted();
unsigned long tankSecondsSinceGood();
void tank_publish_discovery();
void tank_publish_data();
void tank_publish_health();
String tankAvailTopic();
// web
void handleRoot();
void handleTank();
void handleSaveTank();
void handleChlorinator();
void handleSaveChlorinator();
void handleMqtt();
void handleSaveMqtt();
void handleWifi();
void handleSaveWifi();
void handleResetWifi();
void handleData();
void handleTimers();
String blePauseText();
void handleSaveTimer();
String getLiveScript();

// ─────────────────────────────────────────────────────────────────────────────
// Logging - mutex-protected so both tasks can call safely
// ─────────────────────────────────────────────────────────────────────────────
void tlog(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    Serial.print(buf);
    xSemaphoreGive(g_log_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
void str_to_lower(const char *src, char *dst, size_t dst_len) {
    size_t i = 0;
    while (src[i] && i < dst_len - 1) {
        dst[i] = (char)tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

// Display-only labels for the web dashboard (the MQTT JSON payload keeps raw
// enum integers, unchanged, so existing Home Assistant templates still work).
const char *mode_name(ChlorinatorMode m) {
    switch (m) {
        case MODE_OFF:    return "Off";
        case MODE_MANUAL: return "Manual";
        case MODE_AUTO:   return "Auto";
        default:          return "Unknown";
    }
}

const char *speed_name(SpeedLevel s) {
    switch (s) {
        case SPEED_LOW:    return "Low";
        case SPEED_MEDIUM: return "Medium";
        case SPEED_HIGH:   return "High";
        case SPEED_AI:     return "AI";
        default:           return "Not set";
    }
}

const char *chlorine_status_name(ChlorineStatus c) {
    switch (c) {
        case CHLORINE_INVALID:        return "Invalid";
        case CHLORINE_VERY_VERY_LOW:  return "Very very low";
        case CHLORINE_VERY_LOW:       return "Very low";
        case CHLORINE_LOW:            return "Low";
        case CHLORINE_OK:             return "OK";
        case CHLORINE_HIGH:           return "High";
        case CHLORINE_VERY_HIGH:      return "Very high";
        case CHLORINE_VERY_VERY_HIGH: return "Very very high";
        default:                      return "Unknown";
    }
}

// --------------------------------------------------------------------------------
// PERSISTENT STORAGE (LittleFS JSON) - chlorinator + tank settings in one file.
// Key names match both original sketches, so an existing config.json from
// either one is picked up (the other half falls back to defaults).
// --------------------------------------------------------------------------------
static void copyStr(char *dst, const char *src, size_t len) {
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

void loadSettings() {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        tlog("[CFG] No config file found - using defaults.\n");
        saveSettings();
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        tlog("[CFG] Config parse failed: %s\n", err.c_str());
        return;
    }

    // Shared MQTT
    copyStr(mqtt_server, doc["mqtt_server"] | "", sizeof(mqtt_server));
    mqtt_port = doc["mqtt_port"] | 1883;
    copyStr(mqtt_user, doc["mqtt_user"] | "", sizeof(mqtt_user));
    copyStr(mqtt_pass, doc["mqtt_pass"] | "", sizeof(mqtt_pass));

    // Chlorinator
    copyStr(chlorinator_mac,  doc["chlorinator_mac"]  | "",       sizeof(chlorinator_mac));
    copyStr(chlorinator_name, doc["chlorinator_name"] | "POOL01", sizeof(chlorinator_name));
    copyStr(chlorinator_code, doc["chlorinator_code"] | "1234",   sizeof(chlorinator_code));
    poll_interval_sec     = doc["poll_interval_sec"]     | 300;
    if (poll_interval_sec < 60) poll_interval_sec = 300;   // v1.7: old 10 s setting -> 5 min
    ble_scan_duration_sec = doc["ble_scan_duration_sec"] | 5;
    app_backoff_min       = doc["app_backoff_min"]       | 10;
    if (app_backoff_min < 1 || app_backoff_min > 120) app_backoff_min = 10;

    // Tank
    copyStr(tank_topic, doc["mqtt_topic"] | "homeassistant/sensor/tanksensor", sizeof(tank_topic));
    read_interval_sec    = doc["interval"]         | 60;
    numReadingsToAverage = doc["numReadings"]      | 10;
    sensorAlwaysOn       = doc["sensorAlwaysOn"]   | true;   // v1.10
    tankVolumeLitres     = doc["tankVolumeLitres"] | 5000;
    usableHeightMm       = doc["usableHeightMm"]   | 1000;
    unusableHeightMm     = doc["unusableHeightMm"] | 100;
    adcPerMm             = doc["adcPerMm"]         | 10.0f;
    adcOutOfWater        = doc["adcOutOfWater"]    | 205;

    tlog("[CFG] Settings loaded from LittleFS.\n");
}

void saveSettings() {
    JsonDocument doc;

    doc["mqtt_server"] = mqtt_server;
    doc["mqtt_port"]   = mqtt_port;
    doc["mqtt_user"]   = mqtt_user;
    doc["mqtt_pass"]   = mqtt_pass;

    doc["chlorinator_mac"]       = chlorinator_mac;
    doc["chlorinator_name"]      = chlorinator_name;
    doc["chlorinator_code"]      = chlorinator_code;
    doc["poll_interval_sec"]     = poll_interval_sec;
    doc["ble_scan_duration_sec"] = ble_scan_duration_sec;
    doc["app_backoff_min"]       = app_backoff_min;

    doc["mqtt_topic"]       = tank_topic;
    doc["interval"]         = read_interval_sec;
    doc["numReadings"]      = numReadingsToAverage;
    doc["sensorAlwaysOn"]   = sensorAlwaysOn;          // v1.10
    doc["tankVolumeLitres"] = tankVolumeLitres;
    doc["usableHeightMm"]   = usableHeightMm;
    doc["unusableHeightMm"] = unusableHeightMm;
    doc["adcPerMm"]         = adcPerMm;
    doc["adcOutOfWater"]    = adcOutOfWater;

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        tlog("[CFG] Failed to open config file for writing.\n");
        return;
    }
    serializeJson(doc, f);
    f.close();
    tlog("[CFG] Settings saved to LittleFS.\n");
}

// Rebuilds the chlorinator topic strings from chlorinator_name.
void recalcTopics() {
    str_to_lower(chlorinator_name, s_name_lower, sizeof(s_name_lower));
    snprintf(s_topic_state,     sizeof(s_topic_state),     "chlorinator/%s/state",  s_name_lower);
    snprintf(s_topic_action,    sizeof(s_topic_action),    "chlorinator/%s/action", s_name_lower);
    snprintf(s_topic_health,    sizeof(s_topic_health),    "chlorinator/%s/health", s_name_lower);
    snprintf(s_topic_error,     sizeof(s_topic_error),     "chlorinator/%s/error",  s_name_lower);   // v1.8
    snprintf(s_topic_extra,     sizeof(s_topic_extra),     "chlorinator/%s/extra",  s_name_lower);
    snprintf(s_topic_chlorine_set, sizeof(s_topic_chlorine_set), "chlorinator/%s/chlorine_set", s_name_lower);
    snprintf(s_topic_timer_set, sizeof(s_topic_timer_set), "chlorinator/%s/timer_set", s_name_lower);
    snprintf(s_topic_ble_pause, sizeof(s_topic_ble_pause), "chlorinator/%s/ble_pause", s_name_lower);
    snprintf(s_topic_discovery, sizeof(s_topic_discovery), "homeassistant/device/chlorinator/%s/config", s_name_lower);
    snprintf(s_device_id,       sizeof(s_device_id),       "chlorinator_%s", s_name_lower);
}

// --------------------------------------------------------------------------------
// BLE POLL HEALTH
// --------------------------------------------------------------------------------
bool sensorFaulted() {
    return g_ble_consecutive_failures.load() >= SENSOR_FAULT_THRESHOLD;
}

unsigned long secondsSinceGoodPoll() {
    unsigned long lastGood = g_ble_last_good_ms.load();
    if (lastGood == 0) return 0;   // never had a good poll yet
    return (millis() - lastGood) / 1000UL;
}

// --------------------------------------------------------------------------------
// MQTT: state JSON publishing
// --------------------------------------------------------------------------------
void publish_state(const ChlorinatorState &state) {
    if (!s_mqtt.connected()) return;

    JsonDocument doc;
    doc["mode"]                                   = (int)state.mode;
    if (g_mode_pending >= 0) doc["pending"] = mode_name((ChlorinatorMode)g_mode_pending);   // v1.12
    if (g_speed_pending >= 0) doc["speed_pending"] = speed_name((SpeedLevel)g_speed_pending);   // v1.16
    doc["pump_speed"]                             = (int)state.pump_speed;
    doc["active_timer"]                           = state.active_timer;
    doc["info_message"]                           = info_message_name(state.info_message);
    doc["ph_measurement"]                         = state.ph_measurement;
    doc["chlorine_control_status"]                = (int)state.chlorine_status;
    doc["chemistry_values_current"]               = state.chemistry_current;
    doc["chemistry_values_valid"]                 = state.chemistry_valid;
    doc["time_hours"]                             = state.time_hours;
    doc["time_minutes"]                           = state.time_minutes;
    doc["time_seconds"]                           = state.time_seconds;
    doc["spa_selection"]                          = state.spa_selection;
    doc["pump_is_priming"]                        = state.pump_priming;
    doc["pump_is_operating"]                      = state.pump_operating;
    doc["cell_is_operating"]                      = state.cell_operating;
    doc["sanitising_until_next_timer_tomorrow"]   = state.sanitising_tomorrow;
    doc["low_salt"]                               = (state.info_message == INFO_LOW_SALT);
    doc["ip"]                                     = WiFi.localIP().toString();

    size_t len = measureJson(doc);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        tlog("[MQTT] publish_state: malloc failed\n");
        return;
    }
    size_t written = serializeJson(doc, buf, len + 1);
    if (written == 0) {
        // Belt-and-braces: never publish a blank/truncated payload.
        tlog("[MQTT] publish_state: empty payload, skipped\n");
        free(buf);
        return;
    }
    s_mqtt.publish(s_topic_state, buf, /*retain=*/false);
    free(buf);
    tlog("[MQTT] state published\n");
}

void publish_error(const char *msg) {
    if (!s_mqtt.connected()) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    // v1.8: own topic - publishing this on s_topic_state broke every
    // HA template that reads the state JSON.
    s_mqtt.publish(s_topic_error, buf, /*retain=*/false);
}

// v1.9: why did the ESP32 last restart?
const char *resetReasonName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power on";
        case ESP_RST_EXT:       return "Reset pin";
        case ESP_RST_SW:        return "Software restart";
        case ESP_RST_PANIC:     return "Crash (panic)";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Other watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brownout (power supply dip)";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "Unknown";
    }
}

// v1.11: set up the ESP32 hardware task watchdog. Works with Arduino core
// 2.x (ESP-IDF 4) and 3.x (ESP-IDF 5). Tasks are added with esp_task_wdt_add().
void hw_watchdog_init() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms     = HW_WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;          // only watch the tasks we add
    cfg.trigger_panic  = true;       // panic = reset the chip
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(HW_WDT_TIMEOUT_S, true);   // reconfigures if already running
#endif
    tlog("[SYS] hardware watchdog: %d s\n", HW_WDT_TIMEOUT_S);
}

// v1.9: restarts the ESP32 if loop() stops running (a hang).
// v1.11: also watched by the hardware watchdog itself, and checks for a
// stuck BLE job and a stuck OTA/quiet flag.
void supervisor_task(void *) {
    esp_task_wdt_add(NULL);          // v1.11: if this task stops, the chip resets
    unsigned long last_check = millis();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_task_wdt_reset();
        if (millis() - last_check < 10000) continue;
        last_check = millis();

        if (g_ota_in_progress.load()) {
            // v1.11: an OTA (or restart request) that has stopped moving
            unsigned long act = g_ota_activity_ms.load();
            if (act != 0 && millis() - act > OTA_STALL_RESTART_MS) {
                Serial.println("[SYS] OTA/quiet period stalled - restarting");
                delay(100);
                ESP.restart();
            }
            continue;
        }
        unsigned long beat = g_loop_beat_ms.load();
        if (beat != 0 && millis() - beat > LOOP_HANG_RESTART_MS) {
            Serial.println("[SYS] loop() not running - restarting");
            delay(100);
            ESP.restart();
        }
        // v1.11: one BLE job running far too long = stuck BLE stack
        unsigned long busy = g_ble_busy_since.load();
        if (busy != 0 && millis() - busy > BLE_HANG_RESTART_MS) {
            Serial.println("[SYS] BLE task stuck - restarting");
            delay(100);
            ESP.restart();
        }
    }
}

void publish_ble_health() {
    if (!s_mqtt.connected()) return;

    JsonDocument doc;
    doc["fault"]               = sensorFaulted();
    doc["consecutiveFailures"] = g_ble_consecutive_failures.load();
    doc["secondsSinceGood"]    = secondsSinceGoodPoll();
    // v1.9 diagnostics
    doc["resetReason"]         = g_reset_reason;
    doc["uptimeSec"]           = millis() / 1000UL;
    doc["freeHeap"]            = ESP.getFreeHeap();
    doc["minFreeHeap"]         = ESP.getMinFreeHeap();
    doc["wifiDrops"]           = g_wifi_drops.load();
    doc["rssi"]                = WiFi.RSSI();          // v1.11

    String payload;
    size_t written = serializeJson(doc, payload);
    if (written == 0 || payload.length() == 0) return;

    s_mqtt.publish(s_topic_health, payload.c_str(), true);   // retained
}

// --------------------------------------------------------------------------------
// MQTT: Home Assistant discovery (Individual MQTT Topics for entities)
// --------------------------------------------------------------------------------
void publish_autodiscovery() {
    // Common Device Information shared across all discovery topics
    char dev_name[48];
    snprintf(dev_name, sizeof(dev_name), "Chlorinator %s", chlorinator_name);

    auto send_discovery_config = [&](const char *component_type, const char *key, JsonDocument &doc) {
        // Add Device object
        JsonObject dev = doc["dev"].to<JsonObject>();
        dev["ids"]  = s_device_id;
        dev["name"] = dev_name;
        dev["mf"]   = "Open Source";
        dev["mdl"]  = "MQTT Gateway for Astral Pool";

        // Add Origin object
        JsonObject origin = doc["o"].to<JsonObject>();
        origin["name"] = "chlorinator-gateway-esp32";
        origin["url"]  = "https://github.com/hwmaier/chlorinator-gateway-esp32";

        // Generate unique topic for Home Assistant discovery
        char discovery_topic[160];
        snprintf(discovery_topic, sizeof(discovery_topic),
                 "homeassistant/%s/%s/%s/config",
                 component_type, s_device_id, key);

        size_t len = measureJson(doc);
        char *buf = (char *)malloc(len + 1);
        if (!buf) {
            tlog("[MQTT] autodiscovery: malloc failed\n");
            return;
        }
        serializeJson(doc, buf, len + 1);
        s_mqtt.publish(discovery_topic, (const uint8_t *)buf, (unsigned int)len, /*retain=*/true);
        free(buf);
        // publish_autodiscovery() fires ~16 of these back-to-back; a small
        // gap avoids overwhelming PubSubClient's TCP write and silently
        // dropping one of them.
        delay(20);
    };

    auto add_sensor = [&](const char *key, const char *name, const char *vt,
                          const char *dev_class = nullptr, const char *unit = nullptr) {
        JsonDocument doc;
        doc["name"]        = name;
        doc["state_topic"] = s_topic_state;
        doc["value_template"] = vt;
        doc["force_update"] = true;
        doc["qos"]          = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        doc["unique_id"] = uid;

        if (dev_class) doc["device_class"] = dev_class;
        if (unit)      doc["unit_of_measurement"] = unit;

        send_discovery_config("sensor", key, doc);
    };

    auto add_binary = [&](const char *key, const char *name, const char *vt,
                          const char *dev_class = nullptr) {
        JsonDocument doc;
        doc["name"]           = name;
        doc["state_topic"]    = s_topic_state;
        doc["value_template"] = vt;
        doc["payload_on"]     = true;
        doc["payload_off"]    = false;
        doc["force_update"]   = true;
        doc["qos"]            = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        doc["unique_id"] = uid;

        if (dev_class) doc["device_class"] = dev_class;

        send_discovery_config("binary_sensor", key, doc);
    };

    // --- Sensors Automatically Enabled ---
    add_sensor("mode", "Mode",
        "{% if value_json.pending is defined %}Setting to {{ value_json.pending }}..."   // v1.12
        "{% else %}{% set map = {0:'Off', 1:'Manual on', 2:'Auto'} %}"
        "{{ map[value_json.mode|int] }}{% endif %}");

    add_binary("pump_is_operating", "Pump",
        "{{ value_json.pump_is_operating }}", "running");

    add_binary("pump_is_priming", "Priming",
        "{{ value_json.pump_is_priming }}", "running");

    add_binary("cell_is_operating", "Cell",
        "{{ value_json.cell_is_operating }}");

    add_sensor("active_timer", "Active Timer",
        "{{ value_json.active_timer }}");

    add_binary("sanitising_until_next_timer_tomorrow",
        "Sanitising until next timer tomorrow",
        "{{ value_json.sanitising_until_next_timer_tomorrow }}");

    // Automatically auto-discovers pump speed, pH measurement, and chemistry status
    add_sensor("pump_speed", "Pump Speed",                                   // v1.14: Off when idle
        "{% set map = {0:'Low', 1:'Medium', 2:'High', 3:'AI', 255:'Not set'} %}"
        "{% if value_json.speed_pending is defined %}Setting to {{ value_json.speed_pending }}..."   // v1.16
        "{% elif not value_json.pump_is_operating %}Off"
        "{% else %}{{ map[value_json.pump_speed|int] if value_json.pump_speed|int in map else 'Unknown' }}{% endif %}");

    add_sensor("ph_measurement", "pH Measurement",
        "{{ value_json.ph_measurement }}", "ph");

    add_binary("chemistry_values_current", "Chemistry Values Current",
        "{{ value_json.chemistry_values_current }}");

    add_binary("chemistry_values_valid", "Chemistry Values Valid",
        "{{ value_json.chemistry_values_valid }}");

    add_sensor("chlorine_control_status", "Chlorine Control Status",
        "{% set map = {255:'Unknown', 0:'Invalid', 1:'Very Very Low', 2:'Very Low', 3:'Low', 4:'OK', 5:'High', 6:'Very High', 7:'Very Very High'} %}"
        "{{ map[value_json.chlorine_control_status|int] if value_json.chlorine_control_status|int in map else 'Unknown' }}");

    add_binary("spa_selection", "Spa Selection",
        "{{ value_json.spa_selection }}");

    // v1.5: the chlorinator's info/warning message (was in the MQTT payload
    // but never announced to HA). Friendly text; unknown codes pass through.
    add_sensor("info_message", "Info Message",
        "{% set m = {'NoMessage':'No message','PhProbeNoComms':'pH probe no comms',"
        "'PhProbeOtherError':'pH probe error','PhProbeCleanCalibrate':'pH probe clean/calibrate',"
        "'OrpProbeNoComms':'ORP probe no comms','OrpProbeOtherError':'ORP probe error',"
        "'OrpProbeCleanCalibrate':'ORP probe clean/calibrate','G4CommsFailure':'G4 comms failure',"
        "'NoWaterFlow':'No water flow','RtccFault':'Clock fault',"
        "'OrpProbeFittedPhProbeMissing':'pH probe missing','AiPumpSpeed':'AI pump speed',"
        "'LowSalt':'Low salt','Unspecified':'Unspecified'} %}"
        "{{ m.get(value_json.info_message, value_json.info_message) }}");

    add_sensor("ip", "IP Address",
        "{{ value_json.ip }}");

    // --- v1.3: Low salt warning (from the chlorinator's info message) ---
    add_binary("low_salt", "Low Salt",
        "{{ value_json.low_salt }}", "problem");

    // --- v1.3: timers + statistics (separate, retained "extra" topic) ---
    auto add_extra = [&](const char *key, const char *name, const char *vt,
                         const char *icon, const char *unit = nullptr,
                         const char *dev_class = nullptr, const char *state_class = nullptr) {
        JsonDocument doc;
        doc["name"]           = name;
        doc["state_topic"]    = s_topic_extra;
        doc["value_template"] = vt;
        doc["icon"]           = icon;
        doc["qos"]            = 1;
        if (unit)        doc["unit_of_measurement"] = unit;
        if (dev_class)   doc["device_class"]        = dev_class;
        if (state_class) doc["state_class"]         = state_class;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        doc["unique_id"] = uid;

        send_discovery_config("sensor", key, doc);
    };
    add_extra("timer1", "Pump Timer 1", "{{ value_json.timer1 }}", "mdi:timer-outline");
    add_extra("timer2", "Pump Timer 2", "{{ value_json.timer2 }}", "mdi:timer-outline");
    add_extra("timer3", "Pump Timer 3", "{{ value_json.timer3 }}", "mdi:timer-outline");
    add_extra("timer4", "Pump Timer 4", "{{ value_json.timer4 }}", "mdi:timer-outline");
    add_extra("cell_hours", "Cell Running Time", "{{ value_json.cell_hours }}",
              "mdi:timer-cog-outline", "h", "duration", "total_increasing");
    add_extra("low_salt_hours", "Low Salt Cell Running Time", "{{ value_json.low_salt_hours }}",
              "mdi:shaker-outline", "h", "duration", "total_increasing");
    // --- v1.17: timer controls (write via timer_set, state from extra) ---
    for (int n = 1; n <= NUM_PUMP_TIMERS; n++) {
        char key[24], name[40], vt[48], ct[96];

        {   // Speed dropdown
            JsonDocument doc;
            snprintf(name, sizeof(name), "Timer %d Speed", n);
            snprintf(vt, sizeof(vt), "{{ value_json.t%d_speed }}", n);
            snprintf(ct, sizeof(ct), "{\"timer\":%d,\"speed\":\"{{ value }}\"}", n);
            doc["name"]             = name;
            doc["state_topic"]      = s_topic_extra;
            doc["value_template"]   = vt;
            doc["command_topic"]    = s_topic_timer_set;
            doc["command_template"] = ct;
            JsonArray o = doc["options"].to<JsonArray>();
            o.add("Low"); o.add("Medium"); o.add("High");
            doc["icon"] = "mdi:pump";
            doc["qos"]  = 1;
            snprintf(key, sizeof(key), "timer%d_speed", n);
            char uid[64]; snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
            doc["unique_id"] = uid;
            send_discovery_config("select", key, doc);
        }
        for (int which = 0; which < 2; which++) {   // Start / Stop time boxes
            const char *field = which ? "stop" : "start";
            JsonDocument doc;
            snprintf(name, sizeof(name), "Timer %d %s", n, which ? "Stop" : "Start");
            snprintf(vt, sizeof(vt), "{{ value_json.t%d_%s }}", n, field);
            snprintf(ct, sizeof(ct), "{\"timer\":%d,\"%s\":\"{{ value }}\"}", n, field);
            doc["name"]             = name;
            doc["state_topic"]      = s_topic_extra;
            doc["value_template"]   = vt;
            doc["command_topic"]    = s_topic_timer_set;
            doc["command_template"] = ct;
            doc["min"]     = 4;
            doc["max"]     = 5;
            doc["pattern"] = "^([01]?[0-9]|2[0-3]):[0-5][0-9]$";
            doc["icon"]    = which ? "mdi:clock-end" : "mdi:clock-start";
            doc["qos"]     = 1;
            snprintf(key, sizeof(key), "timer%d_%s", n, field);
            char uid[64]; snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
            doc["unique_id"] = uid;
            send_discovery_config("text", key, doc);
        }
        snprintf(key, sizeof(key), "timer%d_on", n);
        if (n == 1) {
            // Timer 1 is always on - remove any old switch for it
            char topic[160];
            snprintf(topic, sizeof(topic), "homeassistant/switch/%s/%s/config", s_device_id, key);
            s_mqtt.publish(topic, (const uint8_t *)"", 0, /*retain=*/true);
            delay(20);
            continue;
        }
        {   // Enabled switch (timers 2-4)
            JsonDocument doc;
            char pon[48], poff[48];
            snprintf(name, sizeof(name), "Timer %d Enabled", n);
            snprintf(vt, sizeof(vt), "{{ 'ON' if value_json.t%d_on else 'OFF' }}", n);
            snprintf(pon,  sizeof(pon),  "{\"timer\":%d,\"enabled\":true}", n);
            snprintf(poff, sizeof(poff), "{\"timer\":%d,\"enabled\":false}", n);
            doc["name"]           = name;
            doc["state_topic"]    = s_topic_extra;
            doc["value_template"] = vt;
            doc["state_on"]       = "ON";
            doc["state_off"]      = "OFF";
            doc["command_topic"]  = s_topic_timer_set;
            doc["payload_on"]     = pon;
            doc["payload_off"]    = poff;
            doc["icon"]           = "mdi:timer-cog-outline";
            doc["qos"]            = 1;
            char uid[64]; snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
            doc["unique_id"] = uid;
            send_discovery_config("switch", key, doc);
        }
    }

    add_extra("cell_reversals", "Cell Reversal Count", "{{ value_json.cell_reversal_count }}",
              "mdi:swap-horizontal", nullptr, nullptr, "total_increasing");

    // --- Select Entity (Action) ---
    {
        JsonDocument doc;
        doc["name"]          = "Action";
        doc["command_topic"] = s_topic_action;
        doc["qos"]           = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_action", s_device_id);
        doc["unique_id"] = uid;

        JsonArray opts = doc["options"].to<JsonArray>();
        opts.add("No action");
        opts.add("Off");
        opts.add("Automatic");
        opts.add("Manual");
        opts.add("Low speed");
        opts.add("Medium speed");
        opts.add("High speed");
        opts.add("Pool mode");
        opts.add("Spa mode");
        opts.add("Dismiss info message");
        opts.add("Disable acid dosing indefinitely");
        opts.add("Disable acid dosing for period");
        opts.add("Reset statistics");
        opts.add("Trigger cell reversal");

        doc["command_template"] =
            "{% set map = {"
            "'No action':0,'Off':1,'Automatic':2,'Manual':3,"
            "'Low speed':4,'Medium speed':5,'High speed':6,"
            "'Pool mode':7,'Spa mode':8,'Dismiss info message':9,"
            "'Disable acid dosing indefinitely':10,"
            "'Disable acid dosing for period':11,"
            "'Reset statistics':12,'Trigger cell reversal':13"
            "} %}{{ map[value] }}";

        send_discovery_config("select", "action", doc);
    }

    // --- Mode Buttons (one press = one action, sent to the same action topic
    //     as the select above; on_mqtt_message() parses the number) ---
    auto add_button = [&](const char *key, const char *name, const char *icon, int action) {
        JsonDocument doc;
        doc["name"]          = name;
        doc["command_topic"] = s_topic_action;
        doc["payload_press"] = String(action);
        doc["icon"]          = icon;
        doc["qos"]           = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        doc["unique_id"] = uid;

        send_discovery_config("button", key, doc);
    };
    add_button("btn_auto",   "Auto",   "mdi:autorenew", ACTION_AUTO);
    add_button("btn_manual", "Manual", "mdi:hand-back-right", ACTION_MANUAL);
    add_button("btn_off",    "Off",    "mdi:power", ACTION_OFF);

    // --- BLE Health Diagnostic Entities ---
    {
        JsonDocument doc;
        doc["name"]           = "BLE Fault";
        doc["state_topic"]    = s_topic_health;
        doc["value_template"] = "{{ 'ON' if value_json.fault else 'OFF' }}";
        doc["payload_on"]     = "ON";
        doc["payload_off"]    = "OFF";
        doc["device_class"]   = "problem";
        doc["qos"]            = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_ble_fault", s_device_id);
        doc["unique_id"] = uid;

        send_discovery_config("binary_sensor", "ble_fault", doc);
    }

    {
        JsonDocument doc;
        doc["name"]                 = "Seconds Since Good Poll";
        doc["state_topic"]          = s_topic_health;
        doc["value_template"]       = "{{ value_json.secondsSinceGood }}";
        doc["unit_of_measurement"]  = "s";
        doc["icon"]                  = "mdi:timer-alert";
        doc["entity_category"]       = "diagnostic";
        doc["force_update"]          = true;
        doc["qos"]                   = 1;

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_ble_reading_age", s_device_id);
        doc["unique_id"] = uid;

        send_discovery_config("sensor", "ble_reading_age", doc);
    }

    // --- v1.11: gateway diagnostics (from the health topic) ---
    auto add_diag = [&](const char *key, const char *name, const char *vt, const char *icon,
                        const char *unit = nullptr, const char *dev_class = nullptr,
                        bool measurement = true) {
        JsonDocument doc;
        doc["name"]            = name;
        doc["state_topic"]     = s_topic_health;
        doc["value_template"]  = vt;
        doc["icon"]            = icon;
        doc["entity_category"] = "diagnostic";
        doc["qos"]             = 1;
        if (unit)        doc["unit_of_measurement"] = unit;
        if (dev_class)   doc["device_class"]        = dev_class;
        if (measurement) doc["state_class"]         = "measurement";

        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        doc["unique_id"] = uid;

        send_discovery_config("sensor", key, doc);
    };
    add_diag("gw_uptime", "Gateway Uptime",
             "{{ (value_json.uptimeSec | int / 3600) | round(2) }}", "mdi:timer-outline", "h");
    add_diag("gw_free_heap", "Gateway Free Heap",
             "{{ value_json.freeHeap }}", "mdi:memory", "B");
    add_diag("gw_min_free_heap", "Gateway Lowest Free Heap",
             "{{ value_json.minFreeHeap }}", "mdi:memory", "B");
    add_diag("gw_rssi", "Gateway Wi-Fi Signal",
             "{{ value_json.rssi }}", "mdi:wifi", "dBm", "signal_strength");
    add_diag("gw_wifi_drops", "Gateway Wi-Fi Drops",
             "{{ value_json.wifiDrops }}", "mdi:wifi-off");
    add_diag("gw_reset_reason", "Gateway Last Restart Reason",
             "{{ value_json.resetReason }}", "mdi:restart", nullptr, nullptr, false);

    publish_setpoint_discovery();   // v1.4
    tlog("[MQTT] autodiscovery sent for all individual entities\n");
}

// ═════════════════════════════════════════════════════════════════════════════
// WATER TANK: geometry, power mode, non-blocking measurement
// ═════════════════════════════════════════════════════════════════════════════
void recalcTankDerived() {
    int totalHeightMm = usableHeightMm + unusableHeightMm;
    volumePerMm = (totalHeightMm > 0) ? ((float)tankVolumeLitres / totalHeightMm) : 0;
    usableVolumeLitres = volumePerMm * usableHeightMm;
}

// Chooses always-on vs. warm-up-before-reading, and restarts the cycle.
// v1.10: time constant of the across-measurement smoothing, from the interval.
int tankSmoothTauSec() {
    long tau = (long)read_interval_sec * TANK_SMOOTH_READINGS;
    return (int)constrain(tau, (long)TANK_SMOOTH_MIN_SEC, (long)TANK_SMOOTH_MAX_SEC);
}

// v1.10: how long one measurement takes with the sensor switched on:
// warm-up + samples (2 conversions at 8 SPS each, ~250 ms per sample).
unsigned long tankCycleMs() {
    return WARMUP_DELAY_MS + (unsigned long)constrain(numReadingsToAverage, 1, TANK_MAX_SAMPLES) * 250UL;
}

// v1.10: power saving only makes sense if the sensor gets a real rest
// between measurements; otherwise it's left on.
bool tankIntervalTooShortToSave() {
    return (unsigned long)read_interval_sec * 1000UL < tankCycleMs() + POWER_SAVE_MARGIN_MS;
}

void updatePowerMode() {
    keepSensorPowered = sensorAlwaysOn || tankIntervalTooShortToSave();
    tankState = TANK_IDLE;   // abandon any measurement in progress
    smoothValid = false;     // v1.10: settings/calibration may have changed - restart smoothing
    stepCount   = 0;

    if (keepSensorPowered) {
        digitalWrite(SENSOR_POWER_PIN, HIGH);
        // first reading ~3 s from now (lets the sensor settle)
        lastMeasurementMs = millis() - (unsigned long)read_interval_sec * 1000UL + WARMUP_DELAY_MS;
    } else {
        digitalWrite(SENSOR_POWER_PIN, LOW);
        lastMeasurementMs = millis();
    }
}

// Called every loop() pass. Never blocks for more than one ADC sample.
void tankService() {
    if (g_ota_in_progress.load()) return;

    unsigned long now        = millis();
    unsigned long intervalMs = (unsigned long)read_interval_sec * 1000UL;

    switch (tankState) {
        case TANK_IDLE:
            if (keepSensorPowered) {
                if (now - lastMeasurementMs >= intervalMs) {
                    lastMeasurementMs = now;
                    tankStartSampling();
                }
            } else {
                // v1.10: power-on to power-on = the interval (was interval
                // + sampling time, because it counted from power-off)
                if (now - lastMeasurementMs >= intervalMs) {
                    lastMeasurementMs = now;
                    digitalWrite(SENSOR_POWER_PIN, HIGH);
                    sensorPowerOnMs = now;
                    tankState = TANK_WARMING;
                    tlog("[TANK] Sensor power ON - warming up...\n");
                }
            }
            break;

        case TANK_WARMING:
            if (now - sensorPowerOnMs >= WARMUP_DELAY_MS) {
                tankStartSampling();
            }
            break;

        case TANK_SAMPLING:
            // v1.10: tankTakeSample() advances a non-blocking conversion and
            // returns quickly; a sample completes every ~250 ms (2 conversions).
            if (adsPhase != 0 || tankSamplesTaken == 0 || now - lastSampleMs >= SAMPLE_SPACING_MS) {
                tankTakeSample();
                if (tankSamplesTaken >= tankSamplesWanted) {
                    tankFinishMeasurement();
                }
            }
            break;
    }
}

void tankStartSampling() {
    if (!adsReady) {
        adsReady = ads.begin();
        if (!adsReady) {
            tlog("[TANK] ADS1115 not detected - skipping this reading.\n");
            tankConsecutiveFailures++;
            tankLastValid      = false;
            tankPublishPending = true;
            tankEndCycle();
            return;
        }
        ads.setDataRate(RATE_ADS1115_8SPS);   // v1.10
    }
    tankValidSamples = 0;
    tankSamplesTaken = 0;
    tankSamplesWanted = constrain(numReadingsToAverage, 1, TANK_MAX_SAMPLES);
    adsPhase = 0;
    tankState = TANK_SAMPLING;
}

// v1.10: one step of a non-blocking sample (single-ended A0, then A0-A1).
// Each call either starts a conversion, finds it still running (returns at
// once), or collects the result. A sample is complete after both conversions.
void tankTakeSample() {
    unsigned long now = millis();
    switch (adsPhase) {
        case 0:
            ads.startADCReading(ADS1X15_REG_CONFIG_MUX_SINGLE_0, false);
            adsConvStartMs = now;
            adsPhase = 1;
            return;

        case 1:
            if (!ads.conversionComplete()) {
                if (now - adsConvStartMs < ADS_CONV_TIMEOUT_MS) return;
                tlog("[TANK] ADS1115 conversion timed out - sample discarded\n");
                tankSamplesTaken++;
                lastSampleMs = now;
                adsPhase = 0;
                return;
            }
            adsPendingRaw = ads.getLastConversionResults();
            ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
            adsConvStartMs = now;
            adsPhase = 2;
            return;

        case 2:
        default:
            if (!ads.conversionComplete()) {
                if (now - adsConvStartMs < ADS_CONV_TIMEOUT_MS) return;
                tlog("[TANK] ADS1115 conversion timed out - sample discarded\n");
                tankSamplesTaken++;
                lastSampleMs = now;
                adsPhase = 0;
                return;
            }
            break;
    }

    int16_t raw     = adsPendingRaw;
    int16_t diffRaw = ads.getLastConversionResults();   // A0 minus A1
    adsPhase = 0;
    lastSampleMs = now;
    tankSamplesTaken++;

    float lowFaultCutoff = adcOutOfWater * (1.0f - ADC_LOW_FAULT_MARGIN);
    if (raw >= 0 && raw >= lowFaultCutoff) {
        if (tankValidSamples < TANK_MAX_SAMPLES) {
            tankSamples[tankValidSamples]     = raw;
            tankDiffSamples[tankValidSamples] = diffRaw;
            tankValidSamples++;
        }
    } else if (raw < 0) {
        tlog("[TANK] Discarded an invalid (negative) raw sample\n");
    } else {
        tlog("[TANK] Discarded a suspiciously low raw sample (%d, below out-of-water cutoff of %.1f) - possible sensor disconnect\n",
             raw, lowFaultCutoff);
    }
}

// v1.10: sort, drop the top and bottom quarter, average the middle half.
// With fewer than 4 samples this is a plain mean.
static float trimmedMean(int16_t *v, int n) {
    for (int i = 1; i < n; i++) {            // insertion sort (n <= 32)
        int16_t key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
    int drop = n / 4;
    long sum = 0;
    for (int i = drop; i < n - drop; i++) sum += v[i];
    return (float)sum / (float)(n - 2 * drop);
}

void tankFinishMeasurement() {
    if (tankValidSamples == 0) {
        tlog("[TANK] All samples this cycle were invalid - keeping previous reading. Free heap: %u\n",
             ESP.getFreeHeap());
        tankConsecutiveFailures++;
        tankLastValid = false;
    } else {
        rawMeasADC     = trimmedMean(tankSamples, tankValidSamples);
        rawMeasDiffADC = trimmedMean(tankDiffSamples, tankValidSamples);

        // v1.10: smoothing across measurements, with a step bypass for real
        // fills/draws. alpha comes from the time constant and read interval.
        float perMm  = (adcPerMm == 0 ? 1.0f : adcPerMm);
        float levelRaw = TANK_USE_DIFFERENTIAL ? rawMeasDiffADC : rawMeasADC;
        float levelSm  = TANK_USE_DIFFERENTIAL ? smoothDiffADC  : smoothADC;

        unsigned long nowMs = millis();
        if (!smoothValid) {
            smoothADC = rawMeasADC;  smoothDiffADC = rawMeasDiffADC;
            smoothValid = true;      stepCount = 0;
        } else if (fabsf(levelRaw - levelSm) / fabsf(perMm) > TANK_STEP_MM) {
            if (++stepCount >= 2) {
                tlog("[TANK] Large level change - smoothing bypassed\n");
                smoothADC = rawMeasADC;  smoothDiffADC = rawMeasDiffADC;
                stepCount = 0;
            }
            // first outlier: hold the smoothed value, wait for confirmation
        } else {
            // v1.10: real elapsed time, so the filter is right whatever the
            // interval or power mode
            float dtSec = (nowMs - lastSmoothMs) / 1000.0f;
            float alpha = 1.0f - expf(-dtSec / (float)tankSmoothTauSec());
            smoothADC     += alpha * (rawMeasADC     - smoothADC);
            smoothDiffADC += alpha * (rawMeasDiffADC - smoothDiffADC);
            stepCount = 0;
        }

        lastSmoothMs   = nowMs;
        currentADC     = (int)lroundf(smoothADC);
        currentDiffADC = (int)lroundf(smoothDiffADC);
        tankConsecutiveFailures = 0;
        tankLastGoodMs = millis();

        float levelADC = TANK_USE_DIFFERENTIAL ? smoothDiffADC : smoothADC;
        heightMm = (levelADC - adcOutOfWater) / perMm;

        int totalHeightMm = usableHeightMm + unusableHeightMm;
        if (heightMm < 0)             heightMm = -1;
        if (heightMm > totalHeightMm) heightMm = totalHeightMm;

        float usableHeightActual = (heightMm > unusableHeightMm) ? (heightMm - (float)unusableHeightMm) : 0.0f;
        volumeLitres = usableHeightActual * volumePerMm;
        percentFull  = (usableVolumeLitres > 0) ? (volumeLitres / usableVolumeLitres) * 100.0f : 0.0f;
        percentFull  = constrain(percentFull, 0.0f, 100.0f);
        tankLastValid = true;
    }
    tankPublishPending = true;
    tankEndCycle();
}

void tankEndCycle() {
    tankState = TANK_IDLE;
    if (!keepSensorPowered) {
        digitalWrite(SENSOR_POWER_PIN, LOW);
        tlog("[TANK] Sensor power OFF\n");
    }
}

bool tankFaulted() {
    return tankConsecutiveFailures >= SENSOR_FAULT_THRESHOLD;
}

unsigned long tankSecondsSinceGood() {
    if (tankLastGoodMs == 0) return 0;
    return (millis() - tankLastGoodMs) / 1000UL;
}

// ═════════════════════════════════════════════════════════════════════════════
// WATER TANK: MQTT (same topics / unique_ids as Watertanksensor v15.1)
// ═════════════════════════════════════════════════════════════════════════════
String tankAvailTopic() {
    return String(tank_topic) + "/availability";
}

void tank_publish_discovery() {
    String chipId      = String((uint32_t)ESP.getEfuseMac(), HEX);
    String stateTopic  = String(tank_topic) + "/state";
    String availTopic  = tankAvailTopic();
    String healthTopic = String(tank_topic) + "/health";
    String devId       = "esp32_tank_" + chipId;

    auto addDevice = [&](JsonDocument &doc, bool full) {
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = devId;
        dev["name"] = "Water Tank Monitor";
        if (full) {
            dev["model"]        = "ESP32 ADS1115 Tank Sensor";
            dev["manufacturer"] = "Custom DIY";
        }
    };

    auto send = [&](String topic, JsonDocument &doc) {
        topic.replace(" ", "_");
        topic.toLowerCase();
        String payload;
        serializeJson(doc, payload);
        s_mqtt.publish(topic.c_str(), payload.c_str(), true);
        delay(20);   // don't flood PubSubClient's TCP write
    };

    auto publishEntity = [&](String name, String unit, String icon, String valTpl, String stateClass = "") {
        JsonDocument doc;
        doc["name"]           = "Tank Sensor " + name;
        doc["state_topic"]    = stateTopic;
        doc["unique_id"]      = "tanksensor_" + chipId + "_" + name;
        if (unit.length() > 0) doc["unit_of_measurement"] = unit;
        doc["value_template"] = valTpl;
        if (icon.length() > 0) doc["icon"] = icon;
        if (stateClass.length() > 0) doc["state_class"] = stateClass;
        doc["force_update"]   = true;
        addDevice(doc, true);
        send("homeassistant/sensor/tanksensor_" + chipId + "_" + name + "/config", doc);
    };

    publishEntity("Volume", "L", "mdi:water", "{{ value_json.volume }}");
    publishEntity("Level Percent", "%", "mdi:gauge", "{{ value_json.percentfull }}");
    publishEntity("Height", "mm", "mdi:ruler", "{{ value_json.height }}");
    publishEntity("ADC Reading", "", "mdi:chip", "{{ value_json.adc }}");
    publishEntity("Differential ADC Reading", "raw", "mdi:chip", "{{ value_json.adc_diff }}", "measurement");
    publishEntity("IP Address", "", "mdi:ip-network", "{{ value_json.ip }}");

    {   // Online/offline status (driven by the connection's Last Will)
        JsonDocument doc;
        doc["name"]         = "Tank Sensor Status";
        doc["state_topic"]  = availTopic;
        doc["payload_on"]   = "online";
        doc["payload_off"]  = "offline";
        doc["device_class"] = "connectivity";
        doc["unique_id"]    = "tanksensor_" + chipId + "_status";
        addDevice(doc, false);
        send("homeassistant/binary_sensor/tanksensor_" + chipId + "_status/config", doc);
    }
    {   // Sensor fault
        JsonDocument doc;
        doc["name"]           = "Tank Sensor Fault";
        doc["state_topic"]    = healthTopic;
        doc["value_template"] = "{{ 'ON' if value_json.fault else 'OFF' }}";
        doc["payload_on"]     = "ON";
        doc["payload_off"]    = "OFF";
        doc["device_class"]   = "problem";
        doc["unique_id"]      = "tanksensor_" + chipId + "_fault";
        addDevice(doc, false);
        send("homeassistant/binary_sensor/tanksensor_" + chipId + "_fault/config", doc);
    }
    {   // Seconds since good reading
        JsonDocument doc;
        doc["name"]                = "Tank Sensor Seconds Since Good Reading";
        doc["state_topic"]         = healthTopic;
        doc["value_template"]      = "{{ value_json.secondsSinceGood }}";
        doc["unit_of_measurement"] = "s";
        doc["icon"]                = "mdi:timer-alert";
        doc["entity_category"]     = "diagnostic";
        doc["unique_id"]           = "tanksensor_" + chipId + "_reading_age";
        doc["force_update"]        = true;
        addDevice(doc, false);
        send("homeassistant/sensor/tanksensor_" + chipId + "_reading_age/config", doc);
    }
    tlog("[MQTT] tank autodiscovery sent\n");
}

void tank_publish_data() {
    JsonDocument doc;
    doc["volume"]      = (long)round(volumeLitres);
    doc["percentfull"] = (long)round(percentFull);
    doc["height"]      = (long)round(heightMm);
    doc["adc"]         = currentADC;
    doc["adc_diff"]    = currentDiffADC;
    doc["adc_raw"]     = round(rawMeasADC * 10.0f) / 10.0f;   // v1.10: unsmoothed, for comparison
    doc["ip"]          = WiFi.localIP().toString();

    String payload;
    if (serializeJson(doc, payload) == 0 || payload.length() == 0) {
        tlog("[MQTT] tank publish skipped - empty payload\n");
        return;
    }
    String stateTopic = String(tank_topic) + "/state";
    s_mqtt.publish(stateTopic.c_str(), payload.c_str());
    tlog("[MQTT] tank state published\n");
}

void tank_publish_health() {
    JsonDocument doc;
    doc["fault"]               = tankFaulted();
    doc["consecutiveFailures"] = tankConsecutiveFailures;
    doc["secondsSinceGood"]    = tankSecondsSinceGood();

    String payload;
    if (serializeJson(doc, payload) == 0 || payload.length() == 0) return;
    String healthTopic = String(tank_topic) + "/health";
    s_mqtt.publish(healthTopic.c_str(), payload.c_str(), true);   // retained
}


// ─────────────────────────────────────────────────────────────────────────────
// v1.3: timers + statistics -> MQTT (retained, so HA has them after a restart)
// ─────────────────────────────────────────────────────────────────────────────
String timer_text(const PumpTimer &t) {
    if (!t.enabled) return "Disabled";
    char buf[40];
    snprintf(buf, sizeof(buf), "%02u:%02u-%02u:%02u %s",
             t.start_h, t.start_m, t.stop_h, t.stop_m, speed_name((SpeedLevel)t.speed));
    return String(buf);
}

void publish_extra(const ChlorinatorExtra &x) {
    if (!s_mqtt.connected()) return;
    JsonDocument doc;
    if (x.timers_valid) {
        for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
            char key[8];
            snprintf(key, sizeof(key), "timer%d", i + 1);
            doc[key] = timer_text(x.timers[i]);
            // v1.17: separate fields for the HA timer controls
            const PumpTimer &t = x.timers[i];
            char k[12], hm[6];
            snprintf(k, sizeof(k), "t%d_speed", i + 1);
            doc[k] = speed_name((SpeedLevel)t.speed);
            snprintf(k, sizeof(k), "t%d_start", i + 1);
            snprintf(hm, sizeof(hm), "%02u:%02u", t.start_h, t.start_m);
            doc[k] = hm;
            snprintf(k, sizeof(k), "t%d_stop", i + 1);
            snprintf(hm, sizeof(hm), "%02u:%02u", t.stop_h, t.stop_m);
            doc[k] = hm;
            snprintf(k, sizeof(k), "t%d_on", i + 1);
            doc[k] = (i == 0) ? true : t.enabled;   // timer 1 always on
        }
    }
    if (x.setup_valid) doc["chlorine_setpoint"] = x.chlorine_setpoint;   // v1.4
    if (x.stats_valid) {
        doc["cell_hours"]          = x.cell_hours;
        doc["low_salt_hours"]      = x.low_salt_hours;
        doc["cell_reversal_count"] = x.cell_reversal_count;
    }
    if (doc.size() == 0) return;
    String payload;
    serializeJson(doc, payload);
    s_mqtt.publish(s_topic_extra, payload.c_str(), /*retain=*/true);
    tlog("[MQTT] timers/statistics published\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT callbacks and reconnect
// ─────────────────────────────────────────────────────────────────────────────
// v1.4: allowed chlorine output range (from the chlorinator's Capabilities,
// falling back to 0-8, the usual manual output scale)
void chlorineRange(int &mn, int &mx) {
    mn = 0; mx = 8;
    if (g_has_extra && g_last_extra.caps_valid && g_last_extra.chlor_max > g_last_extra.chlor_min) {
        mn = g_last_extra.chlor_min;
        mx = g_last_extra.chlor_max;
    }
}

// v1.4: HA "number" entity for the chlorine output setpoint. Re-sent whenever
// the range read from the chlorinator changes, so the slider limits match.
void publish_setpoint_discovery() {
    if (!s_mqtt.connected()) return;
    int mn, mx;
    chlorineRange(mn, mx);

    JsonDocument doc;
    doc["name"]           = "Chlorine Output";
    doc["command_topic"]  = s_topic_chlorine_set;
    doc["state_topic"]    = s_topic_extra;
    doc["value_template"] = "{{ value_json.chlorine_setpoint }}";
    doc["min"]            = mn;
    doc["max"]            = mx;
    doc["step"]           = 1;
    doc["mode"]           = "slider";
    doc["icon"]           = "mdi:beaker-check-outline";
    doc["qos"]            = 1;
    char uid[80];
    snprintf(uid, sizeof(uid), "%s_chlorine_setpoint", s_device_id);
    doc["unique_id"] = uid;

    char dev_name[48];
    snprintf(dev_name, sizeof(dev_name), "Chlorinator %s", chlorinator_name);
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"]  = s_device_id;
    dev["name"] = dev_name;
    dev["mf"]   = "Open Source";
    dev["mdl"]  = "MQTT Gateway for Astral Pool";

    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/number/%s/chlorine_setpoint/config", s_device_id);
    String payload;
    serializeJson(doc, payload);
    s_mqtt.publish(topic, payload.c_str(), true);
    tlog("[MQTT] chlorine setpoint entity published (range %d-%d)\n", mn, mx);
}

// v1.6: parse {"timer":1,"start":"08:50","stop":"15:10","speed":"medium","enabled":true}
static bool parse_hhmm(const char *s, uint8_t &h, uint8_t &m) {
    int hh, mm;
    if (!s || sscanf(s, "%d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    h = (uint8_t)hh; m = (uint8_t)mm;
    return true;
}

bool parse_timer_command(const byte *payload, unsigned int length, TimerSetCmd *out) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) { tlog("[MQTT] timer_set: bad JSON\n"); return false; }
    TimerSetCmd c{};
    int t = doc["timer"] | 0;
    if (t < 1 || t > NUM_PUMP_TIMERS) { tlog("[MQTT] timer_set: \"timer\" must be 1-%d\n", NUM_PUMP_TIMERS); return false; }
    c.index = (uint8_t)(t - 1);

    if (!doc["start"].isNull()) {
        if (!parse_hhmm(doc["start"].as<const char *>(), c.start_h, c.start_m)) { tlog("[MQTT] timer_set: bad start (use HH:MM)\n"); return false; }
        c.has_start = true;
    }
    if (!doc["stop"].isNull()) {
        if (!parse_hhmm(doc["stop"].as<const char *>(), c.stop_h, c.stop_m)) { tlog("[MQTT] timer_set: bad stop (use HH:MM)\n"); return false; }
        c.has_stop = true;
    }
    if (!doc["speed"].isNull()) {
        int sp = -1;
        if (doc["speed"].is<int>()) sp = doc["speed"].as<int>();
        else {
            String v = doc["speed"].as<String>(); v.toLowerCase();
            if      (v == "low")    sp = SPEED_LOW;
            else if (v == "medium") sp = SPEED_MEDIUM;
            else if (v == "high")   sp = SPEED_HIGH;
            // AI (3) deliberately not accepted - not confirmed on this chlorinator
        }
        if (sp < SPEED_LOW || sp > SPEED_HIGH) { tlog("[MQTT] timer_set: bad speed (low/medium/high)\n"); return false; }
        c.speed = (uint8_t)sp; c.has_speed = true;
    }
    if (!doc["enabled"].isNull()) { c.enabled = doc["enabled"].as<bool>(); c.has_enabled = true; }
    if (c.index == 0 && c.has_enabled && !c.enabled) {
        tlog("[MQTT] timer_set: timer 1 must stay on - ignored\n"); return false;
    }

    if (!c.has_start && !c.has_stop && !c.has_speed && !c.has_enabled) {
        tlog("[MQTT] timer_set: nothing to change\n"); return false;
    }
    *out = c;
    return true;
}

// v1.18: fold a change into a pending one (later fields win, others kept)
void merge_timer_cmd(TimerSetCmd &dst, const TimerSetCmd &src) {
    if (!dst.active) { dst = src; dst.active = true; return; }
    if (src.has_start)   { dst.has_start = true;   dst.start_h = src.start_h; dst.start_m = src.start_m; }
    if (src.has_stop)    { dst.has_stop = true;    dst.stop_h  = src.stop_h;  dst.stop_m  = src.stop_m;  }
    if (src.has_speed)   { dst.has_speed = true;   dst.speed   = src.speed; }
    if (src.has_enabled) { dst.has_enabled = true; dst.enabled = src.enabled; }
    dst.seq = src.seq;
}

// v1.18 (BLE task): move any new requests into the task's own batch
bool take_timer_requests(TimerSetCmd *batch) {
    if (!g_timer_req_flag.exchange(false)) return false;
    bool any = false;
    portENTER_CRITICAL(&g_timer_mux);
    for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
        if (g_timer_req[i].active) {
            merge_timer_cmd(batch[i], g_timer_req[i]);
            g_timer_req[i] = TimerSetCmd{};
            any = true;
        }
    }
    portEXIT_CRITICAL(&g_timer_mux);
    return any;
}

// v1.7: queue a timer write and show the new values straight away.
// v1.18: never refused - changes made while a save is running are merged
// and written as soon as the BLE task is free. Runs on loop().
bool queue_timer_write(TimerSetCmd c) {
    if (c.index >= NUM_PUMP_TIMERS) return false;
    if (c.index == 0) { c.has_enabled = true; c.enabled = true; }   // timer 1 always on
    int i = c.index;
    if (++g_tseq[i] == 0) g_tseq[i] = 1;
    c.seq    = g_tseq[i];
    c.active = true;

    if (g_has_extra && g_last_extra.timers_valid) {
        PumpTimer pt = g_tpend[i] ? g_tdesired[i] : g_tactual[i];
        if (c.has_start)   { pt.start_h = c.start_h; pt.start_m = c.start_m; }
        if (c.has_stop)    { pt.stop_h  = c.stop_h;  pt.stop_m  = c.stop_m;  }
        if (c.has_speed)   pt.speed   = c.speed;
        if (c.has_enabled) pt.enabled = c.enabled;
        g_tdesired[i]  = pt;
        g_tpend[i]     = true;
        g_tpend_ms[i]  = millis();
        g_last_extra.timers[i] = pt;
        g_extra_publish_pending = true;   // HA shows the new value at once
    }
    g_timer_done_idx = -1;

    portENTER_CRITICAL(&g_timer_mux);
    merge_timer_cmd(g_timer_req[i], c);
    portEXIT_CRITICAL(&g_timer_mux);
    g_timer_req_flag.store(true);

    // Wake the BLE task - but never overwrite an action that's waiting
    // (if the queue is full the BLE task is about to wake anyway)
    int cmd = TIMER_CMD_CODE;
    xQueueSend(g_action_queue, &cmd, 0);
    return true;
}

void on_mqtt_message(char *topic, byte *payload, unsigned int length) {
    // v1.7: pause BLE for N minutes (0 = resume)
    if (strcmp(topic, s_topic_ble_pause) == 0) {
        char b[12];
        unsigned int n = length < sizeof(b) - 1 ? length : sizeof(b) - 1;
        memcpy(b, payload, n); b[n] = '\0';
        long mins = atol(b);
        if (mins < 0) mins = 0;
        if (mins > 240) mins = 240;
        ble_pause((unsigned long)mins, 1);
        tlog("[MQTT] BLE %s\n", mins ? "paused" : "resumed");
        return;
    }
    // v1.6: timer write (JSON, longer than the 16-byte buffer below)
    if (strcmp(topic, s_topic_timer_set) == 0) {
        TimerSetCmd c;
        if (parse_timer_command(payload, length, &c)) {
            if (queue_timer_write(c)) tlog("[MQTT] timer %d write queued\n", c.index + 1);
            else tlog("[MQTT] timer write ignored - previous save still in progress\n");
        }
        return;
    }

    char buf[16];
    if (length == 0 || length >= sizeof(buf)) return;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    // v1.4: chlorine output setpoint from the HA number entity
    if (strcmp(topic, s_topic_chlorine_set) == 0) {
        int mn, mx;
        chlorineRange(mn, mx);
        int v = (int)lroundf(atof(buf));      // HA may send "5" or "5.0"
        if (v >= mn && v <= mx) {
            int cmd = SETPOINT_CMD_BASE + v;
            xQueueOverwrite(g_action_queue, &cmd);
            tlog("[MQTT] chlorine setpoint received: %d\n", v);
        } else {
            tlog("[MQTT] chlorine setpoint %d out of range %d-%d, ignored\n", v, mn, mx);
        }
        return;
    }

    int action = atoi(buf);
    if (action >= ACTION_NO_ACTION && action <= ACTION_TRIGGER_CELL_REVERSAL) {
        xQueueOverwrite(g_action_queue, &action);
        tlog("[MQTT] action received: %d\n", action);

        // v1.12: show "Setting to ..." in HA straight away for mode changes
        int target = (action == ACTION_OFF)    ? MODE_OFF    :
                     (action == ACTION_MANUAL) ? MODE_MANUAL :
                     (action == ACTION_AUTO)   ? MODE_AUTO   : -1;
        g_mode_pending    = target;            // any other action replaces a queued mode change
        g_mode_pending_ms = millis();

        // v1.16: same for pump speed. Manual follows Timer 1's speed (v1.13),
        // so show that as the target if it is known and not AI.
        int spd = (action == ACTION_LOW)    ? SPEED_LOW    :
                  (action == ACTION_MEDIUM) ? SPEED_MEDIUM :
                  (action == ACTION_HIGH)   ? SPEED_HIGH   : -1;
        if (action == ACTION_MANUAL && g_has_extra && g_last_extra.timers_valid &&
            g_last_extra.timers[0].speed <= SPEED_HIGH) {
            spd = g_last_extra.timers[0].speed;
        }
        g_speed_pending    = spd;              // any other action clears a pending speed
        g_speed_pending_ms = millis();
        if (g_has_last_state) {                // republish last state with the pending flag
            g_chlor_pending_state   = g_last_state;
            g_chlor_publish_pending = true;
        }
    } else {
        tlog("[MQTT] unknown action payload: '%s'\n", buf);
    }
}

// One MQTT connection for both devices. The Last Will goes on the tank's
// availability topic (as in the original tank sketch); the chlorinator never
// used one, so its entities are unchanged.
void mqtt_reconnect() {
    static unsigned long s_last_attempt = 0;
    if (s_last_attempt != 0 && millis() - s_last_attempt < 5000) return;
    s_last_attempt = millis();

    if (strlen(mqtt_server) == 0) return;   // not configured yet

    char client_id[48];
    // v1.15: fixed ID (was random) - the broker then replaces the old session
    // straight away, so its Last Will can't overwrite our new "online"
    snprintf(client_id, sizeof(client_id), "pool-tank-gw-%08x",
             (unsigned)(ESP.getEfuseMac() >> 16));

    String availTopic = tankAvailTopic();

    tlog("[MQTT] connecting to %s:%d ...\n", mqtt_server, mqtt_port);
    bool hasUser = strlen(mqtt_user) > 0;
    bool ok = s_mqtt.connect(client_id,
                             hasUser ? mqtt_user : nullptr,
                             hasUser ? mqtt_pass : nullptr,
                             availTopic.c_str(), 1, true, "offline");

    if (ok) {
        tlog("[MQTT] connected\n");
        s_mqtt.publish(availTopic.c_str(), "online", true);
        s_mqtt.subscribe(s_topic_action);
        s_mqtt.subscribe(s_topic_chlorine_set);   // v1.4
        s_mqtt.subscribe(s_topic_timer_set);      // v1.6
        s_mqtt.subscribe(s_topic_ble_pause);      // v1.7
        publish_autodiscovery();      // device 1: chlorinator
        if (g_has_extra) g_extra_publish_pending = true;
        tank_publish_discovery();     // device 2: water tank
    } else {
        tlog("[MQTT] failed, rc=%d, retry in 5s\n", s_mqtt.state());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE: scan, connect, authenticate, read state, optionally write action
// (from ChlorinatorGatewayWeb v1.02; connect timeout and ble_task changed in v1.2)
// ─────────────────────────────────────────────────────────────────────────────
bool ble_scan() {
    tlog("[BLE] scanning for %s ...\n", chlorinator_mac);
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    unsigned long scanMs = (unsigned long)ble_scan_duration_sec * 1000UL;
    NimBLEScanResults results = pScan->getResults(scanMs, /*is_continue=*/false);
    tlog("[BLE] scan complete, %d device(s) found\n", results.getCount());

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
        String addr = String(dev->getAddress().toString().c_str());
        String name = String(dev->getName().c_str());
        tlog("[BLE]   %s  \"%s\"\n", addr.c_str(), name.c_str());

        bool match_mac  = !String(chlorinator_mac).isEmpty() &&
                           addr.equalsIgnoreCase(chlorinator_mac);
        bool match_name = name.equals(chlorinator_name);

        if (match_mac || match_name) {
            g_ble_address     = dev->getAddress();
            g_has_ble_address = true;
            tlog("[BLE] device found: %s (%s)\n", addr.c_str(), name.c_str());
            pScan->clearResults();
            return true;
        }
    }
    pScan->clearResults();
    tlog("[BLE] device not found\n");
    return false;
}

// Connects to the chlorinator, optionally sends an action, reads and decrypts
// state into out_state. Returns true on success.
static bool timers_read(NimBLERemoteCharacteristic *pT, const uint8_t *session_key, uint8_t *out);   // v1.13

bool ble_operate(ChlorinatorAction action, ChlorinatorState *out_state, ChlorinatorExtra *out_extra, int set_chlorine, const TimerSetCmd *timer_cmd) {
    if (!g_has_ble_address) {
        if (!ble_scan()) return false;
    }

    NimBLEClient *pClient = NimBLEDevice::createClient();
    pClient->setConnectTimeout(6000);   // v1.2: was 15 s - fail fast, retry sooner

    tlog("[BLE] connecting to %s ...\n", g_ble_address.toString().c_str());
    bool connected = pClient->connect(g_ble_address);
    if (!connected) {
        tlog("[BLE] connection failed, re-scanning ...\n");
        NimBLEDevice::deleteClient(pClient);
        g_has_ble_address = false;
        // Chlorinator may need a moment to restart advertising after a previous
        // session disconnect - re-scan gives it that time implicitly.
        if (!ble_scan()) return false;

        pClient = NimBLEDevice::createClient();
        pClient->setConnectTimeout(6000);   // v1.2: was 15 s - fail fast, retry sooner
        tlog("[BLE] retrying connect to %s ...\n", g_ble_address.toString().c_str());
        connected = pClient->connect(g_ble_address);
        if (!connected) {
            tlog("[BLE] connection failed after re-scan\n");
            NimBLEDevice::deleteClient(pClient);
            g_has_ble_address = false;
            return false;
        }
    }
    tlog("[BLE] connected\n");

    bool success = false;

    NimBLERemoteService *pSvc = pClient->getService(UUID_ASTRALPOOL_SERVICE);
    if (!pSvc) {
        tlog("[BLE] Astral Pool service not found\n");
        goto disconnect;
    }

    {
        NimBLERemoteCharacteristic *pSK = pSvc->getCharacteristic(UUID_SLAVE_SESSION_KEY);
        if (!pSK || !pSK->canRead()) {
            tlog("[BLE] session key characteristic unavailable\n");
            goto disconnect;
        }

        std::string sk_raw = pSK->readValue();
        if (sk_raw.size() != 16) {
            tlog("[BLE] unexpected session key size: %d\n", (int)sk_raw.size());
            goto disconnect;
        }
        const uint8_t *session_key = (const uint8_t *)sk_raw.data();

        tlog("[BLE] session key: %02x%02x%02x...%02x\n",
             session_key[0], session_key[1], session_key[2], session_key[15]);

        NimBLERemoteCharacteristic *pAuth = pSvc->getCharacteristic(UUID_MASTER_AUTHENTICATION);
        if (!pAuth || !pAuth->canWrite()) {
            tlog("[BLE] auth characteristic unavailable\n");
            goto disconnect;
        }

        uint8_t mac_key[16];
        encrypt_mac_key(session_key,
                        (const uint8_t *)chlorinator_code, strlen(chlorinator_code),
                        mac_key);

        if (!pAuth->writeValue(mac_key, 16, /*response=*/true)) {
            tlog("[BLE] auth write failed\n");
            goto disconnect;
        }
        tlog("[BLE] authenticated\n");

        if (action != ACTION_NO_ACTION) {
            NimBLERemoteCharacteristic *pAct =
                pSvc->getCharacteristic(UUID_CHLORINATOR_APP_ACTION);
            if (pAct && pAct->canWrite()) {
                // v1.13: Manual -> follow up with Timer 1's pump speed
                int follow = ACTION_NO_ACTION;
                if (action == ACTION_MANUAL) {
                    NimBLERemoteCharacteristic *pT = pSvc->getCharacteristic(UUID_CHLORINATOR_TIMERS);
                    uint8_t td[CHAR_DATA_LEN];
                    if (pT && pT->canRead() && timers_read(pT, session_key, td)) {
                        uint8_t sp = (td[0] & 0xC0) >> 6;          // timer 1 speed bits
                        if (sp <= SPEED_HIGH) {
                            follow = ACTION_LOW + sp;              // 4 Low, 5 Medium, 6 High
                            tlog("[BLE] manual: timer 1 speed is %s\n", speed_name((SpeedLevel)sp));
                        } else {
                            tlog("[BLE] manual: timer 1 speed is AI - leaving default\n");
                        }
                    } else {
                        tlog("[BLE] manual: timer 1 not readable - leaving default speed\n");
                    }
                }

                int to_send[2] = { (int)action, follow };
                for (int k = 0; k < 2; k++) {
                    if (to_send[k] == ACTION_NO_ACTION) continue;
                    ChlorinatorActionPacket pkt;
                    build_action_packet((ChlorinatorAction)to_send[k], 0, &pkt);

                    uint8_t enc[CHAR_DATA_LEN];
                    encrypt_characteristic((const uint8_t *)&pkt, session_key, enc);

                    if (pAct->writeValue(enc, CHAR_DATA_LEN, /*response=*/true)) {
                        tlog("[BLE] action %d sent\n", to_send[k]);
                    } else {
                        tlog("[BLE] action %d write failed\n", to_send[k]);
                    }
                    delay(500);  // Let the device process the action before the next step
                }
            }
        }

        NimBLERemoteCharacteristic *pState =
            pSvc->getCharacteristic(UUID_CHLORINATOR_STATE);
        if (!pState || !pState->canRead()) {
            tlog("[BLE] state characteristic unavailable\n");
            goto disconnect;
        }

        std::string state_raw = pState->readValue();
        if ((int)state_raw.size() < CHAR_DATA_LEN) {
            tlog("[BLE] state too short: %d bytes\n", (int)state_raw.size());
            goto disconnect;
        }

        uint8_t decrypted[CHAR_DATA_LEN];
        decrypt_characteristic((const uint8_t *)state_raw.data(), session_key, decrypted);
        parse_chlorinator_state(decrypted, out_state);
        success = true;

        // v1.3: every few minutes, also read timers + statistics
        // v1.4: chlorine setpoint write (read-modify-write of Setup), then the
        // extras read below reads it straight back for HA
        if (set_chlorine >= 0) {
            if (!ble_write_chlorine_setpoint(pSvc, session_key, set_chlorine)) success = false;
        }
        // v1.6: pump timer write (read-modify-write + verify)
        if (timer_cmd) {
            if (!ble_write_timer(pSvc, session_key, timer_cmd)) success = false;   // v1.18: batch of 4
        }

        if (out_extra) ble_read_extra(pSvc, session_key, out_extra);

        tlog("[BLE] state: mode=%d priming=%s pump=%s cell=%s\n",
            out_state->mode,
            out_state->pump_priming   ? "ON"  : "OFF",
            out_state->pump_operating ? "ON"  : "OFF",
            out_state->cell_operating ? "ON"  : "OFF");
    }

disconnect:
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);

    if (!success) {
        g_has_ble_address = false;  // Force re-scan on next attempt
    }
    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE task - polls the chlorinator independently of the main loop
// ─────────────────────────────────────────────────────────────────────────────
// v1.3: read + decrypt the timers and statistics characteristics.
// Failures here are logged but never fail the poll itself.
void ble_read_extra(NimBLERemoteService *pSvc, const uint8_t *session_key, ChlorinatorExtra *out) {
    out->timers_valid = false;
    out->stats_valid  = false;
    uint8_t d[CHAR_DATA_LEN];

    // Timers: 4 x {start_hour|flags, start_min, stop_hour, stop_min}
    //   start_hour|flags: bits 0-4 hour, bit 5 enabled, bits 6-7 speed
    NimBLERemoteCharacteristic *pT = pSvc->getCharacteristic(UUID_CHLORINATOR_TIMERS);
    if (pT && pT->canRead()) {
        std::string raw = pT->readValue();
        if ((int)raw.size() >= CHAR_DATA_LEN) {
            decrypt_characteristic((const uint8_t *)raw.data(), session_key, d);
            for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
                uint8_t b0 = d[i * 4];
                out->timers[i].start_h = b0 & 0x1F;
                out->timers[i].enabled = (b0 & 0x20) != 0;
                out->timers[i].speed   = (b0 & 0xC0) >> 6;
                out->timers[i].start_m = d[i * 4 + 1];
                out->timers[i].stop_h  = d[i * 4 + 2];
                out->timers[i].stop_m  = d[i * 4 + 3];
            }
            out->timers_valid = true;
        } else {
            tlog("[BLE] timers too short: %d bytes\n", (int)raw.size());
        }
    } else {
        tlog("[BLE] timers characteristic unavailable\n");
    }

    // Statistics (little-endian, native alignment, as pychlorinator "@BBHHHIIB"):
    //   [0] highest pH  [1] lowest pH  [2-3] highest ORP  [4-5] lowest ORP
    //   [6-7] cell reversal count  [8-11] cell running hours
    //   [12-15] low-salt cell running hours  [16] previous day's cell load
    NimBLERemoteCharacteristic *pS = pSvc->getCharacteristic(UUID_CHLORINATOR_STATISTICS);
    if (pS && pS->canRead()) {
        std::string raw = pS->readValue();
        if ((int)raw.size() >= CHAR_DATA_LEN) {
            decrypt_characteristic((const uint8_t *)raw.data(), session_key, d);
            out->cell_reversal_count = (uint16_t)(d[6] | (d[7] << 8));
            out->cell_hours     = (uint32_t)d[8]  | ((uint32_t)d[9]  << 8) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
            out->low_salt_hours = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
            out->stats_valid = true;
        } else {
            tlog("[BLE] statistics too short: %d bytes\n", (int)raw.size());
        }
    } else {
        tlog("[BLE] statistics characteristic unavailable\n");
    }

    // v1.4 Capabilities: [2] min manual chlorine, [3] max manual chlorine,
    //                    [9] chlorine control type
    out->caps_valid  = false;
    out->setup_valid = false;
    NimBLERemoteCharacteristic *pC = pSvc->getCharacteristic(UUID_CHLORINATOR_CAPABILITIES);
    if (pC && pC->canRead()) {
        std::string raw = pC->readValue();
        if ((int)raw.size() >= CHAR_DATA_LEN) {
            decrypt_characteristic((const uint8_t *)raw.data(), session_key, d);
            out->chlor_min          = d[2];
            out->chlor_max          = d[3];
            out->chlor_control_type = d[9];
            out->caps_valid = true;
        }
    }
    // v1.4 Setup: [0] default manual speed, [1] pH setpoint, [2-3] chlorine
    //             setpoint (uint16 LE), [4] flags
    NimBLERemoteCharacteristic *pU = pSvc->getCharacteristic(UUID_CHLORINATOR_SETUP);
    if (pU && pU->canRead()) {
        std::string raw = pU->readValue();
        if ((int)raw.size() >= CHAR_DATA_LEN) {
            decrypt_characteristic((const uint8_t *)raw.data(), session_key, d);
            out->chlorine_setpoint = (uint16_t)(d[2] | (d[3] << 8));
            out->setup_valid = true;
        }
    }
    tlog("[BLE] chlorine setpoint %s: %u (range %u-%u, control type %u)\n",
         out->setup_valid ? "ok" : "--", out->chlorine_setpoint,
         out->chlor_min, out->chlor_max, out->chlor_control_type);

    tlog("[BLE] extras: timers %s, stats %s (cell %lu h, low-salt %lu h)\n",
         out->timers_valid ? "ok" : "--", out->stats_valid ? "ok" : "--",
         (unsigned long)out->cell_hours, (unsigned long)out->low_salt_hours);
}

// v1.4: write a new chlorine output setpoint. Mirrors pychlorinator's
// async_write_setup(): read Setup, keep default speed / pH setpoint / flags,
// replace the chlorine setpoint, zero-pad to 20 bytes, encrypt, write.
bool ble_write_chlorine_setpoint(NimBLERemoteService *pSvc, const uint8_t *session_key, int value) {
    NimBLERemoteCharacteristic *pU = pSvc->getCharacteristic(UUID_CHLORINATOR_SETUP);
    if (!pU || !pU->canRead() || !pU->canWrite()) {
        tlog("[BLE] setup characteristic unavailable/not writable\n");
        return false;
    }
    std::string raw = pU->readValue();
    if ((int)raw.size() < CHAR_DATA_LEN) {
        tlog("[BLE] setup too short: %d bytes\n", (int)raw.size());
        return false;
    }
    uint8_t cur[CHAR_DATA_LEN];
    decrypt_characteristic((const uint8_t *)raw.data(), session_key, cur);

    uint8_t plain[CHAR_DATA_LEN] = {0};
    plain[0] = cur[0];                    // default manual-on speed (unchanged)
    plain[1] = cur[1];                    // pH setpoint (unchanged)
    plain[2] = (uint8_t)(value & 0xFF);   // chlorine setpoint, little-endian
    plain[3] = (uint8_t)((value >> 8) & 0xFF);
    plain[4] = cur[4];                    // flags (unchanged)

    uint8_t enc[CHAR_DATA_LEN];
    encrypt_characteristic(plain, session_key, enc);
    bool ok = pU->writeValue(enc, CHAR_DATA_LEN, /*response=*/true);
    tlog("[BLE] chlorine setpoint %u -> %d %s\n", (unsigned)(cur[2] | (cur[3] << 8)), value, ok ? "written" : "WRITE FAILED");
    delay(300);   // let the chlorinator apply it before we read it back
    return ok;
}

// v1.6: write one pump timer.
// Timers characteristic (45000204), decrypted, first 16 bytes = 4 timers x 4:
//   [0] bits 0-4 start hour, bit 5 enabled, bits 6-7 speed
//   [1] start minute  [2] stop hour  [3] stop minute
// Read-modify-write so the other timers keep their exact bytes, zero-pad to
// 20 bytes (as the phone app does), encrypt, write, then read back and check.
//
// Quirk seen in the phone app's own BLE traffic: the chlorinator sometimes
// stores a stop-minute byte with some bits flipped (e.g. 15:10 -> 15:30). If
// the read-back differs only in stop-minute bytes, we XOR-compensate and write
// once more. Anything else that differs is logged as a failure.
static bool timers_read(NimBLERemoteCharacteristic *pT, const uint8_t *session_key, uint8_t *out) {
    std::string raw = pT->readValue();
    if ((int)raw.size() < CHAR_DATA_LEN) {
        tlog("[BLE] timers read too short: %d bytes\n", (int)raw.size());
        return false;
    }
    decrypt_characteristic((const uint8_t *)raw.data(), session_key, out);
    return true;
}

static bool timers_write(NimBLERemoteCharacteristic *pT, const uint8_t *session_key, const uint8_t *block16) {
    uint8_t plain[CHAR_DATA_LEN] = {0};
    memcpy(plain, block16, 16);
    uint8_t enc[CHAR_DATA_LEN];
    encrypt_characteristic(plain, session_key, enc);
    bool ok = pT->writeValue(enc, CHAR_DATA_LEN, /*response=*/true);
    delay(300);   // let the chlorinator apply it before reading back
    return ok;
}

// v1.18: writes every changed timer in `batch` (4 slots) in one go and
// verifies only the bits that were changed.
bool ble_write_timer(NimBLERemoteService *pSvc, const uint8_t *session_key, const TimerSetCmd *batch) {
    NimBLERemoteCharacteristic *pT = pSvc->getCharacteristic(UUID_CHLORINATOR_TIMERS);
    if (!pT || !pT->canRead() || !pT->canWrite()) {
        tlog("[BLE] timers characteristic unavailable/not writable\n");
        return false;
    }
    uint8_t cur[CHAR_DATA_LEN];
    if (!timers_read(pT, session_key, cur)) return false;

    uint8_t want[16], mask[16] = {0};
    memcpy(want, cur, 16);
    for (int n = 0; n < NUM_PUMP_TIMERS; n++) {
        const TimerSetCmd &cmd = batch[n];
        if (!cmd.active) continue;
        uint8_t *t = &want[n * 4];
        uint8_t *m = &mask[n * 4];
        uint8_t b0 = t[0];
        if (cmd.has_start)   { b0 = (b0 & 0xE0) | (cmd.start_h & 0x1F); t[1] = cmd.start_m; m[0] |= 0x1F; m[1] = 0xFF; }
        if (cmd.has_enabled) { b0 = cmd.enabled ? (b0 | 0x20) : (b0 & ~0x20); m[0] |= 0x20; }
        if (cmd.has_speed)   { b0 = (b0 & 0x3F) | ((cmd.speed & 0x03) << 6); m[0] |= 0xC0; }
        t[0] = b0;
        if (cmd.has_stop)    { t[2] = cmd.stop_h; t[3] = cmd.stop_m; m[2] = 0xFF; m[3] = 0xFF; }
        tlog("[BLE] timer %d: %02x %02x %02x %02x -> %02x %02x %02x %02x\n", n + 1,
             cur[n*4], cur[n*4+1], cur[n*4+2], cur[n*4+3], t[0], t[1], t[2], t[3]);
    }

    uint8_t send[16];
    memcpy(send, want, 16);
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (!timers_write(pT, session_key, send)) {
            tlog("[BLE] timer write failed\n");
            return false;
        }
        uint8_t got[CHAR_DATA_LEN];
        if (!timers_read(pT, session_key, got)) return false;

        bool ok = true, only_stop_min = true;
        for (int i = 0; i < 16; i++) {
            if ((got[i] & mask[i]) != (want[i] & mask[i])) {
                ok = false;
                tlog("[BLE] timer verify: byte %d wanted %02x got %02x\n", i, want[i], got[i]);
                if ((i & 3) != 3) only_stop_min = false;
            }
        }
        if (ok) {
            tlog("[BLE] timer change written and verified (attempt %d)\n", attempt);
            return true;
        }
        if (!only_stop_min || attempt == 2) break;
        // Chlorinator quirk: it sometimes alters a stop-minute byte - compensate once
        for (int i = 3; i < 16; i += 4)
            if (mask[i]) send[i] ^= (uint8_t)(got[i] ^ want[i]);
        tlog("[BLE] stop-minute changed by chlorinator - compensating and re-writing\n");
    }
    tlog("[BLE] timer change NOT verified - check the app\n");
    return false;
}

void ble_task(void *) {
    static ChlorinatorState state{};
    unsigned long mqtt_last_seen = 0;  // millis() timestamp of last MQTT connection

    // v1.2: faster, more reliable actions
    //  - an action from HA is kept until it is actually delivered, and retried
    //    (2 s apart, up to ACTION_MAX_ATTEMPTS) if the BLE connection fails -
    //    e.g. because the phone app happens to be connected at that moment
    //  - after a successful action, two quick "confirm" polls run 3 s apart so
    //    HA shows the new mode within seconds instead of waiting a full poll
    //    interval. Normal polling then resumes at poll_interval_sec.
    const int           ACTION_MAX_ATTEMPTS = 3;
    const unsigned long ACTION_RETRY_MS     = 2000;
    const unsigned long CONFIRM_POLL_MS     = 3000;
    const int           CONFIRM_POLLS       = 2;

    int pending_action  = ACTION_NO_ACTION;
    int action_attempts = 0;
    // v1.18: timer changes waiting to be written (merged), with their own retries
    static TimerSetCmd timer_batch[NUM_PUMP_TIMERS];
    bool timer_batch_pending = false;
    int  timer_attempts      = 0;
    int confirm_polls   = 0;

    // v1.3: timers/statistics rarely change - read them on the first poll,
    // then every EXTRA_INTERVAL_MS, and after any action.
    const unsigned long EXTRA_INTERVAL_MS = 5UL * 60 * 1000;
    unsigned long last_extra_ms = 0;
    bool          extra_due     = true;
    static ChlorinatorExtra extra{};

    while (true) {
        // Pause during OTA - wait for main loop to signal completion
        if (g_ota_in_progress.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Wait for a new action, or until the next poll / retry is due
        // v1.7: while paused, sleep until the pause ends (a command wakes us)
        unsigned long waitMs;
        long pauseLeft = ble_pause_left_ms();
        if (pending_action != ACTION_NO_ACTION || timer_batch_pending) waitMs = ACTION_RETRY_MS;
        else if (confirm_polls > 0)             waitMs = CONFIRM_POLL_MS;
        else if (pauseLeft > 0)                 waitMs = (unsigned long)pauseLeft;
        else                                    waitMs = (unsigned long)poll_interval_sec * 1000UL;
        // v1.7: first poll 2 s after boot so the web page and HA fill straight away
        static bool first_poll = true;
        if (first_poll && pending_action == ACTION_NO_ACTION) { waitMs = 2000; first_poll = false; }

        int incoming = ACTION_NO_ACTION;
        bool scheduled = false;              // v1.7: true = routine poll, not a command
        if (xQueueReceive(g_action_queue, &incoming, pdMS_TO_TICKS(waitMs)) == pdTRUE &&
            incoming != ACTION_NO_ACTION) {
            if (incoming != TIMER_CMD_CODE) {      // v1.18: timer changes come from g_timer_req
                pending_action  = incoming;        // newest action wins
                action_attempts = 0;
            }
            confirm_polls   = 0;
            if (g_ble_paused.load()) tlog("[BLE] command received - pause cancelled\n");
            ble_pause(0, 0);                // user is using the gateway, not the app
        } else if (pending_action == ACTION_NO_ACTION && !timer_batch_pending && confirm_polls > 0) {
            confirm_polls--;
        } else if (pending_action == ACTION_NO_ACTION && !timer_batch_pending) {
            scheduled = true;
        }
        // v1.18: pick up timer changes (any number, merged into one write)
        if (take_timer_requests(timer_batch)) {
            if (!timer_batch_pending) timer_attempts = 0;
            timer_batch_pending = true;
            scheduled = false;
            ble_pause(0, 0);
        }

        // v1.11: from here to the end of this pass the BLE task is "busy";
        // the supervisor restarts the ESP32 if that lasts over 5 minutes.
        BleBusy busy;

        // v1.7: routine polls respect the pause and check the app isn't connected
        static int skip_streak = 0;
        if (scheduled) {
            if (ble_pause_left_ms() > 0) continue;          // woke early, still paused
            extra_due = true;                              // read everything each poll
            if (!ble_scan()) {
                // Not advertising -> most likely the phone app is connected
                ble_pause((unsigned long)app_backoff_min, 2);
                skip_streak++;
                tlog("[BLE] chlorinator not advertising (app connected?) - backing off %d min\n",
                     app_backoff_min);
                if (skip_streak > SKIPS_BEFORE_FAULT) g_ble_consecutive_failures.fetch_add(1);
                continue;
            }
        }

        if (g_mqtt_connected.load()) {
            mqtt_last_seen = millis();
        }

        int sending = pending_action;
        // v1.4: codes >= SETPOINT_CMD_BASE are chlorine setpoint writes, not actions
        bool is_setp  = (sending >= SETPOINT_CMD_BASE && sending < SETPOINT_CMD_BASE + 1000);
        int act   = is_setp ? ACTION_NO_ACTION : sending;
        int set_c = is_setp ? sending - SETPOINT_CMD_BASE : -1;
        bool is_timer = timer_batch_pending;     // v1.18
        if (is_timer) extra_due = true;          // read the timers straight back for HA
        unsigned long t0 = millis();
        if (last_extra_ms != 0 && millis() - last_extra_ms >= EXTRA_INTERVAL_MS) extra_due = true;
        if (set_c >= 0) extra_due = true;       // read the new setpoint straight back
        bool read_extra = extra_due;
        // v1.18: never pass on stale values if this connection doesn't get as far as reading them
        extra.timers_valid = extra.stats_valid = extra.setup_valid = extra.caps_valid = false;
        bool ok = ble_operate((ChlorinatorAction)act, &state, read_extra ? &extra : nullptr, set_c,
                              is_timer ? timer_batch : nullptr);

        // v1.18: tell loop() which timer changes are finished (with the read-back)
        for (int i = 0; i < NUM_PUMP_TIMERS; i++) extra.timer_done_seq[i] = 0;
        extra.timer_batch_failed = false;
        bool batch_finished = false;
        if (is_timer) {
            if (ok) {
                batch_finished = true;
            } else if (++timer_attempts >= ACTION_MAX_ATTEMPTS) {
                batch_finished = true;
                extra.timer_batch_failed = true;
                tlog("[BLE] giving up on timer change after %d attempts\n", timer_attempts);
            }
            tlog("[BLE] timer change %s after %lu ms (attempt %d)\n",
                 ok ? "delivered" : "FAILED", millis() - t0, timer_attempts + (ok ? 1 : 0));
            if (batch_finished) {
                for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
                    if (timer_batch[i].active) extra.timer_done_seq[i] = timer_batch[i].seq;
                    timer_batch[i] = TimerSetCmd{};
                }
                timer_batch_pending = false;
                timer_attempts      = 0;
            }
        }
        if (read_extra) {
            if (ok) {
                extra_due     = false;
                last_extra_ms = millis();
            }
            // v1.18: also after a failed write, so HA shows what the chlorinator really has
            if (extra.timers_valid || extra.stats_valid || extra.setup_valid || batch_finished)
                xQueueOverwrite(g_extra_queue, &extra);
        }
        if (sending != ACTION_NO_ACTION) {
            tlog("[BLE] action %d %s after %lu ms (attempt %d)\n", sending,
                 ok ? "delivered" : "FAILED", millis() - t0, action_attempts + 1);
        }

        if (!ok && scheduled) {
            // v1.7: a routine poll that can't connect is most likely the app -
            // back off instead of hammering the chlorinator
            ble_pause((unsigned long)app_backoff_min, 2);
            skip_streak++;
            tlog("[BLE] routine poll failed - backing off %d min\n", app_backoff_min);
            if (skip_streak <= SKIPS_BEFORE_FAULT) continue;   // don't report a fault yet
        }
        if (ok) skip_streak = 0;

        if (ok) {
            g_ble_consecutive_failures.store(0);
            g_ble_last_good_ms.store(millis());

            if (sending != ACTION_NO_ACTION) {
                pending_action = ACTION_NO_ACTION;
                confirm_polls  = CONFIRM_POLLS;
                extra_due      = true;     // refresh timers/stats after an action
            }

            // Fallback: force Auto on next poll if MQTT has been absent for 30+ minutes
            bool mqtt_absent_30min = mqtt_last_seen != 0 &&
                                     millis() - mqtt_last_seen >= 30UL * 60 * 1000;
            if (state.mode != MODE_AUTO && mqtt_absent_30min) {
                int fallback = ACTION_AUTO;
                xQueueOverwrite(g_action_queue, &fallback);
            }
        } else {
            state.mode = MODE_BLE_ERROR;
            g_ble_consecutive_failures.fetch_add(1);
            if (sending != ACTION_NO_ACTION && ++action_attempts >= ACTION_MAX_ATTEMPTS) {
                tlog("[BLE] giving up on action %d after %d attempts\n", sending, action_attempts);
                pending_action = ACTION_NO_ACTION;
            }
        }
        xQueueOverwrite(g_state_queue, &state);
    }

    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA - pauses the BLE task and tank sampling for the duration of an update
// ─────────────────────────────────────────────────────────────────────────────
void setup_ota() {
    ArduinoOTA.setHostname(GATEWAY_HOSTNAME);

    ArduinoOTA.onStart([]() {
        quiesce_for_restart();          // v1.11: stamps the time for the stall check
        bool isSketch = (ArduinoOTA.getCommand() == U_FLASH);
        tlog("[OTA] update starting (%s)\n", isSketch ? "sketch" : "filesystem");
        if (!isSketch) LittleFS.end();
    });

    ArduinoOTA.onEnd([]() {
        tlog("[OTA] update complete, rebooting\n");
        g_ota_in_progress.store(false);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        g_ota_activity_ms.store(millis() | 1);   // v1.11: OTA still moving
        esp_task_wdt_reset();                    // v1.11: OTA runs inside loop() - keep the watchdog fed
        static unsigned int s_last_pct = 0;
        unsigned int pct = (total > 0) ? (progress * 100 / total) : 0;
        if (pct / 10 > s_last_pct / 10) {
            tlog("[OTA] %u%%\n", pct);
            s_last_pct = pct;
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        g_ota_in_progress.store(false);
        tlog("[OTA] error [%u]: %s\n", error,
             error == OTA_AUTH_ERROR    ? "Auth Failed"    :
             error == OTA_BEGIN_ERROR   ? "Begin Failed"   :
             error == OTA_CONNECT_ERROR ? "Connect Failed" :
             error == OTA_RECEIVE_ERROR ? "Receive Failed" :
             error == OTA_END_ERROR     ? "End Failed"     : "Unknown");
    });

    ArduinoOTA.begin();
    tlog("[SYS] OTA ready, hostname: %s\n", GATEWAY_HOSTNAME);
}

// ═════════════════════════════════════════════════════════════════════════════
// WEB PAGE BUILDERS
// ═════════════════════════════════════════════════════════════════════════════
String getHeader(String title) {
    String html;
    html.reserve(2048);
    html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    html += "<title>" + title + "</title>";
    html += F("<style>"
              "body{font-family:Arial,sans-serif;margin:0;padding:0;background:#f4f4f4;}"
              ".header-banner{background:#007BFF;color:#fff;padding:15px 20px;text-align:center;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
              ".header-banner h1{margin:0;font-size:24px;font-weight:bold;letter-spacing:0.5px;}"
              ".container{max-width:650px;background:#fff;padding:20px;margin:20px auto;border-radius:8px;box-shadow:0 0 10px rgba(0,0,0,0.1);}"
              "h2{color:#333;margin-top:5px;} h2.section{margin-top:30px;border-top:2px solid #007BFF;padding-top:15px;}"
              "nav a{margin-right:15px;text-decoration:none;color:#007BFF;font-weight:bold;display:inline-block;margin-bottom:4px;}"
              "table{width:100%;border-collapse:collapse;margin-top:15px;} td,th{padding:8px;border-bottom:1px solid #ddd;text-align:left;}"
              "input[type='text'],input[type='number'],input[type='password'],input[type='time'],select{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;}"
              "input[type='submit'],.btn{background:#28a745;color:#fff;border:none;padding:10px 15px;border-radius:4px;cursor:pointer;text-decoration:none;display:inline-block;}"
              ".btn-danger{background:#dc3545 !important;}"
              "small{color:#666;display:block;margin-top:-8px;margin-bottom:12px;}"
              ".bigvalue{text-align:center;margin:10px 0 20px;}"
              ".bigvalue .big{font-size:44px;font-weight:bold;color:#007BFF;line-height:1.1;}"
              ".bigvalue .sub{font-size:24px;color:#28a745;}"
              ".disconnected{color:#dc3545;font-weight:bold;}"
              ".card-danger{background:#fff3f3;border:1px solid #ffcdd2;padding:15px;border-radius:6px;margin-top:20px;}"
              "</style></head><body>");
    html += F("<div class='header-banner'><h1>Pool &amp; Tank Gateway</h1></div><div class='container'>");
    html += F("<nav><a href='/'>Status</a><a href='/tank'>Tank Setup</a><a href='/chlorinator'>Chlorinator Setup</a><a href='/timers'>Pump Timers</a>"
              "<a href='/mqtt'>MQTT Setup</a><a href='/wifi'>Wi-Fi Setup</a></nav><hr>");
    return html;
}

String getFooter() {
    return F("</div></body></html>");
}

void redirectHome() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

String getRssiDescription(int rssi) {
    if (rssi >= -60) return " (Strong)";
    if (rssi >= -75) return " (Medium)";
    if (rssi >= -85) return " (Weak)";
    return " (Very Weak)";
}

String tankFaultText() {
    return tankFaulted() ? ("FAULT - no valid reading for " + String(tankSecondsSinceGood()) + "s") : String("OK");
}

String bleFaultText() {
    return sensorFaulted() ? ("FAULT - no valid poll for " + String(secondsSinceGoodPoll()) + "s") : String("OK");
}

// Live-update JavaScript: polls /data.json and fills any element whose id
// matches. Works on every page - missing ids are simply skipped.
String getLiveScript() {
    return F("<script>"
      "function setText(id,t){var e=document.getElementById(id);if(e)e.textContent=t;}"
      "function setFault(id,f,age,what){var e=document.getElementById(id);if(!e)return;"
        "if(f){e.textContent='FAULT - no valid '+what+' for '+age+'s';e.className='disconnected';}"
        "else{e.textContent='OK';e.className='';}}"
      "async function refreshStatus(){"
        "try{"
          "const res=await fetch('/data.json');if(!res.ok)return;"
          "const d=await res.json();"
          "var t=d.tank;"
          "setText('volVal',t.volume+' L');setText('volValRow',t.volume+' L');"
          "setText('pctVal',t.percent.toFixed(1)+' % full');setText('pctValRow',t.percent.toFixed(1)+' %');"
          "setText('heightVal',t.height+' mm');setText('adcVal',t.adc);setText('adcDiffVal',t.adc_diff);"
          "setFault('tankFaultVal',t.fault,t.faultAge,'reading');"
          "var c=d.chlor;"
          "setText('modeVal',c.mode);"
          "setText('pumpVal',c.pump);setText('cellVal',c.cell);setText('primingVal',c.priming);"
          "setText('chlorineVal',c.chlorine);setText('timerVal',c.timer);setText('infoVal',c.info);setText('timeVal',c.time);"
          "setFault('bleFaultVal',c.fault,c.faultAge,'poll');"
          "setText('chlorSetVal',c.chlorSet);setText('cellHoursVal',c.cellHours);setText('lowSaltHoursVal',c.lowSaltHours);"
          "for(var i=1;i<=4;i++)setText('timer'+i+'Val',c['timer'+i]);"
          "var ls=document.getElementById('lowSaltVal');if(ls){ls.textContent=c.lowSalt;ls.className=(c.lowSalt=='LOW SALT')?'disconnected':'';}"
          "setText('rssiVal',d.rssi+' dBm'+d.rssiDesc);"
          "var m=document.getElementById('mqttVal');"
          "if(m){if(d.mqtt){m.textContent='Connected';m.className='';}else{m.textContent='Disconnected';m.className='disconnected';}}"
        "}catch(e){}"
      "}"
      "window.addEventListener('load',refreshStatus);"
      "setInterval(refreshStatus,3000);"
      "</script>");
}

// ═════════════════════════════════════════════════════════════════════════════
// WEB HANDLERS
// ═════════════════════════════════════════════════════════════════════════════
void handleRoot() {
    String html;
    html.reserve(6144);
    html += getHeader("Pool & Tank Gateway");

    // ── Water tank ───────────────────────────────────────────────────────────
    html += F("<h2>Water Tank</h2><div class='bigvalue'>");
    html += "<div class='big' id='volVal'>" + String(volumeLitres, 0) + " L</div>";
    html += "<div class='sub' id='pctVal'>" + String(percentFull, 1) + " % full</div></div><table>";
    html += "<tr><th>Water Height</th><td id='heightVal'>" + String(heightMm, 0) + " mm</td></tr>";
    html += "<tr><th>Current ADC (Single)</th><td id='adcVal'>" + String(currentADC) + "</td></tr>";
    html += "<tr><th>Current ADC (Diff A0-A1)</th><td id='adcDiffVal'>" + String(currentDiffADC) + "</td></tr>";
    html += "<tr><th>Read Frequency</th><td>" + String(read_interval_sec) + " seconds</td></tr>";
    html += "<tr><th>Readings Averaged</th><td>" + String(numReadingsToAverage) + "</td></tr>";
    html += "<tr><th>Smoothing</th><td>" + String(tankSmoothTauSec() / 60.0f, 1) + " min (auto)</td></tr>";   // v1.10
    html += "<tr><th>Sensor Power Mode</th><td>" + String(keepSensorPowered ? (sensorAlwaysOn ? "Always on" : "Always on (fast interval)") : "On 3s before each reading") + "</td></tr>";
    html += "<tr><th>Sensor Health</th><td id='tankFaultVal' class='" + String(tankFaulted() ? "disconnected" : "") + "'>" + tankFaultText() + "</td></tr>";
    html += F("</table>");

    // ── Chlorinator ──────────────────────────────────────────────────────────
    html += F("<h2 class='section'>Pool Chlorinator</h2><div class='bigvalue'>");
    if (g_has_last_state) {
        html += "<div class='big' id='modeVal'>" + String(mode_name(g_last_state.mode)) + "</div>";
    } else {
        html += F("<div class='big' id='modeVal'>--</div>");
    }
    html += F("</div><table>");
    String pump = "--", cell = "--", priming = "--", chlorine = "--", timer = "--", info = "--", tm = "--";
    if (g_has_last_state) {
        pump     = (g_speed_pending >= 0)                                // v1.16
            ? String("Setting to ") + speed_name((SpeedLevel)g_speed_pending) + "..."
            : g_last_state.pump_operating ? String("Running (") + speed_name(g_last_state.pump_speed) + ")" : String("Idle");   // v1.14
        cell     = g_last_state.cell_operating ? "Operating" : "Off";
        priming  = g_last_state.pump_priming ? "Yes" : "No";
        chlorine = chlorine_status_name(g_last_state.chlorine_status);
        timer    = String(g_last_state.active_timer);
        info     = info_message_name(g_last_state.info_message);
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u",
                 g_last_state.time_hours, g_last_state.time_minutes, g_last_state.time_seconds);
        tm = timeBuf;
    }
    html += "<tr><th>Pump</th><td id='pumpVal'>" + pump + "</td></tr>";
    html += "<tr><th>Cell</th><td id='cellVal'>" + cell + "</td></tr>";
    html += "<tr><th>Priming</th><td id='primingVal'>" + priming + "</td></tr>";
    html += "<tr><th>Chlorine Status</th><td id='chlorineVal'>" + chlorine + "</td></tr>";
    html += "<tr><th>Active Timer</th><td id='timerVal'>" + timer + "</td></tr>";
    html += "<tr><th>Info Message</th><td id='infoVal'>" + info + "</td></tr>";
    html += "<tr><th>Device Time</th><td id='timeVal'>" + tm + "</td></tr>";
    html += "<tr><th>Connection Interval</th><td>" + String(poll_interval_sec) + " seconds</td></tr>";
    html += "<tr><th>Gateway BLE</th><td>" + blePauseText() + "</td></tr>";   // v1.7
    {
        bool lowSalt = g_has_last_state && g_last_state.info_message == INFO_LOW_SALT;
        html += "<tr><th>Low Salt</th><td id='lowSaltVal' class='" + String(lowSalt ? "disconnected" : "") + "'>" +
                String(g_has_last_state ? (lowSalt ? "LOW SALT" : "OK") : "--") + "</td></tr>";
        String cellH = "--", lowH = "--", t[NUM_PUMP_TIMERS];
        for (int i = 0; i < NUM_PUMP_TIMERS; i++) t[i] = "--";
        if (g_has_extra && g_last_extra.stats_valid) {
            cellH = String(g_last_extra.cell_hours) + " h";
            lowH  = String(g_last_extra.low_salt_hours) + " h";
        }
        if (g_has_extra && g_last_extra.timers_valid)
            for (int i = 0; i < NUM_PUMP_TIMERS; i++) t[i] = timer_text(g_last_extra.timers[i]);
        html += "<tr><th>Cell Running Time</th><td id='cellHoursVal'>" + cellH + "</td></tr>";
        html += "<tr><th>Low Salt Running Time</th><td id='lowSaltHoursVal'>" + lowH + "</td></tr>";
        {
            int mn, mx; chlorineRange(mn, mx);
            String sp = (g_has_extra && g_last_extra.setup_valid)
                ? String(g_last_extra.chlorine_setpoint) + " (range " + String(mn) + "-" + String(mx) + ")" : String("--");
            html += "<tr><th>Chlorine Output Setpoint</th><td id='chlorSetVal'>" + sp + "</td></tr>";
        }
        for (int i = 0; i < NUM_PUMP_TIMERS; i++)
            html += "<tr><th>Pump Timer " + String(i + 1) + "</th><td id='timer" + String(i + 1) + "Val'>" + t[i] + "</td></tr>";
    }
    html += "<tr><th>BLE Poll Health</th><td id='bleFaultVal' class='" + String(sensorFaulted() ? "disconnected" : "") + "'>" + bleFaultText() + "</td></tr>";
    html += F("</table>");

    // ── System ───────────────────────────────────────────────────────────────
    html += F("<h2 class='section'>System</h2><table>");
    html += "<tr><th>WiFi SSID</th><td>" + WiFi.SSID() + "</td></tr>";
    html += "<tr><th>WiFi IP</th><td>" + WiFi.localIP().toString() + "</td></tr>";
    int rssi = WiFi.RSSI();
    html += "<tr><th>WiFi RSSI Signal</th><td id='rssiVal'>" + String(rssi) + " dBm" + getRssiDescription(rssi) + "</td></tr>";
    bool mqttOk = s_mqtt.connected();
    html += "<tr><th>MQTT Connection</th><td id='mqttVal' class='" + String(mqttOk ? "" : "disconnected") + "'>" + String(mqttOk ? "Connected" : "Disconnected") + "</td></tr>";
    html += "<tr><th>Uptime</th><td>" + String(millis() / 3600000UL) + " h " + String((millis() / 60000UL) % 60) + " min</td></tr>";
    html += "<tr><th>Free Heap</th><td>" + String(ESP.getFreeHeap()) + " bytes (lowest " + String(ESP.getMinFreeHeap()) + ")</td></tr>";
    html += "<tr><th>Last Restart Reason</th><td>" + String(g_reset_reason) + "</td></tr>";
    html += "<tr><th>Wi-Fi Drops Since Boot</th><td>" + String(g_wifi_drops.load()) + "</td></tr>";
    html += F("</table>");

    html += getLiveScript();
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

// ── Tank setup ───────────────────────────────────────────────────────────────
void handleTank() {
    String html;
    html.reserve(4096);
    html += getHeader("Tank Setup");
    html += F("<h2>Tank Setup</h2><form action='/save-tank' method='POST'>");

    html += "Total Volume of water tank, in litres (Geometric capacity):<br>"
            "<input type='number' step='1' name='tankVolumeLitres' value='" + String(tankVolumeLitres) + "'><br>";
    html += "Usable height - from top of bottom outlet to bottom of overflow (mm):<br>"
            "<input type='number' step='1' name='usableHeightMm' value='" + String(usableHeightMm) + "'><br>";
    html += "Unusable height - below the bottom outlet or cutoff switch (mm):<br>"
            "<input type='number' step='1' name='unusableHeightMm' value='" + String(unusableHeightMm) + "'><br>";
    html += "How much does the ADC change per mm of water?<br>"
            "<input type='number' step='0.01' name='adcPerMm' value='" + String(adcPerMm) + "'><br>";
    html += "What ADC value do you get when the probe is out of the water?<br>"
            "<input type='number' name='adcOutOfWater' value='" + String(adcOutOfWater) + "'><br>";
    html += "<small>Current live ADC reading (Single): <span id='adcVal'>" + String(currentADC) +
            "</span> | (Diff A0-A1): <span id='adcDiffVal'>" + String(currentDiffADC) + "</span></small>";
    html += "Measurement Frequency (seconds):<br>"
            "<input type='number' name='interval' min='1' max='86400' value='" + String(read_interval_sec) + "'><br>";
    html += F("<small>1 to 86400 seconds (e.g. 1 = every second, 300 = 5m, 86400 = once a day)</small>");
    html += "Readings per Measurement (~0.25s apart):<br>"
            "<input type='number' name='numReadings' min='1' max='32' value='" + String(numReadingsToAverage) + "'><br>";
    html += F("<small>1 to 32 raw ADC samples; the middle half are averaged into one reading</small>");
    // v1.10: sensor power mode
    html += "Sensor Power Mode:<br><select name='sensorAlwaysOn'>"
            "<option value='1'" + String(sensorAlwaysOn ? " selected" : "") + ">Always on (steadiest readings)</option>"
            "<option value='0'" + String(sensorAlwaysOn ? "" : " selected") + ">On 3s before each reading (saves power)</option>"
            "</select><br>";
    html += "<small>Power saving needs an interval of at least " + String((tankCycleMs() + POWER_SAVE_MARGIN_MS + 999) / 1000) +
            " s with the current readings setting; shorter intervals keep the sensor on</small>";
    html += F("<input type='submit' value='Save Settings'></form>");

    html += F("<h2>Calculated</h2><table>");
    html += "<tr><th>Volume per mm</th><td>" + String(volumePerMm, 3) + " L/mm</td></tr>";
    html += "<tr><th>Usable Volume</th><td>" + String(usableVolumeLitres, 0) + " L</td></tr>";
    html += "<tr><th>Water Height</th><td id='heightVal'>" + String(heightMm, 0) + " mm</td></tr>";
    html += "<tr><th>Water Volume</th><td id='volValRow'>" + String(volumeLitres, 0) + " L</td></tr>";
    html += "<tr><th>Percent Full</th><td id='pctValRow'>" + String(percentFull, 1) + " %</td></tr>";
    html += F("</table>");

    html += getLiveScript();
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSaveTank() {
    if (server.hasArg("tankVolumeLitres")) tankVolumeLitres = server.arg("tankVolumeLitres").toInt();
    if (server.hasArg("usableHeightMm"))   usableHeightMm   = server.arg("usableHeightMm").toInt();
    if (server.hasArg("unusableHeightMm")) unusableHeightMm = server.arg("unusableHeightMm").toInt();
    if (server.hasArg("adcPerMm"))         adcPerMm         = server.arg("adcPerMm").toFloat();
    if (server.hasArg("adcOutOfWater"))    adcOutOfWater    = server.arg("adcOutOfWater").toInt();
    if (server.hasArg("interval")) {
        int val = server.arg("interval").toInt();
        if (val >= 1 && val <= 86400) read_interval_sec = val;
    }
    if (server.hasArg("numReadings")) {
        int val = server.arg("numReadings").toInt();
        if (val >= 1 && val <= TANK_MAX_SAMPLES) numReadingsToAverage = val;
    }
    if (server.hasArg("sensorAlwaysOn")) {                       // v1.10
        sensorAlwaysOn = (server.arg("sensorAlwaysOn") == "1");
    }
    recalcTankDerived();
    saveSettings();
    updatePowerMode();
    redirectHome();
}

// ── v1.7: Pump timers ────────────────────────────────────────────────────────
String blePauseText() {
    long left = ble_pause_left_ms();
    if (left <= 0) return "Active (polling every " + String(poll_interval_sec) + " s)";
    String why = g_ble_pause_reason.load() == 1 ? "paused by you" : "app seems connected - backing off";
    return "Idle, " + why + " (" + String((left + 59999) / 60000) + " min left)";
}

void handleTimers() {
    String html;
    html.reserve(6144);
    html += getHeader("Pump Timers");
    html += F("<h2>Pump Timers</h2>");
    if (server.hasArg("msg")) html += "<p><b>" + server.arg("msg") + "</b></p>";
    int savingIdx = -1;                                   // v1.18: any timer still saving?
    for (int i = 0; i < NUM_PUMP_TIMERS; i++) if (g_tpend[i]) { savingIdx = i; break; }
    if (savingIdx >= 0) {
        html += "<p><b>Saving timer " + String(savingIdx + 1) + " to the chlorinator...</b></p>";
        html += F("<script>setTimeout(function(){location.href='/timers';},3000);</script>");
    } else if (g_timer_done_idx >= 0) {
        html += "<p><b>Timer " + String(g_timer_done_idx + 1) +
                (g_timer_done_ok ? " saved and verified." : " could NOT be confirmed - showing the chlorinator's current values.") +
                "</b></p>";
        g_timer_done_idx = -1;   // show once
    }

    html += "<table><tr><th>Gateway BLE</th><td>" + blePauseText() + "</td></tr></table>";

    if (!(g_has_extra && g_last_extra.timers_valid)) {
        html += F("<p>Timers not read yet - wait for the next poll, then refresh.</p>");
        html += getFooter();
        server.send(200, "text/html; charset=utf-8", html);
        return;
    }
    const char *speeds[3] = {"Low", "Medium", "High"};
    for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
        const PumpTimer &t = g_last_extra.timers[i];
        char st[6], sp[6];
        snprintf(st, sizeof(st), "%02u:%02u", t.start_h, t.start_m);
        snprintf(sp, sizeof(sp), "%02u:%02u", t.stop_h, t.stop_m);
        html += "<h2 class='section'>Timer " + String(i + 1) +
                String(g_tpend[i] ? " <small style='display:inline'>(saving...)</small>" : "") + "</h2>";
        html += "<form action='/save-timer' method='POST'><input type='hidden' name='timer' value='" + String(i + 1) + "'>";
        if (i == 0)
            html += F("<p>Always on</p>");   // v1.7: timer 1 can't be disabled
        else
            html += "<label><input type='checkbox' name='enabled' value='1'" + String(t.enabled ? " checked" : "") +
                    " onchange=\"tToggle(this," + String(i) + "," + String(t.enabled ? 1 : 0) + ")\"> Enabled</label><br><br>";
        // v1.7: start/stop/speed only shown when the timer is enabled (timer 1 always).
        // Hidden fields are still submitted, so a disabled timer keeps its times.
        bool show = (i == 0) || t.enabled;
        html += "<div id='tf" + String(i) + "' style='display:" + String(show ? "block" : "none") + "'>";
        html += "Start:<br><input type='time' name='start' value='" + String(st) + "'>";
        html += "Stop:<br><input type='time' name='stop' value='" + String(sp) + "'>";
        html += F("Speed:<br><select name='speed'>");
        for (int k = 0; k < 3; k++)
            html += "<option value='" + String(k) + "'" + String(t.speed == k ? " selected" : "") + ">" + speeds[k] + "</option>";
        if (t.speed > 2) html += F("<option value='' selected>AI (leave unchanged)</option>");
        // v1.7: Save shown when enabled, or when the tick box differs from the
        // chlorinator (so un-ticking an enabled timer can still be saved)
        html += "</select></div><input type='submit' id='ts" + String(i) + "' value='Save timer' style='display:" +
                String(show ? "block" : "none") + "'></form>";
    }
    html += F("<script>function tToggle(cb,i,was){"
              "document.getElementById('tf'+i).style.display=cb.checked?'block':'none';"
              "document.getElementById('ts'+i).style.display=(cb.checked||was)?'block':'none';}</script>");
    html += F("<small style='margin-top:15px'>Saving connects to the chlorinator straight away, writes the timer, "
              "reads it back to verify, then updates this page and Home Assistant.</small>");
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

static void redirectTimers(const char *msg) {
    String loc = "/timers?msg=";
    for (const char *p = msg; *p; p++) loc += (*p == ' ') ? '+' : *p;
    server.sendHeader("Location", loc, true);
    server.send(302, "text/plain", "");
}

void handleSaveTimer() {
    TimerSetCmd c{};
    int t = server.arg("timer").toInt();
    if (t < 1 || t > NUM_PUMP_TIMERS) { redirectTimers("Bad timer number"); return; }
    c.index = (uint8_t)(t - 1);
    c.has_enabled = true;
    c.enabled = server.hasArg("enabled");
    if (!parse_hhmm(server.arg("start").c_str(), c.start_h, c.start_m) ||
        !parse_hhmm(server.arg("stop").c_str(),  c.stop_h,  c.stop_m)) {
        redirectTimers("Bad time - use HH:MM"); return;
    }
    c.has_start = c.has_stop = true;
    String sp = server.arg("speed");
    if (sp.length() > 0) {
        int v = sp.toInt();
        if (v < SPEED_LOW || v > SPEED_HIGH) { redirectTimers("Bad speed"); return; }
        c.speed = (uint8_t)v; c.has_speed = true;
    }
    if (!queue_timer_write(c)) { redirectTimers("Bad timer number"); return; }
    tlog("[WEB] timer %d write queued\n", t);
    server.sendHeader("Location", "/timers", true);
    server.send(302, "text/plain", "");
}


// ── Chlorinator setup ────────────────────────────────────────────────────────
void handleChlorinator() {
    String html;
    html.reserve(3072);
    html += getHeader("Chlorinator Setup");
    html += F("<h2>Chlorinator Setup</h2><form action='/save-chlorinator' method='POST'>");

    html += "BLE MAC Address (leave blank to match by name only):<br>"
            "<input type='text' name='chlorinator_mac' value='" + String(chlorinator_mac) + "' placeholder='AA:BB:CC:DD:EE:FF'><br>";
    html += "Device Name (as advertised over BLE):<br>"
            "<input type='text' name='chlorinator_name' value='" + String(chlorinator_name) + "'><br>";
    html += "Access Code:<br>"
            "<input type='password' name='chlorinator_code' value='" + String(chlorinator_code) + "'><br>";
    html += "Connection Interval (seconds):<br>"
            "<input type='number' name='poll_interval_sec' min='60' max='3600' value='" + String(poll_interval_sec) + "'><br>";
    html += F("<small>How often the gateway connects to read the chlorinator (60-3600 s). 300 (5 min) or more "
              "leaves the phone app free to connect; commands are always sent straight away.</small>");
    html += "App Back-off (minutes):<br>"
            "<input type='number' name='app_backoff_min' min='1' max='120' value='" + String(app_backoff_min) + "'><br>";
    html += F("<small>How long the gateway stays off Bluetooth when the phone app seems to be connected</small>");
    html += "BLE Scan Duration (seconds):<br>"
            "<input type='number' name='ble_scan_duration_sec' min='1' max='60' value='" + String(ble_scan_duration_sec) + "'><br>";
    html += F("<small>How long to scan for the device when it needs (re)finding</small>");
    html += F("<input type='submit' value='Save Settings'></form>");

    html += F("<h2>Topics</h2><table>");
    html += "<tr><th>State Topic</th><td>" + String(s_topic_state) + "</td></tr>";
    html += "<tr><th>Action Topic</th><td>" + String(s_topic_action) + "</td></tr>";
    html += "<tr><th>Health Topic</th><td>" + String(s_topic_health) + "</td></tr>";
    html += "<tr><th>HA Device ID</th><td>" + String(s_device_id) + "</td></tr>";
    html += F("</table>");

    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSaveChlorinator() {
    char oldName[32];
    copyStr(oldName, chlorinator_name, sizeof(oldName));

    if (server.hasArg("chlorinator_mac"))
        copyStr(chlorinator_mac, server.arg("chlorinator_mac").c_str(), sizeof(chlorinator_mac));
    if (server.hasArg("chlorinator_name") && server.arg("chlorinator_name").length() > 0)
        copyStr(chlorinator_name, server.arg("chlorinator_name").c_str(), sizeof(chlorinator_name));
    if (server.hasArg("chlorinator_code"))
        copyStr(chlorinator_code, server.arg("chlorinator_code").c_str(), sizeof(chlorinator_code));
    if (server.hasArg("poll_interval_sec")) {
        int val = server.arg("poll_interval_sec").toInt();
        if (val >= 60 && val <= 3600) poll_interval_sec = val;   // v1.7: min 60 s
    }
    if (server.hasArg("app_backoff_min")) {        // v1.7
        int val = server.arg("app_backoff_min").toInt();
        if (val >= 1 && val <= 120) app_backoff_min = val;
    }
    if (server.hasArg("ble_scan_duration_sec")) {
        int val = server.arg("ble_scan_duration_sec").toInt();
        if (val >= 1 && val <= 60) ble_scan_duration_sec = val;
    }

    recalcTopics();
    g_has_ble_address = false;   // force a fresh scan in case MAC/name changed
    saveSettings();

    // Name changed -> new topics: reconnect so we subscribe to the new action
    // topic and re-send discovery under the new device id.
    if (strcmp(oldName, chlorinator_name) != 0 && s_mqtt.connected()) {
        s_mqtt.disconnect();
    }
    redirectHome();
}

// ── MQTT setup ───────────────────────────────────────────────────────────────
void handleMqtt() {
    String html;
    html.reserve(2560);
    html += getHeader("MQTT Setup");
    html += F("<h2>MQTT Broker Settings</h2><form action='/save-mqtt' method='POST'>");
    html += "Broker IP / Host: <input type='text' name='mqtt_server' value='" + String(mqtt_server) + "'><br>";
    html += "Broker Port: <input type='number' name='mqtt_port' value='" + String(mqtt_port) + "'><br>";
    html += "User: <input type='text' name='mqtt_user' value='" + String(mqtt_user) + "'><br>";
    html += "Password: <input type='password' name='mqtt_pass' value='" + String(mqtt_pass) + "'><br>";
    html += "Tank Topic Prefix: <input type='text' name='mqtt_topic' value='" + String(tank_topic) + "'><br>";
    html += F("<small>Chlorinator topics are built from its name (see Chlorinator Setup)</small>");
    html += F("<input type='submit' value='Save &amp; Reconnect'></form>");
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSaveMqtt() {
    if (server.hasArg("mqtt_server")) copyStr(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
    if (server.hasArg("mqtt_port"))   mqtt_port = server.arg("mqtt_port").toInt();
    if (server.hasArg("mqtt_user"))   copyStr(mqtt_user, server.arg("mqtt_user").c_str(), sizeof(mqtt_user));
    if (server.hasArg("mqtt_pass"))   copyStr(mqtt_pass, server.arg("mqtt_pass").c_str(), sizeof(mqtt_pass));
    if (server.hasArg("mqtt_topic") && server.arg("mqtt_topic").length() > 0)
        copyStr(tank_topic, server.arg("mqtt_topic").c_str(), sizeof(tank_topic));

    saveSettings();

    if (strlen(mqtt_server) > 0) {
        s_mqtt.setServer(mqtt_server, mqtt_port);
        if (s_mqtt.connected()) s_mqtt.disconnect();   // reconnects in loop()
    }
    redirectHome();
}

// ── Wi-Fi setup ──────────────────────────────────────────────────────────────
void handleWifi() {
    String html;
    html.reserve(3072);
    html += getHeader("Wi-Fi Setup");
    html += F("<h2>Connect to a Wi-Fi Network</h2>");
    html += "<p>Current Network: <b>" + (WiFi.SSID().length() > 0 ? WiFi.SSID() : String("Not Connected")) + "</b></p>";

    int n = WiFi.scanNetworks();

    html += F("<form action='/save-wifi' method='POST'><label for='ssid'>Select Wi-Fi Network (SSID):</label><br><select name='ssid' id='ssid'>");
    if (n <= 0) {
        html += F("<option value=''>No Wi-Fi networks found</option>");
    } else {
        for (int i = 0; i < n; ++i) {
            String ssidName = WiFi.SSID(i);
            String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured";
            html += "<option value='" + ssidName + "'";
            if (ssidName == WiFi.SSID()) html += " selected";
            html += ">" + ssidName + " (" + String(WiFi.RSSI(i)) + " dBm, " + enc + ")</option>";
        }
    }
    WiFi.scanDelete();
    html += F("</select><br>Or enter SSID manually:<br><input type='text' name='manual_ssid' placeholder='Manual SSID (optional)'><br>"
              "Wi-Fi Password:<br><input type='password' name='password' placeholder='Network Password'><br>"
              "<input type='submit' value='Join Network'></form>");

    html += F("<div class='card-danger'><h3 style='color:#dc3545;margin-top:0;'>Forget Wi-Fi Credentials</h3>"
              "<p>Clicking the button below will erase saved Wi-Fi details and reboot the device. "
              "It will restart in Access Point mode (<b>" GATEWAY_AP_NAME "</b>) to allow re-configuration via WiFiManager.</p>"
              "<form action='/reset-wifi' method='POST' onsubmit='return confirm(\"Are you sure you want to forget Wi-Fi credentials and restart in AP mode?\");'>"
              "<input type='submit' class='btn-danger' value='Forget Wi-Fi &amp; Restart AP'></form></div>");

    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSaveWifi() {
    String selectedSsid = server.arg("ssid");
    String manualSsid   = server.arg("manual_ssid");
    String password     = server.arg("password");
    String targetSsid   = (manualSsid.length() > 0) ? manualSsid : selectedSsid;

    if (targetSsid.length() == 0) {
        server.send(400, "text/plain", "No SSID selected or provided.");
        return;
    }

    String html = getHeader("Connecting Wi-Fi");
    html += "<h2>Connecting to " + targetSsid + "...</h2>";
    html += "<p>The device is applying new Wi-Fi credentials and rebooting. Please reconnect to <b>" + targetSsid +
            "</b> and check your router's device list or Home Assistant for the new IP address.</p>";
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
    delay(1000);

    quiesce_for_restart();           // quiesce BLE task before restart (v1.11: timed)
    WiFi.persistent(true);
    WiFi.begin(targetSsid.c_str(), password.c_str());
    delay(1000);
    ESP.restart();
}

void handleResetWifi() {
    String html = getHeader("Resetting Wi-Fi");
    html += F("<h2>Wi-Fi Reset Initiated</h2><p>Wi-Fi credentials have been erased. The ESP32 is restarting and will launch the setup hotspot (<b>" GATEWAY_AP_NAME "</b>).</p>");
    html += getFooter();
    server.send(200, "text/html; charset=utf-8", html);
    delay(1000);

    quiesce_for_restart();           // quiesce BLE task before restart (v1.11: timed)
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
    ESP.restart();
}

// ── Live data endpoint (polled by the pages every 3 s) ───────────────────────
void handleData() {
    int rssi = WiFi.RSSI();
    JsonDocument doc;

    doc["rssi"]     = rssi;
    doc["rssiDesc"] = getRssiDescription(rssi);
    doc["mqtt"]     = s_mqtt.connected();
    doc["ip"]       = WiFi.localIP().toString();

    JsonObject t = doc["tank"].to<JsonObject>();
    t["volume"]   = (long)round(volumeLitres);
    t["percent"]  = percentFull;
    t["height"]   = (long)round(heightMm);
    t["adc"]      = currentADC;
    t["adc_diff"] = currentDiffADC;
    t["fault"]    = tankFaulted();
    t["faultAge"] = tankSecondsSinceGood();

    JsonObject c = doc["chlor"].to<JsonObject>();
    if (g_has_last_state) {
        c["mode"]     = (g_mode_pending >= 0)                          // v1.12
            ? String("Setting to ") + mode_name((ChlorinatorMode)g_mode_pending) + "..."
            : String(mode_name(g_last_state.mode));
        c["pump"]     = (g_speed_pending >= 0)                         // v1.16
            ? String("Setting to ") + speed_name((SpeedLevel)g_speed_pending) + "..."
            : g_last_state.pump_operating ? String("Running (") + speed_name(g_last_state.pump_speed) + ")" : String("Idle");   // v1.14
        c["cell"]     = g_last_state.cell_operating ? "Operating" : "Off";
        c["priming"]  = g_last_state.pump_priming ? "Yes" : "No";
        c["chlorine"] = chlorine_status_name(g_last_state.chlorine_status);
        c["timer"]    = g_last_state.active_timer;
        c["info"]     = info_message_name(g_last_state.info_message);
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u",
                 g_last_state.time_hours, g_last_state.time_minutes, g_last_state.time_seconds);
        c["time"] = timeBuf;
    } else {
        c["mode"] = "--"; c["pump"] = "--"; c["cell"] = "--"; c["priming"] = "--";
        c["chlorine"] = "--"; c["timer"] = "--"; c["info"] = "--"; c["time"] = "--";
    }
    c["lowSalt"]  = g_has_last_state ? (g_last_state.info_message == INFO_LOW_SALT ? "LOW SALT" : "OK") : "--";
    if (g_has_extra && g_last_extra.stats_valid) {
        c["cellHours"]    = String(g_last_extra.cell_hours) + " h";
        c["lowSaltHours"] = String(g_last_extra.low_salt_hours) + " h";
    } else {
        c["cellHours"] = "--"; c["lowSaltHours"] = "--";
    }
    {
        int mn, mx; chlorineRange(mn, mx);
        c["chlorSet"] = (g_has_extra && g_last_extra.setup_valid)
            ? String(g_last_extra.chlorine_setpoint) + " (range " + String(mn) + "-" + String(mx) + ")" : String("--");
    }
    for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "timer%d", i + 1);
        c[key] = (g_has_extra && g_last_extra.timers_valid) ? timer_text(g_last_extra.timers[i]) : String("--");
    }
    c["fault"]    = sensorFaulted();
    c["faultAge"] = secondsSinceGoodPoll();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);            // LED off
    pinMode(SENSOR_POWER_PIN, OUTPUT);
    digitalWrite(SENSOR_POWER_PIN, LOW);    // tank sensor off

    // Sync primitives before the first tlog()
    g_log_mutex    = xSemaphoreCreateMutex();
    g_action_queue = xQueueCreate(1, sizeof(int));
    g_state_queue  = xQueueCreate(1, sizeof(ChlorinatorState));
    g_extra_queue  = xQueueCreate(1, sizeof(ChlorinatorExtra));

    delay(500);
    tlog("\n[SYS] Pool & Tank Gateway v1.18 (ESP32) starting\n");
    g_reset_reason = resetReasonName();
    tlog("[SYS] last restart reason: %s\n", g_reset_reason);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    adsReady = ads.begin();
    if (!adsReady) tlog("[TANK] WARNING: ADS1115 not found on I2C bus.\n");
    else ads.setDataRate(RATE_ADS1115_8SPS);   // v1.10: slow rate = less noise

    if (!LittleFS.begin(true)) {
        tlog("[SYS] LittleFS mount failed even after format attempt.\n");
    }
    loadSettings();
    recalcTopics();
    recalcTankDerived();

    tlog("[SYS] chlorinator state topic: %s\n", s_topic_state);
    tlog("[SYS] tank topic prefix      : %s\n", tank_topic);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                tlog("[WiFi] connected, IP: %s\n", WiFi.localIP().toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                tlog("[WiFi] disconnected, reason: %d\n", info.wifi_sta_disconnected.reason);
                g_wifi_drops.fetch_add(1);
                break;
            default:
                break;
        }
    });
    WiFi.setAutoReconnect(true);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect(GATEWAY_AP_NAME)) {
        tlog("[SYS] Failed to connect to WiFi. Restarting...\n");
        delay(200);
        ESP.restart();
    }
    tlog("[WiFi] connected! Device IP: %s\n", WiFi.localIP().toString().c_str());
    WiFi.setSleep(true);   // modem sleep is REQUIRED with WiFi + BLE together

    s_mqtt.setServer(mqtt_server, mqtt_port);
    s_mqtt.setCallback(on_mqtt_message);
    s_mqtt.setBufferSize(4096);
    s_mqtt.setKeepAlive(15);
    s_mqtt.setSocketTimeout(5);

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);   // max TX power for range
    tlog("[SYS] WiFi MAC : %s\n", WiFi.macAddress().c_str());
    tlog("[SYS] BLE  MAC : %s\n", NimBLEDevice::getAddress().toString().c_str());

    xTaskCreate(ble_task, "ble_task", 8192, nullptr, 1, &g_ble_task_handle);
    g_loop_beat_ms.store(millis() | 1);
    hw_watchdog_init();                                                      // v1.11 (before the tasks add themselves)
    xTaskCreate(supervisor_task, "supervisor", 3072, nullptr, 2, nullptr);   // v1.9

    setup_ota();
    updatePowerMode();    // starts the tank measurement cycle

    server.on("/",                 HTTP_GET,  handleRoot);
    server.on("/tank",             HTTP_GET,  handleTank);
    server.on("/save-tank",        HTTP_POST, handleSaveTank);
    server.on("/chlorinator",      HTTP_GET,  handleChlorinator);
    server.on("/save-chlorinator", HTTP_POST, handleSaveChlorinator);
    server.on("/mqtt",             HTTP_GET,  handleMqtt);
    server.on("/save-mqtt",        HTTP_POST, handleSaveMqtt);
    server.on("/wifi",             HTTP_GET,  handleWifi);
    server.on("/save-wifi",        HTTP_POST, handleSaveWifi);
    server.on("/reset-wifi",       HTTP_POST, handleResetWifi);
    server.on("/data.json",        HTTP_GET,  handleData);
    server.on("/timers",           HTTP_GET,  handleTimers);     // v1.7
    server.on("/save-timer",       HTTP_POST, handleSaveTimer);  // v1.7
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();

    tlog("[SYS] Web server started on port 80\n");
    esp_task_wdt_add(NULL);   // v1.11: loop() (this task) now on the hardware watchdog
    tlog("[SYS] setup complete\n");
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP - OTA, web, LED, tank sensor, chlorinator results, WiFi/MQTT, publishing
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
    g_loop_beat_ms.store(millis() | 1);   // v1.9: tell the supervisor we're alive
    esp_task_wdt_reset();                 // v1.11: and the hardware watchdog
    ArduinoOTA.handle();
    server.handleClient();

    // ── Heartbeat LED ────────────────────────────────────────────────────────
    static unsigned long s_led_toggle = 0;
    static bool s_led_state = false;
    if (millis() - s_led_toggle >= 1000) {
        s_led_toggle = millis();
        s_led_state = !s_led_state;
        digitalWrite(PIN_LED, s_led_state ? LOW : HIGH);
    }

    // ── Periodic heap / stack logging ────────────────────────────────────────
    static unsigned long s_heap_log = 0;
    if (millis() - s_heap_log >= 30000) {
        s_heap_log = millis();
        tlog("[SYS] free heap: %u  loop HWM: %u  BLE HWM: %u\n",
             esp_get_free_heap_size(),
             uxTaskGetStackHighWaterMark(NULL),
             uxTaskGetStackHighWaterMark(g_ble_task_handle));

        // v1.9: out of memory for 90 s -> restart before it crashes or hangs
        static int s_low_heap = 0;
        if (esp_get_free_heap_size() < LOW_HEAP_BYTES) s_low_heap++;
        else                                           s_low_heap = 0;
        if (s_low_heap >= LOW_HEAP_CHECKS && !g_ota_in_progress.load()) {
            tlog("[SYS] free heap low for too long - restarting\n");
            delay(200);
            ESP.restart();
        }
    }

    // ── Tank sensor (runs even when WiFi/MQTT are down) ──────────────────────
    tankService();

    // ── Chlorinator results from the BLE task -> dashboard (always) ──────────
    ChlorinatorState new_state;
    if (xQueueReceive(g_state_queue, &new_state, 0) == pdTRUE) {
        if (new_state.mode == MODE_BLE_ERROR) {
            g_last_state_ok = false;
        } else {
            g_last_state     = new_state;
            g_has_last_state = true;
            g_last_state_ok  = true;
            g_last_state_ms  = millis();
            if (g_mode_pending >= 0 && (int)new_state.mode == g_mode_pending) {
                g_mode_pending = -1;               // v1.12: confirmed by the chlorinator
            }
            if (g_speed_pending >= 0 && (int)new_state.pump_speed == g_speed_pending) {
                g_speed_pending = -1;              // v1.16: confirmed by the chlorinator
            }
        }
        g_chlor_pending_state   = new_state;
        g_chlor_publish_pending = true;
    }
    // v1.12: mode change never confirmed -> drop "Setting to ..." and show the real mode
    if (g_mode_pending >= 0 && millis() - g_mode_pending_ms > MODE_PENDING_TIMEOUT_MS) {
        tlog("[BLE] mode change not confirmed - clearing pending\n");
        g_mode_pending = -1;
        if (g_has_last_state) {
            g_chlor_pending_state   = g_last_state;
            g_chlor_publish_pending = true;
        }
    }
    // v1.16: speed change never confirmed -> drop "Setting to ..." and show the real speed
    if (g_speed_pending >= 0 && millis() - g_speed_pending_ms > MODE_PENDING_TIMEOUT_MS) {
        tlog("[BLE] pump speed change not confirmed - clearing pending\n");
        g_speed_pending = -1;
        if (g_has_last_state) {
            g_chlor_pending_state   = g_last_state;
            g_chlor_publish_pending = true;
        }
    }
    ChlorinatorExtra new_extra;
    if (xQueueReceive(g_extra_queue, &new_extra, 0) == pdTRUE) {
        if (new_extra.timers_valid || new_extra.stats_valid || new_extra.setup_valid) {
            // keep the last timers if this read didn't get them
            bool keepT = !new_extra.timers_valid && g_has_extra && g_last_extra.timers_valid;
            PumpTimer saved[NUM_PUMP_TIMERS];
            if (keepT) memcpy(saved, g_last_extra.timers, sizeof(saved));
            g_last_extra = new_extra;
            if (keepT) { memcpy(g_last_extra.timers, saved, sizeof(saved)); g_last_extra.timers_valid = true; }
            g_has_extra  = true;
        }
        if (new_extra.timers_valid)
            for (int i = 0; i < NUM_PUMP_TIMERS; i++) g_tactual[i] = new_extra.timers[i];
        // v1.18: a finished save (latest request for that timer) -> show the read-back
        for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
            if (g_tpend[i] && new_extra.timer_done_seq[i] != 0 && new_extra.timer_done_seq[i] == g_tseq[i]) {
                g_tpend[i]       = false;
                g_timer_done_idx = i;
                g_timer_done_ok  = !new_extra.timer_batch_failed;
            }
        }
        // still saving -> keep showing what was asked for
        if (g_has_extra && g_last_extra.timers_valid)
            for (int i = 0; i < NUM_PUMP_TIMERS; i++)
                g_last_extra.timers[i] = g_tpend[i] ? g_tdesired[i] : g_tactual[i];
        g_extra_publish_pending = true;
    }
    // v1.18: a save that never finished -> show the chlorinator's values
    for (int i = 0; i < NUM_PUMP_TIMERS; i++) {
        if (g_tpend[i] && millis() - g_tpend_ms[i] > TIMER_SAVE_TIMEOUT_MS) {
            tlog("[BLE] timer %d save not confirmed in time\n", i + 1);
            g_tpend[i] = false;
            if (g_has_extra && g_last_extra.timers_valid) g_last_extra.timers[i] = g_tactual[i];
            g_timer_done_idx = i; g_timer_done_ok = false;
            g_extra_publish_pending = true;
        }
    }
    // v1.4: once the chlorinator's real setpoint range is known, update the
    // HA slider limits if they differ from what was last announced
    {
        static int s_pub_min = -1, s_pub_max = -1;
        if (s_mqtt.connected() && g_has_extra && g_last_extra.caps_valid) {
            int mn, mx;
            chlorineRange(mn, mx);
            if (mn != s_pub_min || mx != s_pub_max) {
                s_pub_min = mn; s_pub_max = mx;
                publish_setpoint_discovery();
            }
        }
    }

    // ── Maintain WiFi ────────────────────────────────────────────────────────
    // v1.9: don't just wait - force a reconnect every 30 s, and restart the
    // ESP32 if Wi-Fi has been down for 10 minutes.
    static unsigned long s_wifi_down_since = 0;   // 0 = Wi-Fi is up
    static unsigned long s_wifi_retry      = 0;
    if (WiFi.status() != WL_CONNECTED) {
        g_mqtt_connected.store(false);
        if (s_mqtt.connected()) s_mqtt.disconnect();
        if (s_wifi_down_since == 0) {
            s_wifi_down_since = millis() | 1;
            s_wifi_retry      = millis();
            tlog("[WiFi] connection lost\n");
        }
        if (!g_ota_in_progress.load()) {
            if (millis() - s_wifi_down_since >= WIFI_RESTART_MS) {
                tlog("[WiFi] offline for 10 minutes - restarting\n");
                delay(200);
                ESP.restart();
            }
            if (millis() - s_wifi_retry >= WIFI_RETRY_MS) {
                s_wifi_retry = millis();
                tlog("[WiFi] still offline - forcing reconnect\n");
                WiFi.reconnect();
            }
        }
        return;
    }
    if (s_wifi_down_since != 0) {
        tlog("[WiFi] back online after %lu s\n", (millis() - s_wifi_down_since) / 1000UL);
        s_wifi_down_since = 0;
    }

    // ── Maintain MQTT ────────────────────────────────────────────────────────
    // v1.11: Wi-Fi says "connected" but MQTT won't come back -> the link is
    // probably dead (stale association after a router/mesh reboot). Rejoin
    // Wi-Fi every 5 minutes; restart after 20 minutes. Only once MQTT has
    // been set up.
    static unsigned long s_mqtt_down_since = 0;   // 0 = MQTT is up
    static unsigned long s_mqtt_rejoin     = 0;
    if (!s_mqtt.connected()) {
        g_mqtt_connected.store(false);
        if (strlen(mqtt_server) > 0 && !g_ota_in_progress.load()) {
            if (s_mqtt_down_since == 0) {
                s_mqtt_down_since = millis() | 1;
                s_mqtt_rejoin     = millis();
            }
            if (millis() - s_mqtt_down_since >= MQTT_RESTART_MS) {
                tlog("[MQTT] down for 20 minutes with Wi-Fi up - restarting\n");
                delay(200);
                ESP.restart();
            }
            if (millis() - s_mqtt_rejoin >= MQTT_REJOIN_MS) {
                s_mqtt_rejoin = millis();
                tlog("[MQTT] still down with Wi-Fi up - rejoining Wi-Fi\n");
                WiFi.reconnect();
                return;   // the Wi-Fi block above takes over until it is back
            }
        }
        mqtt_reconnect();
        return;
    }
    if (s_mqtt_down_since != 0) {
        tlog("[MQTT] back after %lu s\n", (millis() - s_mqtt_down_since) / 1000UL);
        s_mqtt_down_since = 0;
    }
    s_mqtt.loop();
    g_mqtt_connected.store(true);

    // v1.11: diagnostics to HA every minute (also sent after each BLE poll),
    // so HA keeps a history even while the BLE task is backing off
    static unsigned long s_diag_ms = 0;
    if (millis() - s_diag_ms >= DIAG_PUBLISH_MS) {
        s_diag_ms = millis();
        publish_ble_health();
        s_mqtt.publish(tankAvailTopic().c_str(), "online", true);   // v1.15: undo any stale Last Will
    }

    // ── Publish: chlorinator (device 1) ──────────────────────────────────────
    if (g_chlor_publish_pending) {
        g_chlor_publish_pending = false;
        if (g_chlor_pending_state.mode == MODE_BLE_ERROR) {
            publish_error("BLE operation failed");
        } else {
            publish_state(g_chlor_pending_state);
        }
        publish_ble_health();
    }
    if (g_extra_publish_pending) {
        g_extra_publish_pending = false;
        publish_extra(g_last_extra);
    }

    // ── Publish: water tank (device 2) ───────────────────────────────────────
    if (tankPublishPending) {
        tankPublishPending = false;
        if (tankLastValid) tank_publish_data();
        tank_publish_health();
    }
}
