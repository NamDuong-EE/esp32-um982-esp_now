#include <unity.h>

#include <array>
#include <cstring>

#include "protocol/RtcmEspNowProtocol.h"

using namespace rtcm_espnow;

void setUp() {}
void tearDown() {}

void test_header_and_fragment_boundaries() {
    TEST_ASSERT_EQUAL_UINT32(16, sizeof(RtcmEspNowHeader));
    TEST_ASSERT_EQUAL_UINT8(1, expectedFragmentCount(6));
    TEST_ASSERT_EQUAL_UINT8(1, expectedFragmentCount(234));
    TEST_ASSERT_EQUAL_UINT8(2, expectedFragmentCount(235));
    TEST_ASSERT_EQUAL_UINT8(5, expectedFragmentCount(1029));
    TEST_ASSERT_EQUAL_UINT8(0, expectedFragmentCount(1030));
    TEST_ASSERT_EQUAL_UINT16(234, expectedFragmentPayloadLength(1029, 0));
    TEST_ASSERT_EQUAL_UINT16(93, expectedFragmentPayloadLength(1029, 4));
}

void test_packet_header_validation() {
    RtcmEspNowHeader header{};
    header.magic = MAGIC;
    header.version = VERSION;
    header.packetType = PACKET_TYPE_RTCM_DATA;
    header.streamId = 7;
    header.frameSequence = 42;
    header.frameLength = 235;
    header.fragmentIndex = 1;
    header.fragmentCount = 2;
    header.payloadLength = 1;

    TEST_ASSERT_TRUE(validatePacketHeader(header, sizeof(header) + 1));
    header.payloadLength = 2;
    TEST_ASSERT_FALSE(validatePacketHeader(header, sizeof(header) + 2));
}

void test_frame_ack_validation() {
    RtcmEspNowAck ack{};
    ack.magic = MAGIC;
    ack.version = VERSION;
    ack.packetType = PACKET_TYPE_FRAME_ACK;
    ack.streamId = 7;
    ack.frameSequence = 42;
    ack.status = ACK_STATUS_WRITTEN;

    TEST_ASSERT_EQUAL_UINT32(12, sizeof(RtcmEspNowAck));
    TEST_ASSERT_TRUE(validateFrameAck(ack, sizeof(ack)));
    ack.status = 0;
    TEST_ASSERT_FALSE(validateFrameAck(ack, sizeof(ack)));
}

void test_rover_llh_status_validation() {
    RoverLlhStatusPacket status{};
    status.common.magic = MAGIC;
    status.common.version = VERSION;
    status.common.packetType = PACKET_TYPE_ROVER_LLH_STATUS;
    status.sequence = 7;
    status.latitudeE7 = 210734567;
    status.longitudeE7 = 1058123456;
    status.heightMm = 12345;
    status.fixQuality = 4;

    TEST_ASSERT_EQUAL_UINT32(21, sizeof(RoverLlhStatusPacket));
    TEST_ASSERT_TRUE(validateRoverLlhStatus(status, sizeof(status)));
    status.latitudeE7 = 900000001;
    TEST_ASSERT_FALSE(validateRoverLlhStatus(status, sizeof(status)));
    status.latitudeE7 = 210734567;
    status.common.packetType = PACKET_TYPE_FRAME_ACK;
    TEST_ASSERT_FALSE(validateRoverLlhStatus(status, sizeof(status)));
    status.common.packetType = PACKET_TYPE_ROVER_LLH_STATUS;
    status.fixQuality = 9;
    TEST_ASSERT_FALSE(validateRoverLlhStatus(status, sizeof(status)));
}

void test_relayed_rover_llh_status_validation() {
    RelayedRoverLlhStatusPacket status{};
    status.common.magic = MAGIC;
    status.common.version = VERSION;
    status.common.packetType = PACKET_TYPE_RELAYED_ROVER_LLH_STATUS;
    status.sequence = 9;
    const uint8_t roverMac[6] = {0x58, 0x2A, 0xBD, 0x71, 0xE4, 0xF0};
    std::memcpy(status.roverMac, roverMac, sizeof(roverMac));
    status.latitudeE7 = 210734567;
    status.longitudeE7 = 1058123456;
    status.heightMm = 12345;
    status.fixQuality = 5;

    TEST_ASSERT_EQUAL_UINT32(28, sizeof(RelayedRoverLlhStatusPacket));
    TEST_ASSERT_TRUE(validateRelayedRoverLlhStatus(status, sizeof(status)));
    std::memset(status.roverMac, 0, sizeof(status.roverMac));
    TEST_ASSERT_FALSE(validateRelayedRoverLlhStatus(status, sizeof(status)));
    std::memcpy(status.roverMac, roverMac, sizeof(roverMac));
    status.fixQuality = 9;
    TEST_ASSERT_FALSE(validateRelayedRoverLlhStatus(status, sizeof(status)));
}

