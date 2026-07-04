#include "Top_Lvl_Config.h"

#if CONNECT_USING_WIFI
#define WIFI_CODE

#include "Prog_Config.h"
#include "hardware/Wifi_handler.h"
#include <esp_wifi.h>

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
  return static_cast<uint8_t>(WiFi.channel());
}

bool setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.print("\n[WIFI] Dang ket noi mang: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempt > 10) {
      Serial.println("\n[ERROR] Ket noi WiFi that bai!");
      return false;
    }
  }
  Serial.println("\n[WIFI] Ket noi THANH CONG! IP: " + WiFi.localIP().toString());
  Serial.println("[WIFI] Rover STA MAC: " + WiFi.macAddress());
  Serial.printf("[WIFI] Channel: %u\n", getWiFiChannel());
  return configureWiFiForEspNowLongRange();
}

#endif // WIFI_CODE
