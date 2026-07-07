#include "helper.h"

extern String latestGGA;

gga_data_struct ggaData;
gga_data_struct targetGgaData;
ksxt_data_struct ksxtData;
GgaDebugSnapshot ggaDebugSnapshot{};

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
        if (xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE)
        {
            latestGGA = nmeaBuffer;
            xSemaphoreGive(nmeaBufferMutex);
        }

        // Parse dữ liệu GNSS; MQTT uplink sẽ tự bỏ qua khi ROVER_MQTT_ENABLED=false.
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
        else if (nmeaBuffer.startsWith("$GNGGA") || nmeaBuffer.startsWith("$GPGGA"))
        {
            publishRaw(nmeaBuffer, true);
            bool parseOk = parseGGA_toStruct(nmeaBuffer, ggaData);
            if (parseOk)
            {
                if (xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE)
                {
                    ggaDebugSnapshot.valid = true;
                    ggaDebugSnapshot.lat = ggaData.lat;
                    ggaDebugSnapshot.lon = ggaData.lon;
                    ggaDebugSnapshot.fixQuality = static_cast<uint8_t>(ggaData.rtk_status.toInt());
                    ggaDebugSnapshot.satellites = static_cast<uint8_t>(ggaData.satellites.toInt());
                    ggaDebugSnapshot.lastUpdateMs = millis();
                    xSemaphoreGive(nmeaBufferMutex);
                }
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

GgaDebugSnapshot getGgaDebugSnapshot()
{
    GgaDebugSnapshot snapshot{};
    if (nmeaBufferMutex != nullptr &&
        xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        snapshot = ggaDebugSnapshot;
        xSemaphoreGive(nmeaBufferMutex);
    }
    return snapshot;
}

String formDeviceHealthString()
{
    // 1. Lấy các thông số hệ thống
    unsigned long uptime_s = millis() / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();

    int32_t rssi = WIFI_CONNECT_TO_ROUTER_ENABLED && WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
    String connected_via = WIFI_CONNECT_TO_ROUTER_ENABLED ? "WiFi" : "ESP-NOW_STA";

    bool mqttOk = isMqttConnected();
    bool gnssOk = false;
    if (xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        gnssOk = latestGGA.length() > 10;
        latestGGA = "";
        xSemaphoreGive(nmeaBufferMutex);
    }
    const EspNowRtcmStats espnowStats = espnowGetStats();
    const uint32_t frameAgeMs = espnowStats.lastValidFrameMillis == 0
                                    ? UINT32_MAX
                                    : millis() - espnowStats.lastValidFrameMillis;
    const bool hasEspNowRssi = espnowStats.hasRssi;
    const String espnowRssiValue = hasEspNowRssi ? String(espnowStats.lastRssiDbm) : "null";

    // 2. Đóng gói thành JSON
    char healthPayload[1024];
    snprintf(healthPayload, sizeof(healthPayload),
             "{\"uptime_s\":%lu,\"free_heap_bytes\":%u,\"connected_via\":\"%s\",\"rssi_dbm\":%d,\"mqtt_ok\":%s,\"espnow_ready\":%s,\"base_provisioned\":%s,\"espnow_rssi_dbm\":%s,\"gnss_data_ok\":%s,\"packets_received\":%lu,\"packets_wrong_source\":%lu,\"packets_invalid\":%lu,\"rtcm_frames\":%lu,\"rtcm_crc_errors\":%lu,\"rtcm_queue_overflow\":%lu,\"rtcm_queue_hwm\":%lu,\"rtcm_duplicates\":%lu,\"rtcm_timeouts\":%lu,\"rtcm_sequence_gaps\":%lu,\"uart_write_errors\":%lu,\"ack_queued\":%lu,\"ack_send_fail\":%lu,\"last_rtcm_age_ms\":%lu}",
             uptime_s, freeHeap, connected_via.c_str(), rssi,
             mqttOk ? "true" : "false",
             espnowIsReady() ? "true" : "false",
             espnowBaseMacIsConfigured() ? "true" : "false",
             espnowRssiValue.c_str(),
             gnssOk ? "true" : "false",
             static_cast<unsigned long>(espnowStats.packetsReceived),
             static_cast<unsigned long>(espnowStats.packetsWrongSource),
             static_cast<unsigned long>(espnowStats.packetsInvalidHeader),
             static_cast<unsigned long>(espnowStats.framesWritten),
             static_cast<unsigned long>(espnowStats.crcErrors),
             static_cast<unsigned long>(espnowStats.queueOverflow),
             static_cast<unsigned long>(espnowStats.queueHighWater),
             static_cast<unsigned long>(espnowStats.duplicateFragments),
             static_cast<unsigned long>(espnowStats.frameTimeouts),
             static_cast<unsigned long>(espnowStats.sequenceGaps),
             static_cast<unsigned long>(espnowStats.uartWriteErrors),
             static_cast<unsigned long>(espnowStats.ackPacketsQueued),
             static_cast<unsigned long>(espnowStats.ackSendFailures),
             static_cast<unsigned long>(frameAgeMs));
    // 3. Trả về payload để có thể log hoặc dùng cho mục đích khác nếu cần
    return String(healthPayload);
}
