#ifndef PROG_CONFIG_H
#define PROG_CONFIG_H

#include <cstddef>
#include <cstdint>

#include "Top_Lvl_Config.h"

#if __has_include("EspNow_Secrets.h")
#include "EspNow_Secrets.h"
#endif

#ifndef ESPNOW_SECURITY_ENABLED
#define ESPNOW_SECURITY_ENABLED 0
#endif
#ifndef ESPNOW_PMK_BYTES
#define ESPNOW_PMK_BYTES \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif
#ifndef ESPNOW_LMK_BYTES
#define ESPNOW_LMK_BYTES \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

// ================= UART UM980/UM982 =================
// ESP32-WROOM-32U/ESP32U defaults. Change these two pins to match the PCB.
inline constexpr int RX_GNSS = 16; // UM980 TX -> ESP32 RX
inline constexpr int TX_GNSS = 17; // UM980 RX -> ESP32 TX
inline constexpr uint32_t GNSS_BAUD = 115200;
inline constexpr std::size_t GNSS_TX_BUFFER_SIZE = 2048;
inline constexpr int LED_PIN = 2;

// ================= TASKS =================
inline constexpr uint32_t MUTEX_TIMEOUT_MS = 1500;
inline constexpr uint32_t HEALTH_INTERVAL_MS = 30000;
#ifndef SERIAL_DEBUG_STATUS_ENABLED
#define SERIAL_DEBUG_STATUS_ENABLED 0
#endif
inline constexpr bool SERIAL_DEBUG_STATUS_OUTPUT_ENABLED =
    SERIAL_DEBUG_STATUS_ENABLED != 0;
inline constexpr uint32_t SERIAL_DEBUG_STATUS_INTERVAL_MS = 1000;
inline constexpr uint32_t ROVER_LLH_STATUS_INTERVAL_MS = 1000;
inline constexpr uint32_t ROVER_LLH_MAX_GGA_AGE_MS = 3000;
inline constexpr std::size_t GNSS_COMMAND_QUEUE_LENGTH = 4;
inline constexpr uint32_t GNSS_COMMAND_UART_LOCK_TIMEOUT_MS = 2000;
inline constexpr uint32_t GNSS_COMMAND_UNLOG_DELAY_MS = 1000;
inline constexpr uint32_t GNSS_COMMAND_MODE_DELAY_MS = 2000;
inline constexpr uint32_t GNSS_COMMAND_OUTPUT_DELAY_MS = 200;
inline constexpr uint32_t GNSS_COMMAND_RESPONSE_TIMEOUT_MS = 2500;
inline constexpr uint32_t GNSS_COMMAND_SAVECONFIG_SETTLE_MS = 1000;
inline constexpr uint32_t GNSS_COMMAND_RESET_BOOT_TIMEOUT_MS = 15000;
inline constexpr uint32_t GNSS_COMMAND_MODE_QUERY_INTERVAL_MS = 1000;
inline constexpr uint32_t GNSS_COMMAND_GGA_VERIFY_TIMEOUT_MS = 10000;

// ================= OPERATING MODE =================
// Override from PlatformIO with -D ROVER_RELAY_MODE_ENABLED=1 to build the
// intermediate Rover. Normal mode remains the safe default.
#ifndef ROVER_RELAY_MODE_ENABLED
#define ROVER_RELAY_MODE_ENABLED 0
#endif
inline constexpr bool ROVER_RELAY_MODE = ROVER_RELAY_MODE_ENABLED != 0;

// ================= WI-FI / MQTT =================
// ESP-NOW still uses the ESP32 Wi-Fi radio in STA mode, but field mode does
// not connect to a router/AP and does not use MQTT by default.
inline constexpr bool WIFI_CONNECT_TO_ROUTER_ENABLED = false;
inline constexpr bool ROVER_MQTT_ENABLED = false;
inline constexpr uint8_t ESPNOW_WIFI_CHANNEL = 6;

inline constexpr char WIFI_SSID[] = "AITOGY";
inline constexpr char WIFI_PASSWORD[] = "aitogy@aitogy";
inline constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
inline constexpr uint32_t WIFI_RETRY_DELAY_MS = 5000;

