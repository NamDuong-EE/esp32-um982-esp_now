#include "hardware/Relay_handler.h"

#include <Preferences.h>
#include <WiFi.h>
#include <cstring>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#include "Prog_Config.h"
#include "hardware/Espnow_handler.h"
#include "hardware/Wifi_handler.h"
#include "protocol/RtcmEspNowProtocol.h"

namespace {

struct RelayFrame {
    uint16_t frameLength;
    uint16_t streamId;
    uint32_t frameSequence;
    uint8_t frame[rtcm_espnow::MAX_RTCM_FRAME_SIZE];
};

struct RelayAckEvent {
    uint16_t streamId;
    uint32_t frameSequence;
};

constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

QueueHandle_t relayFrameQueue = nullptr;
QueueHandle_t relayAckQueue = nullptr;
SemaphoreHandle_t sendCallbackSemaphore = nullptr;
SemaphoreHandle_t relaySendMutex = nullptr;
bool relayReady = false;
uint8_t childMac[6] = {};
bool hasChildMac = false;
RelayStats stats{};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;

bool childPairingActive = false;
bool pendingPairResponse = false;
uint32_t childPairingEndsAtMs = 0;
uint32_t childPairingBaseNonce = 0;
uint32_t lastDiscoverySentAtMs = 0;
bool discoveryTxLogged = false;
uint8_t pendingPairResponseMac[6] = {};
rtcm_espnow::PairResponsePacket pendingPairResponsePacket{};

bool waitingForAck = false;
uint16_t expectedAckStreamId = 0;
uint32_t expectedAckSequence = 0;
bool waitingForSendCallback = false;
bool lastSendSucceeded = false;
uint8_t expectedSendMac[6] = {};

void incrementStat(uint32_t RelayStats::*member, uint32_t amount = 1) {
    portENTER_CRITICAL(&statsMux);
    stats.*member += amount;
    portEXIT_CRITICAL(&statsMux);
}

void setChildPairingStat(bool active) {
    portENTER_CRITICAL(&statsMux);
    stats.childPairingActive = active;
    portEXIT_CRITICAL(&statsMux);
}

void setStoredChildStat(bool stored) {
    portENTER_CRITICAL(&statsMux);
    stats.hasStoredChildMac = stored;
    portEXIT_CRITICAL(&statsMux);
}

String macToString(const uint8_t* mac) {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buffer);
}

bool macIsConfigured(const uint8_t* mac) {
    return mac != nullptr &&
           (mac[0] != 0 || mac[1] != 0 || mac[2] != 0 ||
            mac[3] != 0 || mac[4] != 0 || mac[5] != 0);
}

bool isChild(const uint8_t* mac) {
    return hasChildMac && mac != nullptr && std::memcmp(mac, childMac, 6) == 0;
}

uint32_t localDeviceId() {
    uint8_t mac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return 0;
    }
    return (static_cast<uint32_t>(mac[2]) << 24) |
           (static_cast<uint32_t>(mac[3]) << 16) |
           (static_cast<uint32_t>(mac[4]) << 8) |
           static_cast<uint32_t>(mac[5]);
}

bool addPeer(const uint8_t* mac, bool encrypted) {
    if (!macIsConfigured(mac)) {
        return false;
    }
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, 6);
    peer.channel = getWiFiChannel();
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypted;
    if (encrypted) {
        std::memcpy(peer.lmk, ESPNOW_LMK, sizeof(peer.lmk));
    }
    const esp_err_t result = esp_now_is_peer_exist(mac)
                                 ? esp_now_mod_peer(&peer)
                                 : esp_now_add_peer(&peer);
    if (result != ESP_OK) {
        Serial.printf("[RELAY][ERROR] Khong cau hinh duoc peer %s: %d\n",
                      macToString(mac).c_str(), result);
        return false;
    }
    return true;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
    const uint8_t* destinationMac = info == nullptr ? nullptr : info->des_addr;
