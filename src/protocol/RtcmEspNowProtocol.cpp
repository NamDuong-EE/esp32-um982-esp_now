#include "protocol/RtcmEspNowProtocol.h"

#include <cstddef>

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

bool validateFrameAck(const RtcmEspNowAck& ack, std::size_t receivedLength) {
    return receivedLength == sizeof(RtcmEspNowAck) &&
           ack.magic == MAGIC &&
           ack.version == VERSION &&
           ack.packetType == PACKET_TYPE_FRAME_ACK &&
           ack.status == ACK_STATUS_WRITTEN;
}

bool validateRoverLlhStatus(const RoverLlhStatusPacket& packet,
                            std::size_t receivedLength) {
    return receivedLength == sizeof(RoverLlhStatusPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_ROVER_LLH_STATUS &&
           packet.latitudeE7 >= -900000000 && packet.latitudeE7 <= 900000000 &&
           packet.longitudeE7 >= -1800000000 && packet.longitudeE7 <= 1800000000 &&
           packet.fixQuality <= 8;
}

bool validateRelayedRoverLlhStatus(const RelayedRoverLlhStatusPacket& packet,
                                   std::size_t receivedLength) {
    bool macConfigured = false;
    bool macBroadcast = true;
    for (uint8_t octet : packet.roverMac) {
        macConfigured = macConfigured || octet != 0;
        macBroadcast = macBroadcast && octet == 0xFF;
    }
    return receivedLength == sizeof(RelayedRoverLlhStatusPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_RELAYED_ROVER_LLH_STATUS &&
           macConfigured && !macBroadcast && (packet.roverMac[0] & 0x01U) == 0 &&
           packet.latitudeE7 >= -900000000 && packet.latitudeE7 <= 900000000 &&
           packet.longitudeE7 >= -1800000000 && packet.longitudeE7 <= 1800000000 &&
           packet.fixQuality <= 8;
}

bool validateGnssCommandRequest(const GnssCommandRequestPacket& packet,
                                std::size_t receivedLength,
                                uint32_t expectedNetworkId,
                                const uint8_t* pairingKey,
                                std::size_t pairingKeyLength) {
    const bool validEcef =
        packet.ecefXmm >= -ECEF_MM_LIMIT && packet.ecefXmm <= ECEF_MM_LIMIT &&
        packet.ecefYmm >= -ECEF_MM_LIMIT && packet.ecefYmm <= ECEF_MM_LIMIT &&
        packet.ecefZmm >= -ECEF_MM_LIMIT && packet.ecefZmm <= ECEF_MM_LIMIT &&
        (packet.ecefXmm < -90000 || packet.ecefXmm > 90000);
    const bool validCommandParameters =
        (packet.commandId == GNSS_COMMAND_SWITCH_TO_ROVER &&
         packet.ecefXmm == 0 && packet.ecefYmm == 0 && packet.ecefZmm == 0) ||
        (packet.commandId == GNSS_COMMAND_SWITCH_TO_BASE_FIXED_ECEF &&
         validEcef);
    return receivedLength == sizeof(GnssCommandRequestPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_GNSS_COMMAND_REQUEST &&
           packet.networkId == expectedNetworkId &&
           packet.transactionId != 0 &&
           validCommandParameters &&
           packet.targetPort == GNSS_PORT_COM2 &&
           packet.authTag == pairingAuthTag(packet, pairingKey, pairingKeyLength);
}

bool validateGnssCommandResult(const GnssCommandResultPacket& packet,
                               std::size_t receivedLength,
                               uint32_t expectedNetworkId,
                               const uint8_t* pairingKey,
                               std::size_t pairingKeyLength) {
    return receivedLength == sizeof(GnssCommandResultPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_GNSS_COMMAND_RESULT &&
           packet.networkId == expectedNetworkId &&
           packet.transactionId != 0 &&
           (packet.commandId == GNSS_COMMAND_SWITCH_TO_ROVER ||
            packet.commandId == GNSS_COMMAND_SWITCH_TO_BASE_FIXED_ECEF) &&
           packet.status >= GNSS_COMMAND_STATUS_UART_SEQUENCE_WRITTEN &&
           packet.status <= GNSS_COMMAND_STATUS_BUSY &&
           packet.authTag == pairingAuthTag(packet, pairingKey, pairingKeyLength);
}

uint32_t computePairingAuthTag(const uint8_t* data,
                               std::size_t lengthWithoutAuthTag,
                               const uint8_t* pairingKey,
                               std::size_t pairingKeyLength) {
    if ((data == nullptr && lengthWithoutAuthTag != 0) ||
        (pairingKey == nullptr && pairingKeyLength != 0)) {
        return 0;
    }

    uint32_t hash = 2166136261UL;
    for (std::size_t index = 0; index < lengthWithoutAuthTag; ++index) {
        hash ^= data[index];
        hash *= 16777619UL;
    }
    hash ^= 0x9E3779B9UL;
    for (std::size_t index = 0; index < pairingKeyLength; ++index) {
        hash ^= pairingKey[index];
        hash *= 16777619UL;
    }
    return hash == 0 ? 0xFFFFFFFFUL : hash;
}

uint32_t pairingAuthTag(const PairDiscoveryPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength) {
    return computePairingAuthTag(reinterpret_cast<const uint8_t*>(&packet),
                                 offsetof(PairDiscoveryPacket, authTag),
                                 pairingKey,
                                 pairingKeyLength);
}

uint32_t pairingAuthTag(const PairResponsePacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength) {
    return computePairingAuthTag(reinterpret_cast<const uint8_t*>(&packet),
                                 offsetof(PairResponsePacket, authTag),
                                 pairingKey,
                                 pairingKeyLength);
}

uint32_t pairingAuthTag(const PairConfirmPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength) {
    return computePairingAuthTag(reinterpret_cast<const uint8_t*>(&packet),
                                 offsetof(PairConfirmPacket, authTag),
                                 pairingKey,
                                 pairingKeyLength);
}

uint32_t pairingAuthTag(const GnssCommandRequestPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength) {
    return computePairingAuthTag(reinterpret_cast<const uint8_t*>(&packet),
                                 offsetof(GnssCommandRequestPacket, authTag),
                                 pairingKey,
                                 pairingKeyLength);
}

uint32_t pairingAuthTag(const GnssCommandResultPacket& packet,
                        const uint8_t* pairingKey,
                        std::size_t pairingKeyLength) {
    return computePairingAuthTag(reinterpret_cast<const uint8_t*>(&packet),
                                 offsetof(GnssCommandResultPacket, authTag),
                                 pairingKey,
                                 pairingKeyLength);
}

bool validatePairDiscovery(const PairDiscoveryPacket& packet,
                           std::size_t receivedLength,
                           uint32_t expectedNetworkId,
                           const uint8_t* pairingKey,
                           std::size_t pairingKeyLength) {
    return receivedLength == sizeof(PairDiscoveryPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_PAIR_DISCOVERY &&
           packet.role == ROLE_BASE &&
           packet.networkId == expectedNetworkId &&
           packet.authTag == pairingAuthTag(packet, pairingKey, pairingKeyLength);
}

bool validatePairResponse(const PairResponsePacket& packet,
                          std::size_t receivedLength,
                          uint32_t expectedNetworkId,
                          uint32_t expectedBaseNonce,
                          const uint8_t* pairingKey,
                          std::size_t pairingKeyLength) {
    return receivedLength == sizeof(PairResponsePacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_PAIR_RESPONSE &&
           packet.role == ROLE_ROVER &&
           packet.networkId == expectedNetworkId &&
           packet.baseNonceEcho == expectedBaseNonce &&
           packet.authTag == pairingAuthTag(packet, pairingKey, pairingKeyLength);
}

bool validatePairConfirm(const PairConfirmPacket& packet,
                         std::size_t receivedLength,
                         uint32_t expectedNetworkId,
                         uint32_t expectedBaseNonce,
                         uint32_t expectedRoverNonce,
                         const uint8_t* pairingKey,
                         std::size_t pairingKeyLength) {
    return receivedLength == sizeof(PairConfirmPacket) &&
           packet.common.magic == MAGIC &&
           packet.common.version == VERSION &&
           packet.common.packetType == PACKET_TYPE_PAIR_CONFIRM &&
           packet.role == ROLE_BASE &&
           packet.networkId == expectedNetworkId &&
           packet.baseNonce == expectedBaseNonce &&
           packet.roverNonce == expectedRoverNonce &&
           packet.authTag == pairingAuthTag(packet, pairingKey, pairingKeyLength);
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
