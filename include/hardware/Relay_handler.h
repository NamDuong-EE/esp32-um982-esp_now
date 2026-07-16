#ifndef RELAY_HANDLER_H
#define RELAY_HANDLER_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

struct RelayStats {
    uint32_t framesQueued;
    uint32_t framesSent;
    uint32_t framesAcked;
    uint32_t fragmentsSent;
    uint32_t sendFailures;
    uint32_t sendImmediateErrors;
    uint32_t sendCallbackTimeouts;
    uint32_t sendDeliveryFailures;
    uint32_t ackTimeouts;
    uint32_t frameRetries;
    uint32_t backoffEvents;
    uint32_t queueOverflow;
    uint32_t queueHighWater;
    uint32_t queueDepth;
    uint32_t framesWithoutChild;
    uint32_t framesSuppressedDuringPairing;
    uint32_t childPairDiscoveriesSent;
    uint32_t childPairResponsesReceived;
    uint32_t childPairConfirmsSent;
    uint32_t childPairAuthFailures;
    uint32_t lastAckMillis;
    bool hasStoredChildMac;
    bool childPairingActive;
};

bool relaySetup();
bool relayIsReady();
bool relayIsChildPairingActive();
void relayLoop();
bool relayQueueFrame(const uint8_t* frame,
                     uint16_t frameLength,
                     uint16_t streamId,
                     uint32_t frameSequence);
bool relayProcessNextFrame(TickType_t waitTicks);
bool relayHandleReceivedPacket(const uint8_t* sourceMac,
                               const uint8_t* data,
                               int length);
RelayStats relayGetStats();
bool relayGetChildMac(uint8_t mac[6]);

#endif
