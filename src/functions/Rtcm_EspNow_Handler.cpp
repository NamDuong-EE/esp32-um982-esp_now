#include "functions/Rtcm_EspNow_Handler.h"

#include <cstring>
#include <freertos/semphr.h>

#include "Prog_Config.h"
#include "hardware/Espnow_handler.h"
#include "hardware/Relay_handler.h"
#include "protocol/RtcmEspNowProtocol.h"

extern SemaphoreHandle_t gnssTxMutex;

namespace {

struct ReassemblyState {
    bool active;
    uint16_t streamId;
    uint32_t frameSequence;
    uint16_t frameLength;
    uint8_t fragmentCount;
    uint8_t fragmentBitmap;
    uint8_t receivedFragments;
    uint32_t startedAt;
    uint8_t frame[rtcm_espnow::MAX_RTCM_FRAME_SIZE];
};

ReassemblyState state{};
bool haveLastCompletedFrame = false;
uint16_t lastCompletedStreamId = 0;
uint32_t lastCompletedSequence = 0;

void clearActiveFrame() {
    state.active = false;
    state.fragmentBitmap = 0;
    state.receivedFragments = 0;
}

void startFrame(const rtcm_espnow::RtcmEspNowHeader& header) {
    clearActiveFrame();
    state.active = true;
    state.streamId = header.streamId;
    state.frameSequence = header.frameSequence;
    state.frameLength = header.frameLength;
    state.fragmentCount = header.fragmentCount;
    state.startedAt = millis();
}

bool isNewerSequence(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

void checkTimeout() {
    if (state.active && millis() - state.startedAt > RTCM_REASSEMBLY_TIMEOUT_MS) {
        clearActiveFrame();
        espnowRecordFrameTimeout();
    }
}

void recordSequenceGap(uint16_t streamId, uint32_t sequence) {
    if (haveLastCompletedFrame && streamId == lastCompletedStreamId) {
        const uint32_t expected = lastCompletedSequence + 1U;
        if (sequence != expected && isNewerSequence(sequence, expected)) {
            espnowRecordSequenceGaps(sequence - expected);
        }
    }
    haveLastCompletedFrame = true;
    lastCompletedStreamId = streamId;
    lastCompletedSequence = sequence;
}

bool writeFrameToGnss() {
    if (gnssTxMutex == nullptr ||
        xSemaphoreTake(gnssTxMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        espnowRecordUartWriteError();
        return false;
    }
    const std::size_t written = Serial1.write(state.frame, state.frameLength);
    xSemaphoreGive(gnssTxMutex);
    if (written != state.frameLength) {
        espnowRecordUartWriteError();
        return false;
    }
    return true;
}

} // namespace

void resetRtcmEspNowReassembly() {
    std::memset(&state, 0, sizeof(state));
    haveLastCompletedFrame = false;
    lastCompletedStreamId = 0;
    lastCompletedSequence = 0;
}

bool processNextRtcmEspNowPacket(TickType_t waitTicks) {
    checkTimeout();
    QueueHandle_t queue = espnowGetReceiveQueue();
    if (queue == nullptr) {
        vTaskDelay(waitTicks);
        return false;
    }

    EspNowRxPacket packet{};
    if (xQueueReceive(queue, &packet, waitTicks) != pdTRUE) {
        checkTimeout();
        return false;
    }

    rtcm_espnow::RtcmEspNowHeader header{};
    std::memcpy(&header, packet.data, sizeof(header));
    if (!rtcm_espnow::validatePacketHeader(header, packet.length)) {
        espnowRecordInvalidHeader();
        return false;
    }

    if (haveLastCompletedFrame && header.streamId == lastCompletedStreamId &&
        !isNewerSequence(header.frameSequence, lastCompletedSequence)) {
        espnowRecordDuplicateFragment();
        if (header.frameSequence == lastCompletedSequence && header.fragmentIndex == 0) {
            espnowSendFrameAck(header.streamId, header.frameSequence);
        }
        return false;
    }

    if (!state.active) {
        startFrame(header);
    } else if (header.streamId != state.streamId) {
        espnowRecordFrameTimeout();
        startFrame(header);
    } else if (header.frameSequence != state.frameSequence) {
        if (!isNewerSequence(header.frameSequence, state.frameSequence)) {
            espnowRecordDuplicateFragment();
            return false;
        }
        espnowRecordFrameTimeout();
        startFrame(header);
    }

    if (header.frameLength != state.frameLength ||
        header.fragmentCount != state.fragmentCount) {
        espnowRecordInvalidHeader();
        clearActiveFrame();
        return false;
    }

    const uint8_t fragmentMask = static_cast<uint8_t>(1U << header.fragmentIndex);
    if ((state.fragmentBitmap & fragmentMask) != 0) {
        espnowRecordDuplicateFragment();
        if (header.fragmentIndex == 0) {
            state.startedAt = millis();
        }
        return false;
    }

    const std::size_t offset = static_cast<std::size_t>(header.fragmentIndex) *
                               rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE;
    std::memcpy(state.frame + offset,
                packet.data + sizeof(rtcm_espnow::RtcmEspNowHeader),
                header.payloadLength);
    state.fragmentBitmap |= fragmentMask;
    ++state.receivedFragments;

    if (state.receivedFragments != state.fragmentCount) {
        return false;
    }

    const uint16_t streamId = state.streamId;
    const uint32_t sequence = state.frameSequence;
    if (!rtcm_espnow::validateRtcm3Frame(state.frame, state.frameLength)) {
        espnowRecordCrcError();
        clearActiveFrame();
        return false;
    }
    if (!writeFrameToGnss()) {
        clearActiveFrame();
        return false;
    }

    recordSequenceGap(streamId, sequence);
    espnowRecordFrameWritten();
    // ACK upstream first. All ESP-NOW transmissions are serialized by the
    // shared TX manager, so downstream forwarding cannot overlap this ACK.
    espnowSendFrameAck(streamId, sequence);
    if constexpr (ROVER_RELAY_MODE) {
        // Downstream delivery is intentionally independent from the upstream
        // ACK. A missing/unpaired child must not make the Base rewrite RTCM to
        // this Rover's UART.
        relayQueueFrame(state.frame, state.frameLength, streamId, sequence);
    }
    clearActiveFrame();
    return true;
}
