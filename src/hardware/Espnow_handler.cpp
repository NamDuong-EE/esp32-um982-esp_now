#include "hardware/Espnow_handler.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstddef>
#include <cstring>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#include "Prog_Config.h"
#include "hardware/Wifi_handler.h"

namespace {

QueueHandle_t receiveQueue = nullptr;
bool ready = false;
EspNowRtcmStats stats{};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
constexpr std::size_t IEEE80211_ADDR2_OFFSET = 10;
constexpr std::size_t IEEE80211_MIN_ADDR2_LENGTH = IEEE80211_ADDR2_OFFSET + 6;

void updateCounter(uint32_t EspNowRtcmStats::*member, uint32_t increment = 1) {
    portENTER_CRITICAL(&statsMux);
    stats.*member += increment;
    portEXIT_CRITICAL(&statsMux);
}

void recordQueueDepth() {
    const uint32_t depth = receiveQueue == nullptr
                               ? 0
                               : static_cast<uint32_t>(uxQueueMessagesWaiting(receiveQueue));
    portENTER_CRITICAL(&statsMux);
    if (depth > stats.queueHighWater) {
        stats.queueHighWater = depth;
    }
    portEXIT_CRITICAL(&statsMux);
}

bool isExpectedBase(const uint8_t* mac) {
    return mac != nullptr && std::memcmp(mac, ESPNOW_BASE_MAC, 6) == 0;
}

void recordRssi(int8_t rssi) {
    portENTER_CRITICAL(&statsMux);
    stats.hasRssi = true;
    stats.lastRssiDbm = rssi;
    portEXIT_CRITICAL(&statsMux);
}

void onPromiscuousPacket(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if (buffer == nullptr || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) {
        return;
    }

    const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    if (packet->rx_ctrl.rx_state != 0 ||
        packet->rx_ctrl.sig_len < IEEE80211_MIN_ADDR2_LENGTH) {
        return;
    }

    const uint8_t* sourceMac = packet->payload + IEEE80211_ADDR2_OFFSET;
    if (isExpectedBase(sourceMac)) {
        recordRssi(static_cast<int8_t>(packet->rx_ctrl.rssi));
    }
}

void handleReceivedPacket(const uint8_t* sourceMac, const uint8_t* data, int length) {
    if (!isExpectedBase(sourceMac)) {
        updateCounter(&EspNowRtcmStats::packetsWrongSource);
        return;
    }
    if (data == nullptr || length < static_cast<int>(sizeof(rtcm_espnow::RtcmEspNowHeader)) ||
        length > static_cast<int>(rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE)) {
        updateCounter(&EspNowRtcmStats::packetsInvalidHeader);
        return;
    }

    rtcm_espnow::RtcmEspNowHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (!rtcm_espnow::validatePacketHeader(header, static_cast<std::size_t>(length))) {
        updateCounter(&EspNowRtcmStats::packetsInvalidHeader);
        return;
    }

    EspNowRxPacket packet{};
    packet.length = static_cast<uint16_t>(length);
    std::memcpy(packet.data, data, static_cast<std::size_t>(length));
    if (receiveQueue == nullptr || xQueueSend(receiveQueue, &packet, 0) != pdTRUE) {
        updateCounter(&EspNowRtcmStats::queueOverflow);
        return;
    }
    updateCounter(&EspNowRtcmStats::packetsReceived);
    recordQueueDepth();
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataReceived(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
    handleReceivedPacket(info == nullptr ? nullptr : info->src_addr, data, length);
}
#else
void onDataReceived(const uint8_t* sourceMac, const uint8_t* data, int length) {
    handleReceivedPacket(sourceMac, data, length);
}
#endif

bool configurePeer(uint8_t channel) {
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, ESPNOW_BASE_MAC, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = ESPNOW_ENCRYPTION_ENABLED;
    if (peer.encrypt) {
        std::memcpy(peer.lmk, ESPNOW_LMK, sizeof(peer.lmk));
    }

    esp_err_t result = esp_now_is_peer_exist(ESPNOW_BASE_MAC)
                           ? esp_now_mod_peer(&peer)
                           : esp_now_add_peer(&peer);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong cau hinh duoc Base peer: %d\n", result);
        return false;
    }

    const wifi_phy_rate_t rate = ESPNOW_USE_LR_250KBPS
                                     ? WIFI_PHY_RATE_LORA_250K
                                     : WIFI_PHY_RATE_LORA_500K;
    result = esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong dat duoc LR PHY rate: %d\n", result);
        return false;
    }
    return true;
}

void setupRssiMonitor() {
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;

    esp_err_t result = esp_wifi_set_promiscuous_filter(&filter);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][WARN] Khong dat duoc RSSI promiscuous filter: %d\n", result);
        return;
    }

    result = esp_wifi_set_promiscuous_rx_cb(onPromiscuousPacket);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][WARN] Khong dang ky duoc RSSI callback: %d\n", result);
        return;
    }

    result = esp_wifi_set_promiscuous(true);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][WARN] Khong bat duoc RSSI monitor: %d\n", result);
        return;
    }

    Serial.println("[ESP-NOW] RSSI monitor enabled for Base MAC");
}

} // namespace

