#ifndef RTCM_ESPNOW_PROTOCOL_H
#define RTCM_ESPNOW_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace rtcm_espnow {

inline constexpr uint16_t MAGIC = 0x5452; // Little-endian wire bytes: 'R', 'T'.
inline constexpr uint8_t VERSION = 1;
inline constexpr uint8_t PACKET_TYPE_RTCM_DATA = 1;
inline constexpr uint8_t PACKET_TYPE_FRAME_ACK = 2;
inline constexpr uint8_t PACKET_TYPE_PAIR_DISCOVERY = 3;
inline constexpr uint8_t PACKET_TYPE_PAIR_RESPONSE = 4;
inline constexpr uint8_t PACKET_TYPE_PAIR_CONFIRM = 5;
inline constexpr uint8_t ROLE_BASE = 1;
inline constexpr uint8_t ROLE_ROVER = 2;
inline constexpr uint8_t ACK_STATUS_WRITTEN = 1;
inline constexpr std::size_t ESPNOW_V1_MAX_PACKET_SIZE = 250;
inline constexpr std::size_t MAX_RTCM_FRAME_SIZE = 1029;
inline constexpr std::size_t MIN_RTCM_FRAME_SIZE = 6;
inline constexpr std::size_t MAX_FRAGMENT_PAYLOAD_SIZE = 234;
inline constexpr uint8_t MAX_FRAGMENT_COUNT = 5;

#pragma pack(push, 1)
struct EspNowCommonHeader {
    uint16_t magic;
    uint8_t version;
    uint8_t packetType;
};

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

struct PairDiscoveryPacket {
    EspNowCommonHeader common;
    uint8_t role;
    uint8_t reserved[3];
    uint32_t networkId;
    uint32_t baseDeviceId;
    uint32_t baseNonce;
    uint32_t pairingWindowMs;
    uint32_t authTag;
};

struct PairResponsePacket {
    EspNowCommonHeader common;
    uint8_t role;
    uint8_t reserved[3];
    uint32_t networkId;
    uint32_t roverDeviceId;
    uint32_t roverNonce;
    uint32_t baseNonceEcho;
    uint32_t authTag;
};

struct PairConfirmPacket {
    EspNowCommonHeader common;
    uint8_t role;
    uint8_t reserved[3];
    uint32_t networkId;
    uint32_t baseNonce;
    uint32_t roverNonce;
    uint32_t authTag;
};
#pragma pack(pop)

static_assert(sizeof(EspNowCommonHeader) == 4, "ESP-NOW common header must be 4 bytes");
static_assert(sizeof(RtcmEspNowHeader) == 16, "RTCM ESP-NOW header must be 16 bytes");
static_assert(sizeof(RtcmEspNowAck) == 12, "RTCM ESP-NOW ACK must be 12 bytes");
static_assert(sizeof(PairDiscoveryPacket) == 28, "PAIR_DISCOVERY must be 28 bytes");
static_assert(sizeof(PairResponsePacket) == 28, "PAIR_RESPONSE must be 28 bytes");
static_assert(sizeof(PairConfirmPacket) == 24, "PAIR_CONFIRM must be 24 bytes");
static_assert(sizeof(RtcmEspNowHeader) + MAX_FRAGMENT_PAYLOAD_SIZE == ESPNOW_V1_MAX_PACKET_SIZE,
              "RTCM ESP-NOW packet must fit ESP-NOW v1");

uint8_t expectedFragmentCount(uint16_t frameLength);
uint16_t expectedFragmentPayloadLength(uint16_t frameLength, uint8_t fragmentIndex);
bool validatePacketHeader(const RtcmEspNowHeader& header, std::size_t receivedLength);
bool validateFrameAck(const RtcmEspNowAck& ack, std::size_t receivedLength);

uint32_t computePairingAuthTag(const uint8_t* data,
                               std::size_t lengthWithoutAuthTag,
                               const uint8_t* pairingKey,
                               std::size_t pairingKeyLength);
uint32_t pairingAuthTag(const PairDiscoveryPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength);
uint32_t pairingAuthTag(const PairResponsePacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength);
uint32_t pairingAuthTag(const PairConfirmPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength);
bool validatePairDiscovery(const PairDiscoveryPacket& packet,
                           std::size_t receivedLength,
                           uint32_t expectedNetworkId,
                           const uint8_t* pairingKey,
                           std::size_t pairingKeyLength);
bool validatePairResponse(const PairResponsePacket& packet,
                          std::size_t receivedLength,
                          uint32_t expectedNetworkId,
                          uint32_t expectedBaseNonce,
                          const uint8_t* pairingKey,
                          std::size_t pairingKeyLength);
bool validatePairConfirm(const PairConfirmPacket& packet,
                         std::size_t receivedLength,
                         uint32_t expectedNetworkId,
                         uint32_t expectedBaseNonce,
                         uint32_t expectedRoverNonce,
                         const uint8_t* pairingKey,
                         std::size_t pairingKeyLength);

uint32_t crc24q(const uint8_t* data, std::size_t length);
bool validateRtcm3Frame(const uint8_t* frame, std::size_t length);

} // namespace rtcm_espnow

#endif