// Field mode: keep health/status on Serial only. Set true when remote MQTT
// diagnostics are needed again.
inline constexpr bool MQTT_PUBLISH_HEALTH_ENABLED = false;

inline constexpr char MQTT_SERVER[] = "aitogy.asia";
inline constexpr uint16_t MQTT_PORT = 1883;
inline constexpr char MQTT_USER[] = "mqttUser";
inline constexpr char MQTT_PASS[] = "MqttPassword123$%^";

inline constexpr char TOPIC_PUB_DATA_GGA[] = "tdm2402/um980/data/gga";
inline constexpr char TOPIC_PUB_DATA_KSXT[] = "tdm2402/um980/data/ksxt";
inline constexpr char TOPIC_SUB_CMD[] = "tdm2402/um980/cmd";
inline constexpr char TOPIC_PUB_RAW_GGA[] = "tdm2402/um980/raw/gga";
inline constexpr char TOPIC_PUB_RAW_KSXT[] = "tdm2402/um980/raw/ksxt";
inline constexpr char TOPIC_PUB_HEALTH[] = "tdm2402/um980/health";

// ================= DEBUG   =================
// Disabled by default for field operation. Set to 1 to compile and host a
// simple SoftAP debug page at http://192.168.4.1 on the ESP-NOW channel.
#ifndef DEBUG_WEB_ENABLED
#define DEBUG_WEB_ENABLED 0
#endif
inline constexpr char DEBUG_WEB_AP_SSID[] = "ESP32-Rover-Debug";
inline constexpr char DEBUG_WEB_RELAY_AP_SSID[] = "ESP32-Rover-Relay";
inline constexpr char DEBUG_WEB_AP_PASSWORD[] = "123456789";
inline constexpr uint8_t DEBUG_WEB_AP_MAX_CLIENTS = 2;

// ================= ESP-NOW =================25
// Base MAC is learned via pairing and stored in NVS/Preferences.
inline constexpr std::size_t ESPNOW_QUEUE_LENGTH = 16;
inline constexpr uint32_t RTCM_REASSEMBLY_TIMEOUT_MS = 1500;
// ESP-NOW TX rate selection:
// - ESPNOW_FORCE_LR_RATE=false: keep the ESP-IDF default TX rate.
// - ESPNOW_FORCE_LR_RATE=true: force 250 or 500 Kbps LR below.
// WIFI_PROTOCOL_LR remains enabled in both cases so LR frames can be received.
inline constexpr bool ESPNOW_FORCE_LR_RATE = true;
inline constexpr bool ESPNOW_USE_LR_250KBPS = true; // true=250 Kbps, false=500 Kbps
inline constexpr uint32_t ESPNOW_TX_MUTEX_TIMEOUT_MS = 500;
inline constexpr uint32_t ESPNOW_TX_CALLBACK_TIMEOUT_MS = 300;

// Temporary Base sends validated RTCM only to its paired original Base.
inline constexpr std::size_t TEMP_BASE_UPLINK_QUEUE_LENGTH = 3;
inline constexpr uint32_t TEMP_BASE_UPLINK_ACK_TIMEOUT_MS = 300;
inline constexpr uint32_t TEMP_BASE_UPLINK_FRAME_DEADLINE_MS = 1000;
inline constexpr uint8_t TEMP_BASE_UPLINK_FRAME_RETRY_COUNT = 1;
inline constexpr uint8_t TEMP_BASE_UPLINK_FRAGMENT_RETRY_COUNT = 2;
inline constexpr uint32_t TEMP_BASE_UPLINK_FRAGMENT_GAP_MS = 5;