bool espnowSetup() {
    if (ready) {
        return true;
    }
    if constexpr (WIFI_CONNECT_TO_ROUTER_ENABLED) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[ESP-NOW][ERROR] Wi-Fi STA chua ket noi router/AP");
            return false;
        }
        if (!configureWiFiForEspNowLongRange()) {
            return false;
        }
    } else if (!setupEspNowStaRadio()) {
        return false;
    }

    if (!espnowBaseMacIsConfigured()) {
        Serial.println("[ESP-NOW][ERROR] ESPNOW_BASE_MAC chua duoc provision trong Prog_Config.h");
        return false;
    }
    if (ESPNOW_ENCRYPTION_ENABLED && !espnowSecurityKeysAreConfigured()) {
        Serial.println("[ESP-NOW][ERROR] Ma hoa da bat nhung PMK/LMK chua duoc provision");
        return false;
    }

    receiveQueue = xQueueCreate(ESPNOW_QUEUE_LENGTH, sizeof(EspNowRxPacket));
    if (receiveQueue == nullptr) {
        Serial.println("[ESP-NOW][ERROR] Khong tao duoc receive queue");
        return false;
    }

    esp_err_t result = esp_now_init();
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] esp_now_init: %d\n", result);
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if (ESPNOW_ENCRYPTION_ENABLED) {
        result = esp_now_set_pmk(ESPNOW_PMK);
        if (result != ESP_OK) {
            Serial.printf("[ESP-NOW][ERROR] esp_now_set_pmk: %d\n", result);
            esp_now_deinit();
            vQueueDelete(receiveQueue);
            receiveQueue = nullptr;
            return false;
        }
    }

    result = esp_now_register_recv_cb(onDataReceived);
    if (result != ESP_OK || !configurePeer(getWiFiChannel())) {
        Serial.printf("[ESP-NOW][ERROR] Khong dang ky duoc receiver: %d\n", result);
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if constexpr (DEBUG_WEB_ENABLED) {
        setupRssiMonitor();
    }

    ready = true;
    Serial.printf("[ESP-NOW] Ready, STA channel=%u, LR=%u Kbps\n",
                  getWiFiChannel(), ESPNOW_USE_LR_250KBPS ? 250U : 500U);
    return true;
}

bool espnowRefreshPeerChannel() {
    if (!ready) {
        return false;
    }
    return configureWiFiForEspNowLongRange() && configurePeer(getWiFiChannel());
}

bool espnowIsReady() {
    return ready;
}

QueueHandle_t espnowGetReceiveQueue() {
    return receiveQueue;
}

EspNowRtcmStats espnowGetStats() {
    portENTER_CRITICAL(&statsMux);
    EspNowRtcmStats copy = stats;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}

bool espnowSendFrameAck(uint16_t streamId, uint32_t frameSequence) {
    if (!ready) {
        updateCounter(&EspNowRtcmStats::ackSendFailures);
        return false;
    }

    rtcm_espnow::RtcmEspNowAck ack{};
    ack.magic = rtcm_espnow::MAGIC;
    ack.version = rtcm_espnow::VERSION;
    ack.packetType = rtcm_espnow::PACKET_TYPE_FRAME_ACK;
    ack.streamId = streamId;
    ack.frameSequence = frameSequence;
    ack.status = rtcm_espnow::ACK_STATUS_WRITTEN;

    const esp_err_t result = esp_now_send(
        ESPNOW_BASE_MAC,
        reinterpret_cast<const uint8_t*>(&ack),
        sizeof(ack));
    if (result != ESP_OK) {
        updateCounter(&EspNowRtcmStats::ackSendFailures);
        return false;
    }
    updateCounter(&EspNowRtcmStats::ackPacketsQueued);
    return true;
}

void espnowRecordInvalidHeader() { updateCounter(&EspNowRtcmStats::packetsInvalidHeader); }
void espnowRecordDuplicateFragment() { updateCounter(&EspNowRtcmStats::duplicateFragments); }
void espnowRecordFrameTimeout() { updateCounter(&EspNowRtcmStats::frameTimeouts); }
void espnowRecordSequenceGaps(uint32_t count) { updateCounter(&EspNowRtcmStats::sequenceGaps, count); }
void espnowRecordCrcError() { updateCounter(&EspNowRtcmStats::crcErrors); }
void espnowRecordUartWriteError() { updateCounter(&EspNowRtcmStats::uartWriteErrors); }

void espnowRecordFrameWritten() {
    portENTER_CRITICAL(&statsMux);
    ++stats.framesWritten;
    stats.lastValidFrameMillis = millis();
    portEXIT_CRITICAL(&statsMux);
}