#else
void onDataSent(const uint8_t* destinationMac, esp_now_send_status_t status) {
#endif
    bool matches = false;
    portENTER_CRITICAL(&relayMux);
    matches = waitingForSendCallback && destinationMac != nullptr &&
              std::memcmp(destinationMac, expectedSendMac, 6) == 0;
    if (matches) {
        lastSendSucceeded = status == ESP_NOW_SEND_SUCCESS;
    }
    portEXIT_CRITICAL(&relayMux);
    if (matches && sendCallbackSemaphore != nullptr) {
        xSemaphoreGive(sendCallbackSemaphore);
    }
}

bool sendPacketSync(const uint8_t* destinationMac,
                    const uint8_t* packet,
                    std::size_t packetLength) {
    if (destinationMac == nullptr || packet == nullptr || packetLength == 0 ||
        sendCallbackSemaphore == nullptr || relaySendMutex == nullptr ||
        xSemaphoreTake(relaySendMutex,
                       pdMS_TO_TICKS(RELAY_SEND_CALLBACK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    while (xSemaphoreTake(sendCallbackSemaphore, 0) == pdTRUE) {
    }
    portENTER_CRITICAL(&relayMux);
    std::memcpy(expectedSendMac, destinationMac, 6);
    lastSendSucceeded = false;
    waitingForSendCallback = true;
    portEXIT_CRITICAL(&relayMux);

    const esp_err_t queued = esp_now_send(destinationMac, packet, packetLength);
    BaseType_t callbackReceived = pdFALSE;
    if (queued == ESP_OK) {
        callbackReceived = xSemaphoreTake(
            sendCallbackSemaphore,
            pdMS_TO_TICKS(RELAY_SEND_CALLBACK_TIMEOUT_MS));
    }

    portENTER_CRITICAL(&relayMux);
    const bool succeeded = callbackReceived == pdTRUE && lastSendSucceeded;
    waitingForSendCallback = false;
    portEXIT_CRITICAL(&relayMux);
    xSemaphoreGive(relaySendMutex);
    return succeeded;
}

bool loadStoredChildMac(uint8_t mac[6]) {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, true)) {
        return false;
    }
    const bool ok = preferences.getBytesLength(ESPNOW_NVS_CHILD_MAC_KEY) == 6 &&
                    preferences.getBytes(ESPNOW_NVS_CHILD_MAC_KEY, mac, 6) == 6 &&
                    macIsConfigured(mac);
    preferences.end();
    return ok;
}

bool saveStoredChildMac(const uint8_t mac[6]) {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, false)) {
        return false;
    }
    const bool ok = preferences.putBytes(ESPNOW_NVS_CHILD_MAC_KEY, mac, 6) == 6;
    preferences.end();
    return ok;
}

void updateQueueHighWater() {
    const uint32_t depth = relayFrameQueue == nullptr
                               ? 0
                               : static_cast<uint32_t>(uxQueueMessagesWaiting(relayFrameQueue));
    portENTER_CRITICAL(&statsMux);
    stats.queueDepth = depth;
    if (depth > stats.queueHighWater) {
        stats.queueHighWater = depth;
    }
    portEXIT_CRITICAL(&statsMux);
}

void startChildPairing() {
    uint8_t baseMac[6] = {};
    if (!relayReady || !espnowGetBaseMac(baseMac)) {
        return;
    }
    if (!addPeer(BROADCAST_MAC, false)) {
        return;
    }
    const uint32_t now = millis();
    portENTER_CRITICAL(&relayMux);
    childPairingActive = true;
    pendingPairResponse = false;
    childPairingEndsAtMs = now + PAIRING_WINDOW_MS;
    childPairingBaseNonce = esp_random();
    lastDiscoverySentAtMs = 0;
    discoveryTxLogged = false;
    portEXIT_CRITICAL(&relayMux);
    setChildPairingStat(true);
    Serial.printf("[RELAY][PAIR] Child pairing mode ON for %lu ms\n",
                  static_cast<unsigned long>(PAIRING_WINDOW_MS));
}