// ================= ESP-NOW PAIRING =================
inline constexpr bool ESPNOW_PAIRING_ENABLED = true;
inline constexpr int PAIRING_BUTTON_PIN = 0; // BOOT on many ESP32 boards; change to PCB pairing button.
inline constexpr bool PAIRING_BUTTON_ACTIVE_LOW = true;
inline constexpr uint32_t PAIRING_BUTTON_HOLD_MS = 1500;
inline constexpr uint32_t PAIRING_WINDOW_MS = 60000;
inline constexpr uint32_t PAIR_CONFIRM_TIMEOUT_MS = 3000;
inline constexpr uint32_t ESPNOW_NETWORK_ID = 0xA1700001UL;
inline constexpr uint8_t ESPNOW_PAIRING_KEY[16] = {
    0x41, 0x49, 0x54, 0x4F, 0x47, 0x59, 0x5F, 0x50,
    0x41, 0x49, 0x52, 0x5F, 0x56, 0x30, 0x30, 0x31,
};
inline constexpr char ESPNOW_NVS_NAMESPACE[] = "espnow";
inline constexpr char ESPNOW_NVS_BASE_MAC_KEY[] = "base_mac";

// ================= RELAY / DOWNSTREAM CHILD =================
// Relay mode uses the same physical pairing button. If no Base is stored the
// button opens upstream pairing; after Base provisioning it opens child pairing.
inline constexpr char ESPNOW_NVS_CHILD_MAC_KEY[] = "child_mac";
inline constexpr char ESPNOW_NVS_CHILD_COUNT_KEY[] = "child_count";
inline constexpr char ESPNOW_NVS_CHILD_MAC_PREFIX[] = "child";
inline constexpr std::size_t RELAY_MAX_CHILDREN = 5;
inline constexpr std::size_t RELAY_QUEUE_LENGTH = 3;
inline constexpr uint32_t RELAY_ACK_TIMEOUT_MS = 300;
inline constexpr uint8_t RELAY_FRAME_RETRY_COUNT = 2;
inline constexpr uint8_t RELAY_FRAGMENT_SEND_RETRY_COUNT = 2;
inline constexpr uint32_t RELAY_FRAGMENT_GAP_MS = 5;
inline constexpr uint32_t RELAY_FAILED_FRAME_BACKOFF_MS = 1000;
inline constexpr uint32_t RELAY_DISCOVERY_INTERVAL_MS = 500;
inline constexpr uint32_t RELAY_CHILD_ACK_STATUS_INTERVAL_MS = 5000;
inline constexpr uint32_t RELAY_CHILD_CLEAR_HOLD_MS = 20000;
inline constexpr uint8_t RELAY_CHILD_FAILURES_BEFORE_COOLDOWN = 2;
inline constexpr uint32_t RELAY_CHILD_FAILURE_COOLDOWN_MS = 3000;

// The real PMK/LMK come from the ignored EspNow_Secrets.h provisioned with Base.
inline constexpr bool ESPNOW_ENCRYPTION_ENABLED = ESPNOW_SECURITY_ENABLED != 0;
inline constexpr uint8_t ESPNOW_PMK[16] = {ESPNOW_PMK_BYTES};
inline constexpr uint8_t ESPNOW_LMK[16] = {ESPNOW_LMK_BYTES};

inline constexpr bool espnowSecurityKeyIsProvisioned(const uint8_t (&key)[16]) {
    bool anyNonZero = false;
    bool anyNotFf = false;
    bool anyDifferent = false;
    for (std::size_t index = 0; index < sizeof(key); ++index) {
        anyNonZero = anyNonZero || key[index] != 0x00;
        anyNotFf = anyNotFf || key[index] != 0xFF;
        anyDifferent = anyDifferent || key[index] != key[0];
    }
    return anyNonZero && anyNotFf && anyDifferent;
}

inline constexpr bool espnowSecurityKeysAreConfigured() {
    bool keysAreDistinct = false;
    for (std::size_t index = 0; index < sizeof(ESPNOW_PMK); ++index) {
        keysAreDistinct = keysAreDistinct || ESPNOW_PMK[index] != ESPNOW_LMK[index];
    }
    return espnowSecurityKeyIsProvisioned(ESPNOW_PMK) &&
           espnowSecurityKeyIsProvisioned(ESPNOW_LMK) &&
           keysAreDistinct;
}

static_assert(!ESPNOW_ENCRYPTION_ENABLED || espnowSecurityKeysAreConfigured(),
              "ESP-NOW encryption requires distinct provisioned 16-byte PMK/LMK values");

#endif
