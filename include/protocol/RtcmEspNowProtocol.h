#ifndef RTCM_ESPNOW_PROTOCOL_H
#define RTCM_ESPNOW_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace rtcm_espnow {

inline constexpr uint16_t MAGIC = 0x5452; // Little-endian wire bytes: 'R', 'T'.
inline constexpr uint8_t VERSION = 2;
inline constexpr uint8_t PACKET_TYPE_RTCM_DATA = 1;
inline constexpr uint8_t PACKET_TYPE_FRAME_ACK = 2;
inline constexpr uint8_t PACKET_TYPE_PAIR_DISCOVERY = 3;
inline constexpr uint8_t PACKET_TYPE_PAIR_RESPONSE = 4;
inline constexpr uint8_t PACKET_TYPE_PAIR_CONFIRM = 5;
inline constexpr uint8_t PACKET_TYPE_ROVER_ECEF_STATUS = 6;
inline constexpr uint8_t PACKET_TYPE_RELAYED_ROVER_ECEF_STATUS = 7;
inline constexpr uint8_t PACKET_TYPE_GNSS_COMMAND_REQUEST = 8;
inline constexpr uint8_t PACKET_TYPE_GNSS_COMMAND_RESULT = 9;
inline constexpr uint8_t PACKET_TYPE_TEMP_RTCM_DATA = 10;
inline constexpr uint8_t PACKET_TYPE_TEMP_RTCM_ACK = 11;
inline constexpr uint8_t ROLE_BASE = 1;
inline constexpr uint8_t ROLE_ROVER = 2;
inline constexpr uint8_t GNSS_COMMAND_SWITCH_TO_ROVER = 2;
inline constexpr uint8_t GNSS_COMMAND_SWITCH_TO_BASE_FIXED_ECEF = 3;
inline constexpr uint8_t GNSS_COMMAND_RESET_RTK = 4;
inline constexpr uint8_t GNSS_COMMAND_RESUME_RTK = 5;
inline constexpr uint8_t GNSS_PORT_COM2 = 2;
inline constexpr uint8_t GNSS_COMMAND_STATUS_UART_SEQUENCE_WRITTEN = 1;
inline constexpr uint8_t GNSS_COMMAND_STATUS_REJECTED = 2;
inline constexpr uint8_t GNSS_COMMAND_STATUS_UART_ERROR = 3;
inline constexpr uint8_t GNSS_COMMAND_STATUS_BUSY = 4;
inline constexpr uint16_t GNSS_COMMAND_DETAIL_NONE = 0;
inline constexpr uint16_t GNSS_COMMAND_DETAIL_INVALID_REQUEST = 1;
inline constexpr uint16_t GNSS_COMMAND_DETAIL_QUEUE_FULL = 2;
inline constexpr uint16_t GNSS_COMMAND_DETAIL_UART_WRITE = 3;
inline constexpr uint8_t ACK_STATUS_WRITTEN = 1;
inline constexpr double ECEF_SCALE = 10000.0;
inline constexpr int64_t ECEF_SCALED_LIMIT = 70000000000LL;
inline constexpr uint32_t GNSS_MILLISECONDS_PER_DAY = 86400000UL;
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

struct RoverEcefStatusPacket {
    EspNowCommonHeader common;
    uint32_t sequence;
    uint32_t gnssTimeMsOfDay;
    uint16_t correctionStreamId;
    uint8_t fixQuality;
    uint8_t reserved;
    int64_t ecefXScaled;
    int64_t ecefYScaled;
    int64_t ecefZScaled;
};

struct RelayedRoverEcefStatusPacket {
    EspNowCommonHeader common;
    uint32_t sequence;
    uint8_t roverMac[6];
    uint32_t gnssTimeMsOfDay;
    uint16_t correctionStreamId;
    uint8_t fixQuality;
    uint8_t reserved[3];
    int64_t ecefXScaled;
    int64_t ecefYScaled;
    int64_t ecefZScaled;
};

struct GnssCommandRequestPacket {
    EspNowCommonHeader common;
    uint32_t networkId;
    uint32_t transactionId;
    int64_t ecefXScaled;
    int64_t ecefYScaled;
    int64_t ecefZScaled;
    uint8_t commandId;
    uint8_t targetPort;
    uint16_t reserved;
    uint32_t authTag;
};

struct GnssCommandResultPacket {
    EspNowCommonHeader common;
    uint32_t networkId;
    uint32_t transactionId;
    uint8_t commandId;
    uint8_t status;
    uint8_t completedStep;
    uint8_t totalSteps;
    uint16_t detailCode;
    uint16_t reserved;
    uint32_t authTag;
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
static_assert(sizeof(RoverEcefStatusPacket) == 40,
              "ROVER_ECEF_STATUS must be 40 bytes");
static_assert(sizeof(RelayedRoverEcefStatusPacket) == 48,
              "RELAYED_ROVER_ECEF_STATUS must be 48 bytes");
static_assert(sizeof(GnssCommandRequestPacket) == 44,
              "GNSS_COMMAND_REQUEST must be 44 bytes");
static_assert(sizeof(GnssCommandResultPacket) == 24,
              "GNSS_COMMAND_RESULT must be 24 bytes");
static_assert(sizeof(PairDiscoveryPacket) == 28, "PAIR_DISCOVERY must be 28 bytes");
static_assert(sizeof(PairResponsePacket) == 28, "PAIR_RESPONSE must be 28 bytes");
static_assert(sizeof(PairConfirmPacket) == 24, "PAIR_CONFIRM must be 24 bytes");
static_assert(sizeof(RtcmEspNowHeader) + MAX_FRAGMENT_PAYLOAD_SIZE == ESPNOW_V1_MAX_PACKET_SIZE,
              "RTCM ESP-NOW packet must fit ESP-NOW v1");

uint8_t expectedFragmentCount(uint16_t frameLength);
uint16_t expectedFragmentPayloadLength(uint16_t frameLength, uint8_t fragmentIndex);
bool validatePacketHeader(const RtcmEspNowHeader& header, std::size_t receivedLength);
bool validateFrameAck(const RtcmEspNowAck& ack, std::size_t receivedLength);
bool validateRoverEcefStatus(const RoverEcefStatusPacket& packet,
                             std::size_t receivedLength);
bool validateRelayedRoverEcefStatus(const RelayedRoverEcefStatusPacket& packet,
                                    std::size_t receivedLength);
bool validateGnssCommandRequest(const GnssCommandRequestPacket& packet,
                                std::size_t receivedLength,
                                uint32_t expectedNetworkId,
                                const uint8_t* pairingKey,
                                std::size_t pairingKeyLength);
bool validateGnssCommandResult(const GnssCommandResultPacket& packet,
                               std::size_t receivedLength,
                               uint32_t expectedNetworkId,
                               const uint8_t* pairingKey,
                               std::size_t pairingKeyLength);

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
uint32_t pairingAuthTag(const GnssCommandRequestPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength);
uint32_t pairingAuthTag(const GnssCommandResultPacket& packet,
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
