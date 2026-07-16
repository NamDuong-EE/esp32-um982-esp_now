#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

// ============= GẮN THƯ VIỆN CẦN THIẾT =============
#include <WiFi.h>
#include "Top_Lvl_Config.h"

// ================= HÀM CẤU HÌNH WI-FI =================

bool setupWiFi();
bool setupEspNowStaRadio();
bool configureWiFiForEspNowLongRange();
bool wifiRadioIsReady();
uint8_t getWiFiChannel();

#endif