void stopChildPairing(const char* reason) {
    portENTER_CRITICAL(&relayMux);
    childPairingActive = false;
    pendingPairResponse = false;
    portEXIT_CRITICAL(&relayMux);
    setChildPairingStat(false);
    Serial.printf("[RELAY][PAIR] Child pairing mode OFF: %s\n", reason);
}

void sendPairDiscovery() {
    rtcm_espnow::PairDiscoveryPacket discovery{};
    discovery.common.magic = rtcm_espnow::MAGIC;
    discovery.common.version = rtcm_espnow::VERSION;
    discovery.common.packetType = rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY;
    discovery.role = rtcm_espnow::ROLE_BASE;
    discovery.networkId = ESPNOW_NETWORK_ID;
    discovery.baseDeviceId = localDeviceId();
    discovery.baseNonce = childPairingBaseNonce;
    discovery.pairingWindowMs = PAIRING_WINDOW_MS;
    discovery.authTag = rtcm_espnow::pairingAuthTag(discovery,
                                                    ESPNOW_PAIRING_KEY,
                                                    sizeof(ESPNOW_PAIRING_KEY));
    if (sendPacketSync(BROADCAST_MAC,
                       reinterpret_cast<const uint8_t*>(&discovery),
                       sizeof(discovery))) {
        incrementStat(&RelayStats::childPairDiscoveriesSent);
        if (!discoveryTxLogged) {
            discoveryTxLogged = true;
            Serial.println("[RELAY][PAIR] PAIR_DISCOVERY radio TX confirmed");
        }
    } else {
        incrementStat(&RelayStats::sendFailures);
        Serial.println("[RELAY][PAIR][WARN] PAIR_DISCOVERY send callback failed");
    }
}

void processPairResponse() {
    rtcm_espnow::PairResponsePacket response{};
    uint8_t sourceMac[6] = {};
    bool pending = false;
    uint32_t expectedNonce = 0;
    portENTER_CRITICAL(&relayMux);
    pending = pendingPairResponse;
    if (pending) {
        response = pendingPairResponsePacket;
        std::memcpy(sourceMac, pendingPairResponseMac, 6);
        pendingPairResponse = false;
    }
    expectedNonce = childPairingBaseNonce;
    portEXIT_CRITICAL(&relayMux);
    if (!pending) {
        return;
    }

    if (!rtcm_espnow::validatePairResponse(response,
                                           sizeof(response),
                                           ESPNOW_NETWORK_ID,
                                           expectedNonce,
                                           ESPNOW_PAIRING_KEY,
                                           sizeof(ESPNOW_PAIRING_KEY))) {
        incrementStat(&RelayStats::childPairAuthFailures);
        return;
    }
    incrementStat(&RelayStats::childPairResponsesReceived);
    if (!addPeer(sourceMac, false)) {
        return;
    }

    rtcm_espnow::PairConfirmPacket confirm{};
    confirm.common.magic = rtcm_espnow::MAGIC;
    confirm.common.version = rtcm_espnow::VERSION;
    confirm.common.packetType = rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM;
    confirm.role = rtcm_espnow::ROLE_BASE;
    confirm.networkId = ESPNOW_NETWORK_ID;
    confirm.baseNonce = expectedNonce;
    confirm.roverNonce = response.roverNonce;
    confirm.authTag = rtcm_espnow::pairingAuthTag(confirm,
                                                  ESPNOW_PAIRING_KEY,
                                                  sizeof(ESPNOW_PAIRING_KEY));
    if (!sendPacketSync(sourceMac,
                        reinterpret_cast<const uint8_t*>(&confirm),
                        sizeof(confirm))) {
        incrementStat(&RelayStats::sendFailures);
        Serial.println("[RELAY][PAIR][ERROR] PAIR_CONFIRM send callback failed");
        return;
    }
    incrementStat(&RelayStats::childPairConfirmsSent);
    if (!saveStoredChildMac(sourceMac)) {
        Serial.println("[RELAY][PAIR][ERROR] Khong luu duoc MAC child vao NVS");
        return;
    }
    std::memcpy(childMac, sourceMac, 6);
    hasChildMac = true;
    setStoredChildStat(true);
    stopChildPairing("paired");
    if (!addPeer(childMac, ESPNOW_ENCRYPTION_ENABLED)) {
        Serial.println("[RELAY][PAIR][ERROR] Child da luu nhung runtime peer chua san sang");
        return;
    }
    Serial.println("[RELAY][PAIR] Paired child " + macToString(childMac));
}

