#include "helper.h"

extern String latestGGA;

gga_data_struct ggaData;
gga_data_struct targetGgaData;
ksxt_data_struct ksxtData;

// int roverReadCharFromRtk(String &nmeaBuffer)
// {
//     char c = Serial1.read();
//     nmeaBuffer += c;
//     if (c == '\n')
//     {
//         return 1;
//     }
//     if (c == NULL)
//     {
//         return 2;
//     }
//     return 0;
// }

int publishGGA(String &nmeaBuffer)
{
    nmeaBuffer.trim();

    // Bắt dòng tọa độ
    if (nmeaBuffer.startsWith("$GNGGA") || nmeaBuffer.startsWith("$GPGGA") || nmeaBuffer.startsWith("$KSXT"))
    {
        // Cập nhật tọa độ mới nhất để health check đánh giá dữ liệu GNSS.
        latestGGA = nmeaBuffer;

        // Đẩy lên MQTT
        String jsonPayload = "";
        if (nmeaBuffer.startsWith("$KSXT"))
        {
            bool parseOk = parseKSXT_toStruct(nmeaBuffer, ksxtData);
            publishRaw(nmeaBuffer, false);
            if (parseOk)
            {
                jsonPayload = parseKSXT_toJSON(ksxtData);
                #if PROGRAM_DEBUG
                Serial.println("[KSXT PARSE] Da parse duoc du lieu KSXT va chuyen sang JSON: ");
                Serial.println(jsonPayload);
                #endif
            }
            publishData(jsonPayload, false);
        }
        else if (nmeaBuffer.startsWith("$GNGGA"))
        {
            publishRaw(nmeaBuffer, true);
            bool parseOk = parseGGA_toStruct(nmeaBuffer, ggaData);
            if (parseOk)
            {
                jsonPayload = parseGGA_toJSON(ggaData);
                #if PROGRAM_DEBUG
                Serial.println("[GGA PARSE] Da parse duoc du lieu GGA va chuyen sang JSON: ");
                Serial.println(jsonPayload);
                #endif
            }
            publishData(jsonPayload, true);
        }
        nmeaBuffer = "";
        return 0;
    }
    // Bắt dòng phản hồi lệnh
    else if (nmeaBuffer.startsWith("#"))
    {
        Serial.print("[UM980 RESPONSE] ");
        Serial.println(nmeaBuffer);
        nmeaBuffer = "";
        return -1;
    }
    nmeaBuffer = "";
    return -1;
}

String formDeviceHealthString()
{
    // 1. Lấy các thông số hệ thống
    unsigned long uptime_s = millis() / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();

    int32_t rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
    String connected_via = "WiFi";

    bool mqttOk = isMqttConnected();
    bool gnssOk = (latestGGA.length() > 10); // Nếu có chuỗi NMEA hợp lệ
    const EspNowRtcmStats espnowStats = espnowGetStats();
    const uint32_t frameAgeMs = espnowStats.lastValidFrameMillis == 0
                                    ? UINT32_MAX
                                    : millis() - espnowStats.lastValidFrameMillis;

    // 2. Đóng gói thành JSON
    char healthPayload[512];
    snprintf(healthPayload, sizeof(healthPayload),
             "{\"uptime_s\":%lu,\"free_heap_bytes\":%u,\"connected_via\":\"%s\",\"rssi_dbm\":%d,\"mqtt_ok\":%s,\"espnow_ready\":%s,\"base_provisioned\":%s,\"gnss_data_ok\":%s,\"rtcm_frames\":%lu,\"rtcm_crc_errors\":%lu,\"rtcm_queue_overflow\":%lu,\"rtcm_sequence_gaps\":%lu,\"last_rtcm_age_ms\":%lu}",
             uptime_s, freeHeap, connected_via, rssi,
             mqttOk ? "true" : "false",
             espnowIsReady() ? "true" : "false",
             espnowBaseMacIsConfigured() ? "true" : "false",
             gnssOk ? "true" : "false",
             static_cast<unsigned long>(espnowStats.framesWritten),
             static_cast<unsigned long>(espnowStats.crcErrors),
             static_cast<unsigned long>(espnowStats.queueOverflow),
             static_cast<unsigned long>(espnowStats.sequenceGaps),
             static_cast<unsigned long>(frameAgeMs));
    /*Xóa tọa độ sau khi đã dùng để đánh giá sức khoẻ, nếu còn giữ, 
    trong trường hợp không có dữ liệu mới, sẽ luôn báo GNSS OK dù 
    thực tế đã mất tín hiệu. Việc này giúp phản ánh tình trạng thực tế hơn.*/ 
    latestGGA = "";
    // 3. Trả về payload để có thể log hoặc dùng cho mục đích khác nếu cần
    return String(healthPayload);
}
