#include "hardware/TemporaryBaseUplink.h"

#include <cstring>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Prog_Config.h"
#include "hardware/Espnow_handler.h"
#include "hardware/Espnow_tx_manager.h"
#include "protocol/RtcmEspNowProtocol.h"

namespace {
struct UplinkFrame {
    uint16_t length;
    uint8_t data[rtcm_espnow::MAX_RTCM_FRAME_SIZE];
};

struct UplinkAckEvent {
    uint16_t streamId;
    uint32_t frameSequence;
};

enum class ParserState : uint8_t {
    WaitPreamble,
    ReadLengthHigh,
    ReadLengthLow,
    ReadBody,
};

QueueHandle_t frameQueue = nullptr;
QueueHandle_t ackQueue = nullptr;
TemporaryBaseUplinkStats stats{};
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
ParserState parserState = ParserState::WaitPreamble;
uint8_t parserFrame[rtcm_espnow::MAX_RTCM_FRAME_SIZE] = {};
size_t parserIndex = 0;
size_t parserExpectedLength = 0;
uint16_t uplinkStreamId = 0;
uint32_t uplinkSequence = 0;

void incrementStat(uint32_t TemporaryBaseUplinkStats::*member)
{
    portENTER_CRITICAL(&stateMux);
    ++(stats.*member);
    portEXIT_CRITICAL(&stateMux);
}

void resetParser()
{
    parserState = ParserState::WaitPreamble;
    parserIndex = 0;
    parserExpectedLength = 0;
}

uint16_t newStreamId()
{
    uint16_t value = 0;
    while (value == 0) {
        value = static_cast<uint16_t>(esp_random() & 0xFFFFU);
    }
    return value;
}

bool isEnabled()
{
    portENTER_CRITICAL(&stateMux);
    const bool enabled = stats.enabled;
    portEXIT_CRITICAL(&stateMux);
    return enabled;
}

void queueParsedFrame(const uint8_t* frame, size_t length)
{
    incrementStat(&TemporaryBaseUplinkStats::framesParsed);
    UplinkFrame queued{};
    queued.length = static_cast<uint16_t>(length);
    std::memcpy(queued.data, frame, length);
    if (frameQueue == nullptr || xQueueSend(frameQueue, &queued, 0) != pdTRUE) {
        UplinkFrame stale{};
        if (frameQueue != nullptr && xQueueReceive(frameQueue, &stale, 0) == pdTRUE &&
            xQueueSend(frameQueue, &queued, 0) == pdTRUE) {
            incrementStat(&TemporaryBaseUplinkStats::queueOverflow);
            return;
        }
        incrementStat(&TemporaryBaseUplinkStats::queueOverflow);
        return;
    }
}

bool sendFragment(const uint8_t* baseMac,
                  const uint8_t* packet,
                  size_t packetLength,
                  uint32_t startedAtMs)
{
    for (uint8_t attempt = 0;
         attempt <= TEMP_BASE_UPLINK_FRAGMENT_RETRY_COUNT;
         ++attempt) {
        if (!isEnabled() || millis() - startedAtMs >= TEMP_BASE_UPLINK_FRAME_DEADLINE_MS) {
            return false;
        }
        const EspNowTxResult result = espnowTxSend(baseMac, packet, packetLength);
        if (result == EspNowTxResult::Success) {
            incrementStat(&TemporaryBaseUplinkStats::fragmentsSent);
            return true;
        }
        if (attempt < TEMP_BASE_UPLINK_FRAGMENT_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(TEMP_BASE_UPLINK_FRAGMENT_GAP_MS));
        }
    }
    incrementStat(&TemporaryBaseUplinkStats::fragmentFailures);
    return false;
}

