#include "hardware/Relay_handler.h"

#include <Preferences.h>
#include <WiFi.h>
#include <cstring>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "Prog_Config.h"
#include "hardware/Espnow_handler.h"
#include "hardware/Espnow_tx_manager.h"
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

struct RelayLlhEnvelope {
    uint8_t childMac[6];
    rtcm_espnow::RoverLlhStatusPacket packet;
};

constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

QueueHandle_t relayFrameQueue = nullptr;
QueueHandle_t relayAckQueue = nullptr;
QueueHandle_t relayLlhQueue = nullptr;
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

bool isChildPairingActive() {
    portENTER_CRITICAL(&relayMux);
    const bool active = childPairingActive;
    portEXIT_CRITICAL(&relayMux);
    return active;
}

void clearWaitingForAck() {
    portENTER_CRITICAL(&relayMux);
    waitingForAck = false;
    portEXIT_CRITICAL(&relayMux);
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

bool sendPacketSync(const uint8_t* destinationMac,
                    const uint8_t* packet,
                    std::size_t packetLength) {
    const EspNowTxResult result = espnowTxSend(destinationMac, packet, packetLength);
    if (result == EspNowTxResult::Success) {
        return true;
    }

    incrementStat(&RelayStats::sendFailures);
    switch (result) {
        case EspNowTxResult::CallbackTimeout:
            incrementStat(&RelayStats::sendCallbackTimeouts);
            break;
        case EspNowTxResult::DeliveryFailed:
            incrementStat(&RelayStats::sendDeliveryFailures);
            break;
        default:
            incrementStat(&RelayStats::sendImmediateErrors);
            break;
    }
    return false;
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
    const uint32_t now = millis();
    portENTER_CRITICAL(&relayMux);
    childPairingActive = true;
    pendingPairResponse = false;
    waitingForAck = false;
    childPairingEndsAtMs = now + PAIRING_WINDOW_MS;
    childPairingBaseNonce = esp_random();
    lastDiscoverySentAtMs = 0;
    discoveryTxLogged = false;
    portEXIT_CRITICAL(&relayMux);
    setChildPairingStat(true);

    // RTCM received before/during pairing is stale for the new child and must
    // not compete with PAIR_DISCOVERY on the shared ESP-NOW transmitter.
    if (relayFrameQueue != nullptr) {
        const uint32_t dropped =
            static_cast<uint32_t>(uxQueueMessagesWaiting(relayFrameQueue));
        xQueueReset(relayFrameQueue);
        if (dropped > 0) {
            incrementStat(&RelayStats::framesSuppressedDuringPairing, dropped);
            Serial.printf("[RELAY][PAIR] Dropped %lu queued RTCM frame(s)\n",
                          static_cast<unsigned long>(dropped));
        }
        portENTER_CRITICAL(&statsMux);
        stats.queueDepth = 0;
        portEXIT_CRITICAL(&statsMux);
    }
    if (relayAckQueue != nullptr) {
        xQueueReset(relayAckQueue);
    }
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

    Serial.printf("[RELAY][PAIR][DIAG] Process PAIR_RESPONSE src=%s len=%u expected_base_nonce=0x%08lX\n",
                  macToString(sourceMac).c_str(),
                  static_cast<unsigned>(sizeof(response)),
                  static_cast<unsigned long>(expectedNonce));

    const uint32_t expectedAuth = rtcm_espnow::pairingAuthTag(
        response, ESPNOW_PAIRING_KEY, sizeof(ESPNOW_PAIRING_KEY));
    if (!rtcm_espnow::validatePairResponse(response,
                                           sizeof(response),
                                           ESPNOW_NETWORK_ID,
                                           expectedNonce,
                                           ESPNOW_PAIRING_KEY,
                                           sizeof(ESPNOW_PAIRING_KEY))) {
        incrementStat(&RelayStats::childPairAuthFailures);
        Serial.printf("[RELAY][PAIR][DIAG][REJECT] PAIR_RESPONSE magic=%s version=%s type=%s role=%s network=%s nonce=%s auth=%s received_auth=0x%08lX expected_auth=0x%08lX\n",
                      response.common.magic == rtcm_espnow::MAGIC ? "ok" : "bad",
                      response.common.version == rtcm_espnow::VERSION ? "ok" : "bad",
                      response.common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE ? "ok" : "bad",
                      response.role == rtcm_espnow::ROLE_ROVER ? "ok" : "bad",
                      response.networkId == ESPNOW_NETWORK_ID ? "ok" : "bad",
                      response.baseNonceEcho == expectedNonce ? "ok" : "bad",
                      response.authTag == expectedAuth ? "ok" : "bad",
                      static_cast<unsigned long>(response.authTag),
                      static_cast<unsigned long>(expectedAuth));
        return;
    }
    Serial.println("[RELAY][PAIR][DIAG] PAIR_RESPONSE validation=ok");
    incrementStat(&RelayStats::childPairResponsesReceived);
    if (!addPeer(sourceMac, false)) {
        Serial.println("[RELAY][PAIR][DIAG][REJECT] Temporary Child peer add=failed");
        return;
    }
    Serial.printf("[RELAY][PAIR][DIAG] Temporary Child peer add=ok channel=%u\n",
                  static_cast<unsigned>(getWiFiChannel()));

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
    Serial.println("[RELAY][PAIR][DIAG] Calling TX PAIR_CONFIRM");
    if (!sendPacketSync(sourceMac,
                        reinterpret_cast<const uint8_t*>(&confirm),
                        sizeof(confirm))) {
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
        if (isChildPairingActive()) {
            return false;
        }
        if (sendPacketSync(child, packet, packetLength)) {
            incrementStat(&RelayStats::fragmentsSent);
            vTaskDelay(pdMS_TO_TICKS(RELAY_FRAGMENT_GAP_MS));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool sendFrame(const RelayFrame& relayFrame) {
    uint8_t packet[rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE] = {};
    const uint8_t fragmentCount = rtcm_espnow::expectedFragmentCount(relayFrame.frameLength);

    for (uint8_t attempt = 0; attempt <= RELAY_FRAME_RETRY_COUNT; ++attempt) {
        if (isChildPairingActive()) {
            clearWaitingForAck();
            return false;
        }
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
            if (isChildPairingActive()) {
                clearWaitingForAck();
                return false;
            }
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
            if (isChildPairingActive()) {
                clearWaitingForAck();
                return false;
            }
            incrementStat(&RelayStats::ackTimeouts);
        }
        if (attempt < RELAY_FRAME_RETRY_COUNT) {
            incrementStat(&RelayStats::frameRetries);
        }
    }

    clearWaitingForAck();
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
    // Register the broadcast peer before RTCM tasks start. Child pairing then
    // never mutates this peer while downstream transmissions are in flight.
    if (!addPeer(BROADCAST_MAC, false)) {
        Serial.println("[RELAY][ERROR] Khong cau hinh duoc broadcast pairing peer");
        return false;
    }
    relayFrameQueue = xQueueCreate(RELAY_QUEUE_LENGTH, sizeof(RelayFrame));
    relayAckQueue = xQueueCreate(4, sizeof(RelayAckEvent));
    relayLlhQueue = xQueueCreate(RELAY_LLH_QUEUE_LENGTH, sizeof(RelayLlhEnvelope));
    if (relayFrameQueue == nullptr || relayAckQueue == nullptr || relayLlhQueue == nullptr) {
        Serial.println("[RELAY][ERROR] Khong tao duoc relay queue");
        if (relayFrameQueue != nullptr) {
            vQueueDelete(relayFrameQueue);
            relayFrameQueue = nullptr;
        }
        if (relayAckQueue != nullptr) {
            vQueueDelete(relayAckQueue);
            relayAckQueue = nullptr;
        }
        if (relayLlhQueue != nullptr) {
            vQueueDelete(relayLlhQueue);
            relayLlhQueue = nullptr;
        }
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

bool relayIsChildPairingActive() {
    return ROVER_RELAY_MODE && isChildPairingActive();
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
    if (isChildPairingActive()) {
        incrementStat(&RelayStats::framesSuppressedDuringPairing);
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
    if (isChildPairingActive()) {
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
    if (isChildPairingActive()) {
        incrementStat(&RelayStats::framesSuppressedDuringPairing);
        return false;
    }
    const bool sent = sendFrame(frame);
    if (!sent) {
        if (isChildPairingActive()) {
            incrementStat(&RelayStats::framesSuppressedDuringPairing);
        } else {
            incrementStat(&RelayStats::backoffEvents);
            vTaskDelay(pdMS_TO_TICKS(RELAY_FAILED_FRAME_BACKOFF_MS));
        }
    }
    return sent;
}

bool relayProcessNextLlh(TickType_t waitTicks) {
    if (!ROVER_RELAY_MODE || !relayReady || relayLlhQueue == nullptr) {
        vTaskDelay(waitTicks);
        return false;
    }

    RelayLlhEnvelope envelope{};
    if (xQueueReceive(relayLlhQueue, &envelope, waitTicks) != pdTRUE) {
        return false;
    }
    if (isChildPairingActive()) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        return false;
    }

    uint8_t baseMac[6] = {};
    if (!espnowGetBaseMac(baseMac)) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        return false;
    }

    rtcm_espnow::RelayedRoverLlhStatusPacket forwarded{};
    forwarded.common.magic = rtcm_espnow::MAGIC;
    forwarded.common.version = rtcm_espnow::VERSION;
    forwarded.common.packetType = rtcm_espnow::PACKET_TYPE_RELAYED_ROVER_LLH_STATUS;
    forwarded.sequence = envelope.packet.sequence;
    std::memcpy(forwarded.roverMac, envelope.childMac, sizeof(forwarded.roverMac));
    forwarded.latitudeE7 = envelope.packet.latitudeE7;
    forwarded.longitudeE7 = envelope.packet.longitudeE7;
    forwarded.heightMm = envelope.packet.heightMm;
    if (!rtcm_espnow::validateRelayedRoverLlhStatus(forwarded, sizeof(forwarded))) {
        incrementStat(&RelayStats::childLlhForwardFailures);
        return false;
    }

    const EspNowTxResult result = espnowTxTrySend(
        baseMac,
        reinterpret_cast<const uint8_t*>(&forwarded),
        sizeof(forwarded));
    if (result == EspNowTxResult::Busy) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        return false;
    }
    if (result != EspNowTxResult::Success) {
        incrementStat(&RelayStats::childLlhForwardFailures);
        Serial.printf("[RELAY][LLH][WARN] child=%s seq=%lu result=%s\n",
                      macToString(envelope.childMac).c_str(),
                      static_cast<unsigned long>(forwarded.sequence),
                      espnowTxResultToString(result));
        return false;
    }

    incrementStat(&RelayStats::childLlhForwarded);
    Serial.printf("[RELAY][LLH] Forwarded child=%s seq=%lu\n",
                  macToString(envelope.childMac).c_str(),
                  static_cast<unsigned long>(forwarded.sequence));
    return true;
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

    if (common.packetType == rtcm_espnow::PACKET_TYPE_ROVER_LLH_STATUS &&
        isChild(sourceMac)) {
        if (length != static_cast<int>(sizeof(rtcm_espnow::RoverLlhStatusPacket))) {
            incrementStat(&RelayStats::childLlhInvalid);
            return true;
        }
        RelayLlhEnvelope envelope{};
        std::memcpy(&envelope.packet, data, sizeof(envelope.packet));
        if (!rtcm_espnow::validateRoverLlhStatus(envelope.packet,
                                                 sizeof(envelope.packet))) {
            incrementStat(&RelayStats::childLlhInvalid);
            return true;
        }
        std::memcpy(envelope.childMac, sourceMac, sizeof(envelope.childMac));
        if (relayLlhQueue == nullptr) {
            incrementStat(&RelayStats::childLlhForwardFailures);
            return true;
        }
        if (uxQueueMessagesWaiting(relayLlhQueue) > 0) {
            incrementStat(&RelayStats::childLlhQueueOverwrites);
        }
        if (xQueueOverwrite(relayLlhQueue, &envelope) != pdPASS) {
            incrementStat(&RelayStats::childLlhForwardFailures);
            return true;
        }
        incrementStat(&RelayStats::childLlhReceived);
        return true;
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