bool sendFragment(const uint8_t* child,
                  const uint8_t* packet,
                  std::size_t packetLength) {
    for (uint8_t attempt = 0; attempt <= RELAY_FRAGMENT_SEND_RETRY_COUNT; ++attempt) {
        if (sendPacketSync(child, packet, packetLength)) {
            incrementStat(&RelayStats::fragmentsSent);
            vTaskDelay(pdMS_TO_TICKS(RELAY_FRAGMENT_GAP_MS));
            return true;
        }
        incrementStat(&RelayStats::sendFailures);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool sendFrame(const RelayFrame& relayFrame) {
    uint8_t packet[rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE] = {};
    const uint8_t fragmentCount = rtcm_espnow::expectedFragmentCount(relayFrame.frameLength);

    for (uint8_t attempt = 0; attempt <= RELAY_FRAME_RETRY_COUNT; ++attempt) {
        RelayAckEvent staleAck{};
        while (xQueueReceive(relayAckQueue, &staleAck, 0) == pdTRUE) {
        }
        portENTER_CRITICAL(&relayMux);
        waitingForAck = true;
        expectedAckStreamId = relayFrame.streamId;
        expectedAckSequence = relayFrame.frameSequence;
        portEXIT_CRITICAL(&relayMux);

        bool allFragmentsSent = true;
        for (uint8_t index = 0; index < fragmentCount; ++index) {
            const uint16_t payloadLength =
                rtcm_espnow::expectedFragmentPayloadLength(relayFrame.frameLength, index);
            const std::size_t offset =
                static_cast<std::size_t>(index) * rtcm_espnow::MAX_FRAGMENT_PAYLOAD_SIZE;
            rtcm_espnow::RtcmEspNowHeader header{};
            header.magic = rtcm_espnow::MAGIC;
            header.version = rtcm_espnow::VERSION;
            header.packetType = rtcm_espnow::PACKET_TYPE_RTCM_DATA;
            header.streamId = relayFrame.streamId;
            header.frameSequence = relayFrame.frameSequence;
            header.frameLength = relayFrame.frameLength;
            header.fragmentIndex = index;
            header.fragmentCount = fragmentCount;
            header.payloadLength = payloadLength;
            std::memcpy(packet, &header, sizeof(header));
            std::memcpy(packet + sizeof(header), relayFrame.frame + offset, payloadLength);
            if (!sendFragment(childMac, packet, sizeof(header) + payloadLength)) {
                allFragmentsSent = false;
                break;
            }
        }

        if (allFragmentsSent) {
            incrementStat(&RelayStats::framesSent);
            RelayAckEvent ack{};
            if (xQueueReceive(relayAckQueue, &ack,
                              pdMS_TO_TICKS(RELAY_ACK_TIMEOUT_MS)) == pdTRUE) {
                portENTER_CRITICAL(&relayMux);
                waitingForAck = false;
                portEXIT_CRITICAL(&relayMux);
                portENTER_CRITICAL(&statsMux);
                ++stats.framesAcked;
                stats.lastAckMillis = millis();
                portEXIT_CRITICAL(&statsMux);
                return true;
            }
            incrementStat(&RelayStats::ackTimeouts);
        }
        if (attempt < RELAY_FRAME_RETRY_COUNT) {
            incrementStat(&RelayStats::frameRetries);
        }
    }

    portENTER_CRITICAL(&relayMux);
    waitingForAck = false;
    portEXIT_CRITICAL(&relayMux);
    return false;
}

} // namespace

bool relaySetup() {
    if (!ROVER_RELAY_MODE || relayReady) {
        return relayReady;
    }
    if (!espnowIsReady()) {
        return false;
    }
    relayFrameQueue = xQueueCreate(RELAY_QUEUE_LENGTH, sizeof(RelayFrame));
    relayAckQueue = xQueueCreate(4, sizeof(RelayAckEvent));
    sendCallbackSemaphore = xSemaphoreCreateBinary();
    relaySendMutex = xSemaphoreCreateMutex();
    if (relayFrameQueue == nullptr || relayAckQueue == nullptr ||
        sendCallbackSemaphore == nullptr || relaySendMutex == nullptr) {
        Serial.println("[RELAY][ERROR] Khong tao duoc relay queue/semaphore");
        if (relayFrameQueue != nullptr) {
            vQueueDelete(relayFrameQueue);
            relayFrameQueue = nullptr;
        }
        if (relayAckQueue != nullptr) {
            vQueueDelete(relayAckQueue);
            relayAckQueue = nullptr;
        }
        if (sendCallbackSemaphore != nullptr) {
            vSemaphoreDelete(sendCallbackSemaphore);
            sendCallbackSemaphore = nullptr;
        }
        if (relaySendMutex != nullptr) {
            vSemaphoreDelete(relaySendMutex);
            relaySendMutex = nullptr;
        }
        return false;
    }

    const esp_err_t callbackResult = esp_now_register_send_cb(onDataSent);
    if (callbackResult != ESP_OK) {
        Serial.printf("[RELAY][ERROR] Khong dang ky duoc send callback: %d\n",
                      callbackResult);
        return false;
    }

    uint8_t storedMac[6] = {};
    if (loadStoredChildMac(storedMac) && addPeer(storedMac, ESPNOW_ENCRYPTION_ENABLED)) {
        std::memcpy(childMac, storedMac, 6);
        hasChildMac = true;
        setStoredChildStat(true);
        Serial.println("[RELAY] Loaded child MAC from NVS: " + macToString(childMac));
    } else {
        Serial.println("[RELAY] No stored child MAC; hold pairing button after Base pairing");
    }
    relayReady = true;
    Serial.println("[RELAY] Downstream relay ready");
    return true;
}

bool relayIsReady() {
    return relayReady;
}

void relayLoop() {
    if (!ROVER_RELAY_MODE || !relayReady) {
        return;
    }

    static uint32_t pressedSinceMs = 0;
    static bool pairingStartHandled = false;
    uint8_t baseMac[6] = {};
    const bool hasBase = espnowGetBaseMac(baseMac);
    const bool rawLevel = digitalRead(PAIRING_BUTTON_PIN) == HIGH;
    const bool pressed = PAIRING_BUTTON_ACTIVE_LOW ? !rawLevel : rawLevel;
    const uint32_t now = millis();

    if (hasBase && pressed) {
        if (pressedSinceMs == 0) {
            pressedSinceMs = now;
        } else if (!pairingStartHandled && now - pressedSinceMs >= PAIRING_BUTTON_HOLD_MS) {
            startChildPairing();
            pairingStartHandled = true;
        }
    } else if (!pressed) {
        pressedSinceMs = 0;
        pairingStartHandled = false;
    }

    bool active = false;
    uint32_t endsAt = 0;
    uint32_t lastSent = 0;
    portENTER_CRITICAL(&relayMux);
    active = childPairingActive;
    endsAt = childPairingEndsAtMs;
    lastSent = lastDiscoverySentAtMs;
    portEXIT_CRITICAL(&relayMux);

    if (active && static_cast<int32_t>(now - endsAt) >= 0) {
        stopChildPairing("timeout");
        return;
    }
    if (active && (lastSent == 0 || now - lastSent >= RELAY_DISCOVERY_INTERVAL_MS)) {
        sendPairDiscovery();
        portENTER_CRITICAL(&relayMux);
        lastDiscoverySentAtMs = now;
        portEXIT_CRITICAL(&relayMux);
    }
    processPairResponse();
}

bool relayQueueFrame(const uint8_t* frame,
                     uint16_t frameLength,
                     uint16_t streamId,
                     uint32_t frameSequence) {
    if (!ROVER_RELAY_MODE || !relayReady || frame == nullptr ||
        frameLength > rtcm_espnow::MAX_RTCM_FRAME_SIZE) {
        return false;
    }
    if (!hasChildMac) {
        incrementStat(&RelayStats::framesWithoutChild);
        return false;
    }
    RelayFrame relayFrame{};
    relayFrame.frameLength = frameLength;
    relayFrame.streamId = streamId;
    relayFrame.frameSequence = frameSequence;
    std::memcpy(relayFrame.frame, frame, frameLength);
    if (xQueueSend(relayFrameQueue, &relayFrame, pdMS_TO_TICKS(50)) != pdTRUE) {
        incrementStat(&RelayStats::queueOverflow);
        return false;
    }
    incrementStat(&RelayStats::framesQueued);
    updateQueueHighWater();
    return true;
}

bool relayProcessNextFrame(TickType_t waitTicks) {
    if (!ROVER_RELAY_MODE || !relayReady || relayFrameQueue == nullptr) {
        vTaskDelay(waitTicks);
        return false;
    }
    RelayFrame frame{};
    if (xQueueReceive(relayFrameQueue, &frame, waitTicks) != pdTRUE) {
        return false;
    }
    portENTER_CRITICAL(&statsMux);
    stats.queueDepth = static_cast<uint32_t>(uxQueueMessagesWaiting(relayFrameQueue));
    portEXIT_CRITICAL(&statsMux);
    return sendFrame(frame);
}

bool relayHandleReceivedPacket(const uint8_t* sourceMac,
                               const uint8_t* data,
                               int length) {
    if (!ROVER_RELAY_MODE || !relayReady || sourceMac == nullptr || data == nullptr ||
        length < static_cast<int>(sizeof(rtcm_espnow::EspNowCommonHeader))) {
        return false;
    }
    rtcm_espnow::EspNowCommonHeader common{};
    std::memcpy(&common, data, sizeof(common));

    if (common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE) {
        bool active = false;
        portENTER_CRITICAL(&relayMux);
        active = childPairingActive;
        if (active && !pendingPairResponse &&
            length == static_cast<int>(sizeof(rtcm_espnow::PairResponsePacket))) {
            std::memcpy(&pendingPairResponsePacket, data, sizeof(pendingPairResponsePacket));
            std::memcpy(pendingPairResponseMac, sourceMac, 6);
            pendingPairResponse = true;
        }
        portEXIT_CRITICAL(&relayMux);
        return active;
    }

    if (common.packetType != rtcm_espnow::PACKET_TYPE_FRAME_ACK || !isChild(sourceMac)) {
        return false;
    }
    rtcm_espnow::RtcmEspNowAck ack{};
    if (length != static_cast<int>(sizeof(ack))) {
        return true;
    }
    std::memcpy(&ack, data, sizeof(ack));
    if (!rtcm_espnow::validateFrameAck(ack, sizeof(ack))) {
        return true;
    }

    bool matches = false;
    portENTER_CRITICAL(&relayMux);
    matches = waitingForAck && ack.streamId == expectedAckStreamId &&
              ack.frameSequence == expectedAckSequence;
    portEXIT_CRITICAL(&relayMux);
    if (matches && relayAckQueue != nullptr) {
        RelayAckEvent event{ack.streamId, ack.frameSequence};
        xQueueSend(relayAckQueue, &event, 0);
    }
    return true;
}

void relayRecordChildRssi(const uint8_t* sourceMac, int8_t rssiDbm) {
    if (!ROVER_RELAY_MODE || !isChild(sourceMac)) {
        return;
    }
    portENTER_CRITICAL(&statsMux);
    stats.hasChildRssi = true;
    stats.lastChildRssiDbm = rssiDbm;
    portEXIT_CRITICAL(&statsMux);
}

RelayStats relayGetStats() {
    portENTER_CRITICAL(&statsMux);
    RelayStats copy = stats;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}

bool relayGetChildMac(uint8_t mac[6]) {
    if (mac == nullptr || !hasChildMac) {
        return false;
    }
    std::memcpy(mac, childMac, 6);
    return true;
}