void test_pairing_packet_validation() {
    const std::array<uint8_t, 16> key{{0x10, 0x21, 0x32, 0x43,
                                      0x54, 0x65, 0x76, 0x87,
                                      0x98, 0xA9, 0xBA, 0xCB,
                                      0xDC, 0xED, 0xFE, 0x0F}};
    constexpr uint32_t networkId = 0xA1700001UL;
    constexpr uint32_t baseNonce = 0x11223344UL;
    constexpr uint32_t roverNonce = 0x55667788UL;

    TEST_ASSERT_EQUAL_UINT32(4, sizeof(EspNowCommonHeader));
    TEST_ASSERT_EQUAL_UINT32(28, sizeof(PairDiscoveryPacket));
    TEST_ASSERT_EQUAL_UINT32(28, sizeof(PairResponsePacket));
    TEST_ASSERT_EQUAL_UINT32(24, sizeof(PairConfirmPacket));

    PairDiscoveryPacket discovery{};
    discovery.common.magic = MAGIC;
    discovery.common.version = VERSION;
    discovery.common.packetType = PACKET_TYPE_PAIR_DISCOVERY;
    discovery.role = ROLE_BASE;
    discovery.networkId = networkId;
    discovery.baseDeviceId = 0x01020304UL;
    discovery.baseNonce = baseNonce;
    discovery.pairingWindowMs = 60000;
    discovery.authTag = pairingAuthTag(discovery, key.data(), key.size());
    TEST_ASSERT_TRUE(validatePairDiscovery(discovery, sizeof(discovery), networkId, key.data(), key.size()));
    TEST_ASSERT_FALSE(validatePairDiscovery(discovery, sizeof(discovery), networkId + 1, key.data(), key.size()));

    PairResponsePacket response{};
    response.common.magic = MAGIC;
    response.common.version = VERSION;
    response.common.packetType = PACKET_TYPE_PAIR_RESPONSE;
    response.role = ROLE_ROVER;
    response.networkId = networkId;
    response.roverDeviceId = 0xAABBCCDDUL;
    response.roverNonce = roverNonce;
    response.baseNonceEcho = baseNonce;
    response.authTag = pairingAuthTag(response, key.data(), key.size());
    TEST_ASSERT_TRUE(validatePairResponse(response, sizeof(response), networkId, baseNonce, key.data(), key.size()));
    TEST_ASSERT_FALSE(validatePairResponse(response, sizeof(response), networkId, baseNonce + 1, key.data(), key.size()));

    PairConfirmPacket confirm{};
    confirm.common.magic = MAGIC;
    confirm.common.version = VERSION;
    confirm.common.packetType = PACKET_TYPE_PAIR_CONFIRM;
    confirm.role = ROLE_BASE;
    confirm.networkId = networkId;
    confirm.baseNonce = baseNonce;
    confirm.roverNonce = roverNonce;
    confirm.authTag = pairingAuthTag(confirm, key.data(), key.size());
    TEST_ASSERT_TRUE(validatePairConfirm(confirm, sizeof(confirm), networkId, baseNonce, roverNonce, key.data(), key.size()));
    confirm.roverNonce ^= 1U;
    TEST_ASSERT_FALSE(validatePairConfirm(confirm, sizeof(confirm), networkId, baseNonce, roverNonce, key.data(), key.size()));
}

