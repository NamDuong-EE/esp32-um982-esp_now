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
