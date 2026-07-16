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
    uint32_t queueHighWater;
    uint32_t duplicateFragments;
    uint32_t frameTimeouts;
    uint32_t sequenceGaps;
    uint32_t crcErrors;
    uint32_t framesWritten;
    uint32_t uartWriteErrors;
    uint32_t ackPacketsQueued;
    uint32_t ackSendFailures;
    uint32_t lastValidFrameMillis;
    uint32_t pairDiscoveryReceived;
    uint32_t pairResponsesSent;
    uint32_t pairConfirmsAccepted;
    uint32_t pairAuthFailures;
    bool hasStoredBaseMac;
    bool pairingActive;
};

bool espnowPrepare();
bool espnowSetup();
bool espnowRefreshPeerChannel();
bool espnowIsReady();
void espnowLoop();
QueueHandle_t espnowGetReceiveQueue();
EspNowRtcmStats espnowGetStats();
bool espnowSendFrameAck(uint16_t streamId, uint32_t frameSequence);
bool espnowGetBaseMac(uint8_t mac[6]);

void espnowRecordInvalidHeader();
void espnowRecordDuplicateFragment();
void espnowRecordFrameTimeout();
void espnowRecordSequenceGaps(uint32_t count);
void espnowRecordCrcError();
void espnowRecordFrameWritten();
void espnowRecordUartWriteError();

#endif