void test_gnss_command_packet_validation() {
    const std::array<uint8_t, 16> key{{0x10, 0x21, 0x32, 0x43,
                                      0x54, 0x65, 0x76, 0x87,
                                      0x98, 0xA9, 0xBA, 0xCB,
                                      0xDC, 0xED, 0xFE, 0x0F}};
    constexpr uint32_t networkId = 0xA1700001UL;

    GnssCommandRequestPacket request{};
    request.common.magic = MAGIC;
    request.common.version = VERSION;
    request.common.packetType = PACKET_TYPE_GNSS_COMMAND_REQUEST;
    request.networkId = networkId;
    request.transactionId = 123;
    request.surveyDurationSeconds = 60;
    request.commandId = GNSS_COMMAND_SWITCH_TO_BASE_SURVEY_IN;
    request.targetPort = GNSS_PORT_COM2;
    request.authTag = pairingAuthTag(request, key.data(), key.size());

    TEST_ASSERT_EQUAL_UINT32(24, sizeof(GnssCommandRequestPacket));
    TEST_ASSERT_TRUE(validateGnssCommandRequest(
        request, sizeof(request), networkId, key.data(), key.size()));
    request.surveyDurationSeconds = GNSS_SURVEY_MIN_SECONDS - 1;
    request.authTag = pairingAuthTag(request, key.data(), key.size());
    TEST_ASSERT_FALSE(validateGnssCommandRequest(
        request, sizeof(request), networkId, key.data(), key.size()));
    request.surveyDurationSeconds = 60;
    request.authTag = pairingAuthTag(request, key.data(), key.size());
    request.transactionId ^= 1U;
    TEST_ASSERT_FALSE(validateGnssCommandRequest(
        request, sizeof(request), networkId, key.data(), key.size()));
    request.transactionId = 123;
    request.commandId = GNSS_COMMAND_SWITCH_TO_ROVER;
    request.surveyDurationSeconds = 0;
    request.authTag = pairingAuthTag(request, key.data(), key.size());
    TEST_ASSERT_TRUE(validateGnssCommandRequest(
        request, sizeof(request), networkId, key.data(), key.size()));
    request.surveyDurationSeconds = 60;
    request.authTag = pairingAuthTag(request, key.data(), key.size());
    TEST_ASSERT_FALSE(validateGnssCommandRequest(
        request, sizeof(request), networkId, key.data(), key.size()));

    GnssCommandResultPacket result{};
    result.common.magic = MAGIC;
    result.common.version = VERSION;
    result.common.packetType = PACKET_TYPE_GNSS_COMMAND_RESULT;
    result.networkId = networkId;
    result.transactionId = 123;
    result.commandId = GNSS_COMMAND_SWITCH_TO_BASE_SURVEY_IN;
    result.status = GNSS_COMMAND_STATUS_UART_SEQUENCE_WRITTEN;
    result.completedStep = 14;
    result.totalSteps = 14;
    result.authTag = pairingAuthTag(result, key.data(), key.size());

    TEST_ASSERT_EQUAL_UINT32(24, sizeof(GnssCommandResultPacket));
    TEST_ASSERT_TRUE(validateGnssCommandResult(
        result, sizeof(result), networkId, key.data(), key.size()));
    result.status = 0;
    result.authTag = pairingAuthTag(result, key.data(), key.size());
    TEST_ASSERT_FALSE(validateGnssCommandResult(
        result, sizeof(result), networkId, key.data(), key.size()));
    result.commandId = GNSS_COMMAND_SWITCH_TO_ROVER;
    result.status = GNSS_COMMAND_STATUS_UART_SEQUENCE_WRITTEN;
    result.completedStep = 4;
    result.totalSteps = 4;
    result.authTag = pairingAuthTag(result, key.data(), key.size());
    TEST_ASSERT_TRUE(validateGnssCommandResult(
        result, sizeof(result), networkId, key.data(), key.size()));
}

void test_rtcm_crc_validation() {
    std::array<uint8_t, 9> frame{{0xD3, 0x00, 0x03, 0x3E, 0xD0, 0x00, 0, 0, 0}};
    const uint32_t crc = crc24q(frame.data(), frame.size() - 3);
    frame[6] = static_cast<uint8_t>(crc >> 16);
    frame[7] = static_cast<uint8_t>(crc >> 8);
    frame[8] = static_cast<uint8_t>(crc);

    TEST_ASSERT_TRUE(validateRtcm3Frame(frame.data(), frame.size()));
    frame[4] ^= 0x01;
    TEST_ASSERT_FALSE(validateRtcm3Frame(frame.data(), frame.size()));
}

void runTests() {
    UNITY_BEGIN();
    RUN_TEST(test_header_and_fragment_boundaries);
    RUN_TEST(test_packet_header_validation);
    RUN_TEST(test_frame_ack_validation);
    RUN_TEST(test_rover_llh_status_validation);
    RUN_TEST(test_relayed_rover_llh_status_validation);
    RUN_TEST(test_pairing_packet_validation);
    RUN_TEST(test_gnss_command_packet_validation);
    RUN_TEST(test_rtcm_crc_validation);
    UNITY_END();
}

#ifdef ARDUINO
void setup() { runTests(); }
void loop() {}
#else
int main() {
    runTests();
    return 0;
}
#endif
