#ifndef RTCM_ESPNOW_PROTOCOL_H
#define RTCM_ESPNOW_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace rtcm_espnow {

inline constexpr uint16_t MAGIC = 0x5452; // Little-endian wire bytes: 'R', 'T'.
inline constexpr uint8_t VERSION = 1;
inline constexpr uint8_t PACKET_TYPE_RTCM_DATA = 1;
inline constexpr uint8_t PACKET_TYPE_FRAME_ACK = 2;
inline constexpr uint8_t ACK_STATUS_WRITTEN = 1;
inline constexpr std::size_t ESPNOW_V1_MAX_PACKET_SIZE = 250;
inline constexpr std::size_t MAX_RTCM_FRAME_SIZE = 1029;
inline constexpr std::size_t MIN_RTCM_FRAME_SIZE = 6;
inline constexpr std::size_t MAX_FRAGMENT_PAYLOAD_SIZE = 234;
inline constexpr uint8_t MAX_FRAGMENT_COUNT = 5;

#pragma pack(push, 1)
struct RtcmEspNowHeader {
    uint16_t magic;
    uint8_t version;
    uint8_t packetType;
    uint16_t streamId;
    uint32_t frameSequence;
    uint16_t frameLength;
    uint8_t fragmentIndex;
    uint8_t fragmentCount;
    uint16_t payloadLength;
};

struct RtcmEspNowAck {
    uint16_t magic;
    uint8_t version;
    uint8_t packetType;
    uint16_t streamId;
    uint32_t frameSequence;
    uint8_t status;
    uint8_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RtcmEspNowHeader) == 16, "RTCM ESP-NOW header must be 16 bytes");
static_assert(sizeof(RtcmEspNowAck) == 12, "RTCM ESP-NOW ACK must be 12 bytes");
static_assert(sizeof(RtcmEspNowHeader) + MAX_FRAGMENT_PAYLOAD_SIZE == ESPNOW_V1_MAX_PACKET_SIZE,
              "RTCM ESP-NOW packet must fit ESP-NOW v1");

uint8_t expectedFragmentCount(uint16_t frameLength);
uint16_t expectedFragmentPayloadLength(uint16_t frameLength, uint8_t fragmentIndex);
bool validatePacketHeader(const RtcmEspNowHeader& header, std::size_t receivedLength);
bool validateFrameAck(const RtcmEspNowAck& ack, std::size_t receivedLength);

uint32_t crc24q(const uint8_t* data, std::size_t length);
bool validateRtcm3Frame(const uint8_t* frame, std::size_t length);

} // namespace rtcm_espnow

#endif
