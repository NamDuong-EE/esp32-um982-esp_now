#ifndef RTCM_ESPNOW_HANDLER_H
#define RTCM_ESPNOW_HANDLER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

void resetRtcmEspNowReassembly();
bool processNextRtcmEspNowPacket(TickType_t waitTicks);
uint16_t getLastCompletedRtcmStreamId();

#endif