bool waitForAck(uint16_t streamId, uint32_t sequence, uint32_t startedAtMs)
{
    while (millis() - startedAtMs < TEMP_BASE_UPLINK_FRAME_DEADLINE_MS) {
        const uint32_t elapsed = millis() - startedAtMs;
        const uint32_t remaining = TEMP_BASE_UPLINK_FRAME_DEADLINE_MS - elapsed;
        const uint32_t waitMs = min(TEMP_BASE_UPLINK_ACK_TIMEOUT_MS, remaining);
        UplinkAckEvent event{};
        if (xQueueReceive(ackQueue, &event, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
            incrementStat(&TemporaryBaseUplinkStats::ackTimeouts);
            return false;
        }
        if (event.streamId == streamId && event.frameSequence == sequence) {
            incrementStat(&TemporaryBaseUplinkStats::ackReceived);
            return true;
        }
    }
    incrementStat(&TemporaryBaseUplinkStats::ackTimeouts);
    return false;
}

bool sendFrame(const UplinkFrame& frame, uint16_t streamId, uint32_t sequence)
{
    uint8_t baseMac[6] = {};
    if (!espnowGetBaseMac(baseMac)) {
        return false;
    }
    while (ackQueue != nullptr) {
        UplinkAckEvent stale{};
        if (xQueueReceive(ackQueue, &stale, 0) != pdTRUE) {
            break;
        }
    }

    const uint8_t fragmentCount = static_cast<uint8_t>(
        (frame.length + rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE - 1) /
        rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE);
    uint8_t packet[rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE] = {};
    for (uint8_t frameAttempt = 0;
         frameAttempt <= TEMP_BASE_UPLINK_FRAME_RETRY_COUNT;
         ++frameAttempt) {
        const uint32_t startedAtMs = millis();
        bool allFragmentsSent = true;
        for (uint8_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex) {
            const size_t offset = static_cast<size_t>(fragmentIndex) *
                                  rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE;
            const size_t payloadLength = min(
                rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE,
                static_cast<size_t>(frame.length) - offset);
            auto* header = reinterpret_cast<rtcm_espnow::RtcmEspNowHeader*>(packet);
            header->magic = rtcm_espnow::MAGIC;
            header->version = rtcm_espnow::VERSION;
            header->packetType = rtcm_espnow::PACKET_TYPE_TEMP_RTCM_DATA;
            header->streamId = streamId;
            header->frameSequence = sequence;
            header->frameLength = frame.length;
            header->fragmentIndex = fragmentIndex;
            header->fragmentCount = fragmentCount;
            header->payloadLength = static_cast<uint16_t>(payloadLength);
            std::memcpy(packet + sizeof(*header), frame.data + offset, payloadLength);
            if (!sendFragment(baseMac,
                              packet,
                              sizeof(*header) + payloadLength,
                              startedAtMs)) {
                allFragmentsSent = false;
                break;
            }
            if (TEMP_BASE_UPLINK_FRAGMENT_GAP_MS > 0) {
                vTaskDelay(pdMS_TO_TICKS(TEMP_BASE_UPLINK_FRAGMENT_GAP_MS));
            }
        }
        if (allFragmentsSent && waitForAck(streamId, sequence, startedAtMs)) {
            return true;
        }
        if (frameAttempt < TEMP_BASE_UPLINK_FRAME_RETRY_COUNT) {
            incrementStat(&TemporaryBaseUplinkStats::frameRetries);
        }
    }
    return false;
}
}

bool temporaryBaseUplinkSetup()
{
    if (frameQueue != nullptr && ackQueue != nullptr) {
        return true;
    }
    frameQueue = xQueueCreate(TEMP_BASE_UPLINK_QUEUE_LENGTH, sizeof(UplinkFrame));
    ackQueue = xQueueCreate(4, sizeof(UplinkAckEvent));
    if (frameQueue == nullptr || ackQueue == nullptr) {
        Serial.println("[TEMP_BASE][UPLINK][ERROR] Cannot create queues");
        return false;
    }
    uplinkStreamId = newStreamId();
    resetParser();
    Serial.printf("[TEMP_BASE][UPLINK] Ready streamId=%u\n", uplinkStreamId);
    return true;
}

void temporaryBaseUplinkSetEnabled(bool enabled)
{
    bool changed = false;
    portENTER_CRITICAL(&stateMux);
    changed = stats.enabled != enabled;
    stats.enabled = enabled;
    if (enabled && changed) {
        uplinkStreamId = newStreamId();
        uplinkSequence = 0;
    }
    portEXIT_CRITICAL(&stateMux);
    if (!changed) {
        return;
    }
    resetParser();
    if (frameQueue != nullptr) {
        xQueueReset(frameQueue);
    }
    if (ackQueue != nullptr) {
        xQueueReset(ackQueue);
    }
    Serial.printf("[TEMP_BASE][UPLINK] %s streamId=%u\n",
                  enabled ? "enabled" : "disabled",
                  uplinkStreamId);
}

