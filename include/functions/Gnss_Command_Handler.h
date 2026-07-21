#ifndef GNSS_COMMAND_HANDLER_H
#define GNSS_COMMAND_HANDLER_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

struct GnssCommandStats {
    uint32_t requestsReceived = 0;
    uint32_t requestsInvalid = 0;
    uint32_t queueOverflow = 0;
    uint32_t duplicateRequests = 0;
    uint32_t sequencesCompleted = 0;
    uint32_t uartWriteFailures = 0;
    uint32_t resultsSent = 0;
    uint32_t resultSendFailures = 0;
    bool promotedToBase = false;
};

bool gnssCommandSetup();
bool gnssCommandHandleRequest(const uint8_t* sourceMac,
                              const uint8_t* data,
                              int length);
void gnssCommandTask(void* parameter);
bool gnssCommandAcceptsRtcmCorrection();
GnssCommandStats gnssCommandGetStats();

#endif // GNSS_COMMAND_HANDLER_H
