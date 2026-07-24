#ifndef RELAY_HANDLER_H
#define RELAY_HANDLER_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

struct RelayChildStatus {
    uint8_t mac[6] = {};
    uint32_t framesSent = 0;
    uint32_t framesAcked = 0;
    uint32_t frameRetries = 0;
    uint32_t ackTimeouts = 0;
    uint32_t sendFailures = 0;
    uint32_t framesSkippedCooldown = 0;
    uint32_t llhReceived = 0;
    uint32_t llhForwarded = 0;
    uint32_t llhForwardSkipped = 0;
    uint32_t llhForwardFailures = 0;
    uint32_t lastAckMillis = 0;
    uint32_t cooldownUntilMs = 0;
    uint8_t consecutiveFailures = 0;
    bool stored = false;
};

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
    uint32_t childLlhReceived;
    uint32_t childLlhInvalid;
    uint32_t childLlhQueueOverwrites;
    uint32_t childLlhForwarded;
    uint32_t childLlhForwardSkipped;
    uint32_t childLlhForwardFailures;
    uint32_t childCount;
    uint32_t childClearEvents;
    uint32_t framesSkippedCooldown;
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
bool relayProcessNextLlh(TickType_t waitTicks);
bool relayHandleReceivedPacket(const uint8_t* sourceMac,
                               const uint8_t* data,
                               int length);
bool relayRequestChildrenRtkReset(uint32_t parentTransactionId);
bool relayRequestChildrenRtkResume(uint32_t parentTransactionId);
RelayStats relayGetStats();
bool relayGetChildMac(uint8_t mac[6]);
size_t relayCopyChildren(RelayChildStatus* destination, size_t capacity);

#endif
