#include "Top_Lvl_Config.h"

#if CONNECT_USING_WIFI
#define WIFI_CODE

#include "Prog_Config.h"
#include "hardware/Wifi_handler.h"
#include <esp_wifi.h>

namespace {

const char* wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

const char* encryptionToText(wifi_auth_mode_t encryption) {
  switch (encryption) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    default:
      return "UNKNOWN";
  }
}

void printVisibleTargetNetwork() {
  Serial.println("[WIFI] Dang scan SSID de chan doan...");
  const int networkCount = WiFi.scanNetworks(false, true);
  bool found = false;

  if (networkCount <= 0) {
    Serial.println("[WIFI][ERROR] ESP32 khong scan thay mang Wi-Fi nao. Kiem tra anten ESP32U, nguon va khoang cach router.");
  } else {
    Serial.printf("[WIFI] So mang 2.4 GHz scan duoc: %d\n", networkCount);
    const int maxNetworksToPrint = networkCount > 12 ? 12 : networkCount;
    for (int index = 0; index < maxNetworksToPrint; ++index) {
      const String ssid = WiFi.SSID(index).isEmpty() ? String("<hidden>") : WiFi.SSID(index);
      Serial.printf("[WIFI] #%02d SSID=%s RSSI=%d dBm channel=%d encryption=%s\n",
                    index + 1,
                    ssid.c_str(),
                    WiFi.RSSI(index),
                    WiFi.channel(index),
                    encryptionToText(static_cast<wifi_auth_mode_t>(WiFi.encryptionType(index))));
    }
  }

  for (int index = 0; index < networkCount; ++index) {
    if (WiFi.SSID(index) == WIFI_SSID) {
      found = true;
      Serial.printf("[WIFI] Tim thay SSID %s: RSSI=%d dBm, channel=%d, encryption=%s\n",
                    WIFI_SSID,
                    WiFi.RSSI(index),
                    WiFi.channel(index),
                    encryptionToText(static_cast<wifi_auth_mode_t>(WiFi.encryptionType(index))));
    }
  }

  if (!found) {
    Serial.printf("[WIFI][ERROR] Khong thay SSID %s. ESP32 chi ket noi duoc Wi-Fi 2.4 GHz; nen dat router channel co dinh 1, 6 hoac 11.\n",
                  WIFI_SSID);
  }

  WiFi.scanDelete();
}

bool configureMaxTxPower() {
  if (!WiFi.setTxPower(WIFI_POWER_19_5dBm)) {
    Serial.println("[WIFI][ERROR] Khong set duoc TX power 19.5 dBm");
    return false;
  }

  int8_t actualPower = 0;
  const esp_err_t result = esp_wifi_get_max_tx_power(&actualPower);
  if (result == ESP_OK) {
    Serial.printf("[WIFI] TX power fixed raw=%d dBm=%.2f\n",
                  actualPower,
                  actualPower / 4.0f);
  } else {
    Serial.printf("[WIFI][WARN] Khong doc duoc TX power: %d\n", result);
  }
  return true;
}

} // namespace

bool configureWiFiForEspNowLongRange() {
  const uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                            WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR;
  esp_err_t result = esp_wifi_set_protocol(WIFI_IF_STA, protocols);
  if (result != ESP_OK) {
    Serial.printf("[WIFI][ERROR] Khong bat duoc B/G/N/LR: %d\n", result);
    return false;
  }
  return true;
}

uint8_t getWiFiChannel() {
  uint8_t primaryChannel = 0;
  wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primaryChannel, &secondChannel) == ESP_OK && primaryChannel != 0) {
    return primaryChannel;
  }
  return ESPNOW_WIFI_CHANNEL;
}

bool setupEspNowStaRadio() {
  WiFi.mode(DEBUG_WEB_ENABLED ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(100);
  if (!configureMaxTxPower()) {
    return false;
  }

  if (!configureWiFiForEspNowLongRange()) {
    return false;
  }

  esp_err_t result = esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (result != ESP_OK) {
    Serial.printf("[WIFI][ERROR] Khong dat duoc ESP-NOW channel %u: %d\n",
                  ESPNOW_WIFI_CHANNEL,
                  result);
    return false;
  }

  Serial.println("[WIFI] Khong ket noi router/AP; chi dung STA radio cho ESP-NOW");
  if constexpr (DEBUG_WEB_ENABLED) {
    Serial.println("[WIFI] Debug web bat; Wi-Fi mode AP+STA");
  }
  Serial.println("[WIFI] Local STA MAC: " + WiFi.macAddress());
  Serial.printf("[WIFI] ESP-NOW fixed channel: %u\n", getWiFiChannel());
  return true;
}

bool setupWiFi() {
  if (!WIFI_CONNECT_TO_ROUTER_ENABLED) {
    return setupEspNowStaRadio();
  }

  WiFi.mode(DEBUG_WEB_ENABLED ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, false);
  delay(100);
  if (!configureMaxTxPower()) {
    return false;
  }
  Serial.print("\n[WIFI] Dang ket noi mang: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  const wl_status_t finalStatus = WiFi.status();
  if (finalStatus != WL_CONNECTED) {
    Serial.printf("\n[ERROR] Ket noi WiFi that bai! status=%s(%d), timeout=%lu ms\n",
                  wifiStatusToText(finalStatus),
                  static_cast<int>(finalStatus),
                  static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT_MS));
    printVisibleTargetNetwork();
    return false;
  }

  Serial.println("\n[WIFI] Ket noi THANH CONG! IP: " + WiFi.localIP().toString());
  Serial.println("[WIFI] Rover STA MAC: " + WiFi.macAddress());
  Serial.printf("[WIFI] Channel: %u\n", getWiFiChannel());
  return configureWiFiForEspNowLongRange();
}

#endif // WIFI_CODE