bool temporaryBaseUplinkIsEnabled()
{
    return isEnabled();
}

void temporaryBaseUplinkConsumeGnssByte(uint8_t value)
{
    if (!isEnabled()) {
        return;
    }
    ++stats.uartBytes;
    switch (parserState) {
    case ParserState::WaitPreamble:
        if (value == 0xD3) {
            parserFrame[0] = value;
            parserIndex = 1;
            parserState = ParserState::ReadLengthHigh;
        }
        break;
    case ParserState::ReadLengthHigh:
        if ((value & 0xFCU) != 0) {
            resetParser();
            break;
        }
        parserFrame[parserIndex++] = value;
        parserState = ParserState::ReadLengthLow;
        break;
    case ParserState::ReadLengthLow: {
        parserFrame[parserIndex++] = value;
        const size_t payloadLength =
            (static_cast<size_t>(parserFrame[1] & 0x03U) << 8) | parserFrame[2];
        parserExpectedLength = payloadLength + 6;
        if (parserExpectedLength > rtcm_espnow::MAX_RTCM_FRAME_SIZE) {
            resetParser();
            incrementStat(&TemporaryBaseUplinkStats::crcErrors);
            break;
        }
        parserState = ParserState::ReadBody;
        break;
    }
    case ParserState::ReadBody:
        parserFrame[parserIndex++] = value;
        if (parserIndex >= parserExpectedLength) {
            if (rtcm_espnow::validateRtcm3Frame(parserFrame, parserExpectedLength)) {
                queueParsedFrame(parserFrame, parserExpectedLength);
            } else {
                incrementStat(&TemporaryBaseUplinkStats::crcErrors);
            }
            resetParser();
        }
        break;
    }
}

bool temporaryBaseUplinkHandleAck(const uint8_t* sourceMac,
                                  const uint8_t* data,
                                  int length)
{
    if (data == nullptr || length != static_cast<int>(sizeof(rtcm_espnow::RtcmEspNowAck))) {
        return false;
    }
    rtcm_espnow::RtcmEspNowAck ack{};
    std::memcpy(&ack, data, sizeof(ack));
    if (ack.magic != rtcm_espnow::MAGIC || ack.version != rtcm_espnow::VERSION ||
        ack.packetType != rtcm_espnow::PACKET_TYPE_TEMP_RTCM_ACK ||
        ack.status != rtcm_espnow::ACK_STATUS_WRITTEN) {
        return false;
    }
    uint8_t baseMac[6] = {};
    if (!espnowGetBaseMac(baseMac) || sourceMac == nullptr ||
        std::memcmp(sourceMac, baseMac, sizeof(baseMac)) != 0) {
        return true;
    }
    UplinkAckEvent event{ack.streamId, ack.frameSequence};
    if (ackQueue != nullptr) {
        xQueueSend(ackQueue, &event, 0);
    }
    return true;
}

void temporaryBaseUplinkTask(void*)
{
    while (true) {
        UplinkFrame frame{};
        if (frameQueue == nullptr ||
            xQueueReceive(frameQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!isEnabled()) {
            continue;
        }
        uint16_t streamId = 0;
        uint32_t sequence = 0;
        portENTER_CRITICAL(&stateMux);
        streamId = uplinkStreamId;
        sequence = uplinkSequence++;
        portEXIT_CRITICAL(&stateMux);
        if (sendFrame(frame, streamId, sequence)) {
            portENTER_CRITICAL(&stateMux);
            ++stats.framesSent;
            stats.lastFrameSentAtMs = millis();
            portEXIT_CRITICAL(&stateMux);
        } else {
            incrementStat(&TemporaryBaseUplinkStats::framesDropped);
            Serial.printf("[TEMP_BASE][UPLINK][WARN] Drop seq=%lu without Base ACK\n",
                          static_cast<unsigned long>(sequence));
        }
    }
}

TemporaryBaseUplinkStats temporaryBaseUplinkGetStats()
{
    portENTER_CRITICAL(&stateMux);
    const TemporaryBaseUplinkStats copy = stats;
    portEXIT_CRITICAL(&stateMux);
    return copy;
}
