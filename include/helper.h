#ifndef HELPER_H
#define HELPER_H

#include <Arduino.h>
#include <freertos/semphr.h>

#include "Top_Lvl_Config.h"
#include "Prog_Config.h"
#include "DataStructs.h"
#include "functions/MQTT_Manager.h"
#include "functions/NMEA_Parser.h"
#include "functions/Rtcm_EspNow_Handler.h"
#include "hardware/Espnow_handler.h"
#include "hardware/Wifi_handler.h"

extern String latestGGA;
extern String targetGGA;
extern SemaphoreHandle_t mqttClientMutex;
extern SemaphoreHandle_t nmeaBufferMutex;
extern SemaphoreHandle_t gnssTxMutex;

struct GgaDebugSnapshot {
    bool valid;
    double lat;
    double lon;
    uint8_t fixQuality;
    uint8_t satellites;
    uint32_t lastUpdateMs;
};

String formDeviceHealthString();
int publishGGA(String& nmeaBuffer);
GgaDebugSnapshot getGgaDebugSnapshot();

#endif
