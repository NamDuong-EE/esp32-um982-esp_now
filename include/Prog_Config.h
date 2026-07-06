#ifndef PROG_CONFIG_H
#define PROG_CONFIG_H

#include <cstddef>
#include <cstdint>

#include "Top_Lvl_Config.h"

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

// ================= ESP-NOW =================
// Provision the STA MAC of the Base before deployment. An all-zero MAC is rejected.
inline constexpr uint8_t ESPNOW_BASE_MAC[6] = {0x68, 0x09, 0x47, 0xf8, 0x48, 0x90};
inline constexpr std::size_t ESPNOW_QUEUE_LENGTH = 16;
inline constexpr uint32_t RTCM_REASSEMBLY_TIMEOUT_MS = 1500;
inline constexpr bool ESPNOW_USE_LR_250KBPS = true;

// Enable only after replacing both keys on Base and Rover with the same provisioned values.
inline constexpr bool ESPNOW_ENCRYPTION_ENABLED = false;
inline constexpr uint8_t ESPNOW_PMK[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
inline constexpr uint8_t ESPNOW_LMK[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

inline constexpr bool espnowBaseMacIsConfigured() {
    return ESPNOW_BASE_MAC[0] != 0 || ESPNOW_BASE_MAC[1] != 0 ||
           ESPNOW_BASE_MAC[2] != 0 || ESPNOW_BASE_MAC[3] != 0 ||
           ESPNOW_BASE_MAC[4] != 0 || ESPNOW_BASE_MAC[5] != 0;
}

inline constexpr bool espnowSecurityKeysAreConfigured() {
    bool pmkConfigured = false;
    bool lmkConfigured = false;
    for (std::size_t index = 0; index < sizeof(ESPNOW_PMK); ++index) {
        pmkConfigured = pmkConfigured || ESPNOW_PMK[index] != 0;
        lmkConfigured = lmkConfigured || ESPNOW_LMK[index] != 0;
    }
    return pmkConfigured && lmkConfigured;
}

#endif
