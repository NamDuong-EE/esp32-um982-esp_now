#ifndef ESPNOW_HANDLER_H
#define ESPNOW_HANDLER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "protocol/RtcmEspNowProtocol.h"

struct EspNowRxPacket {
    uint16_t length;
    uint8_t data[rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE];
};

struct EspNowRtcmStats {
    uint32_t packetsReceived;
    uint32_t packetsWrongSource;
    uint32_t packetsInvalidHeader;
    uint32_t queueOverflow;
    uint32_t duplicateFragments;
    uint32_t frameTimeouts;
    uint32_t sequenceGaps;
    uint32_t crcErrors;
    uint32_t framesWritten;
    uint32_t uartWriteErrors;
    uint32_t lastValidFrameMillis;
};

bool espnowSetup();
bool espnowRefreshPeerChannel();
bool espnowIsReady();
QueueHandle_t espnowGetReceiveQueue();
EspNowRtcmStats espnowGetStats();

void espnowRecordInvalidHeader();
void espnowRecordDuplicateFragment();
void espnowRecordFrameTimeout();
void espnowRecordSequenceGaps(uint32_t count);
void espnowRecordCrcError();
void espnowRecordFrameWritten();
void espnowRecordUartWriteError();

#endif
