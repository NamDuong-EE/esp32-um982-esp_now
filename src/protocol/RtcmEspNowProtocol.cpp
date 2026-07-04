#include "protocol/RtcmEspNowProtocol.h"

namespace rtcm_espnow {

uint8_t expectedFragmentCount(uint16_t frameLength) {
    if (frameLength < MIN_RTCM_FRAME_SIZE || frameLength > MAX_RTCM_FRAME_SIZE) {
        return 0;
    }
    return static_cast<uint8_t>((frameLength + MAX_FRAGMENT_PAYLOAD_SIZE - 1) /
                                MAX_FRAGMENT_PAYLOAD_SIZE);
}

uint16_t expectedFragmentPayloadLength(uint16_t frameLength, uint8_t fragmentIndex) {
    const uint8_t fragmentCount = expectedFragmentCount(frameLength);
    if (fragmentCount == 0 || fragmentIndex >= fragmentCount) {
        return 0;
    }

    const std::size_t offset = static_cast<std::size_t>(fragmentIndex) *
                               MAX_FRAGMENT_PAYLOAD_SIZE;
    const std::size_t remaining = static_cast<std::size_t>(frameLength) - offset;
    return static_cast<uint16_t>(remaining > MAX_FRAGMENT_PAYLOAD_SIZE
                                     ? MAX_FRAGMENT_PAYLOAD_SIZE
                                     : remaining);
}

bool validatePacketHeader(const RtcmEspNowHeader& header, std::size_t receivedLength) {
    if (receivedLength < sizeof(RtcmEspNowHeader) ||
        receivedLength > ESPNOW_V1_MAX_PACKET_SIZE) {
        return false;
    }
    if (header.magic != MAGIC || header.version != VERSION ||
        header.packetType != PACKET_TYPE_RTCM_DATA) {
        return false;
    }

    const uint8_t expectedCount = expectedFragmentCount(header.frameLength);
    if (expectedCount == 0 || header.fragmentCount != expectedCount ||
        header.fragmentIndex >= header.fragmentCount) {
        return false;
    }

    const uint16_t expectedPayload =
        expectedFragmentPayloadLength(header.frameLength, header.fragmentIndex);
    if (header.payloadLength != expectedPayload) {
        return false;
    }

    return receivedLength == sizeof(RtcmEspNowHeader) + header.payloadLength;
}

uint32_t crc24q(const uint8_t* data, std::size_t length) {
    if (data == nullptr && length != 0) {
        return 0;
    }

    uint32_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 16;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc <<= 1;
            if ((crc & 0x1000000U) != 0) {
                crc ^= 0x1864CFBU;
            }
        }
    }
    return crc & 0xFFFFFFU;
}

bool validateRtcm3Frame(const uint8_t* frame, std::size_t length) {
    if (frame == nullptr || length < MIN_RTCM_FRAME_SIZE ||
        length > MAX_RTCM_FRAME_SIZE || frame[0] != 0xD3 ||
        (frame[1] & 0xFCU) != 0) {
        return false;
    }

    const std::size_t payloadLength =
        (static_cast<std::size_t>(frame[1] & 0x03U) << 8) | frame[2];
    if (payloadLength + MIN_RTCM_FRAME_SIZE != length) {
        return false;
    }

    const uint32_t expected = crc24q(frame, length - 3);
    const uint32_t received = (static_cast<uint32_t>(frame[length - 3]) << 16) |
                              (static_cast<uint32_t>(frame[length - 2]) << 8) |
                              static_cast<uint32_t>(frame[length - 1]);
    return expected == received;
}

} // namespace rtcm_espnow
