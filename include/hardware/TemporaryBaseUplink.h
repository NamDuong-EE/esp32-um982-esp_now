#ifndef TEMPORARY_BASE_UPLINK_H
#define TEMPORARY_BASE_UPLINK_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

struct TemporaryBaseUplinkStats {
    uint32_t uartBytes = 0;
    uint32_t framesParsed = 0;
    uint32_t crcErrors = 0;
    uint32_t queueOverflow = 0;
    uint32_t framesSent = 0;
    uint32_t framesDropped = 0;
    uint32_t fragmentsSent = 0;
    uint32_t fragmentFailures = 0;
    uint32_t frameRetries = 0;
    uint32_t ackReceived = 0;
    uint32_t ackTimeouts = 0;
    uint32_t lastFrameSentAtMs = 0;
    bool enabled = false;
};

bool temporaryBaseUplinkSetup();
void temporaryBaseUplinkSetEnabled(bool enabled);
bool temporaryBaseUplinkIsEnabled();
void temporaryBaseUplinkConsumeGnssByte(uint8_t value);
bool temporaryBaseUplinkHandleAck(const uint8_t* sourceMac,
                                  const uint8_t* data,
                                  int length);
void temporaryBaseUplinkTask(void* parameter);
TemporaryBaseUplinkStats temporaryBaseUplinkGetStats();

#endif // TEMPORARY_BASE_UPLINK_H
