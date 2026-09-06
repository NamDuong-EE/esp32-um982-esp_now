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
    uint8_t sourceMac[6];
    uint16_t streamId;
    uint32_t frameSequence;
};

struct RelayChildLlhSlot {
    rtcm_espnow::RoverEcefStatusPacket packet;
    bool pending;
};

constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

QueueHandle_t relayFrameQueue = nullptr;
QueueHandle_t relayAckQueue = nullptr;
bool relayReady = false;
RelayChildStatus childPeers[RELAY_MAX_CHILDREN] = {};
RelayChildLlhSlot childLlhSlots[RELAY_MAX_CHILDREN] = {};
size_t childCount = 0;
size_t llhRoundRobinIndex = 0;
size_t ackStatusRoundRobinIndex = 0;
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
uint8_t expectedAckMac[6] = {};

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

void updateChildCountStats() {
    portENTER_CRITICAL(&relayMux);
    const size_t count = childCount;
    portEXIT_CRITICAL(&relayMux);
    portENTER_CRITICAL(&statsMux);
    stats.childCount = static_cast<uint32_t>(count);
    stats.hasStoredChildMac = count > 0;
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

size_t findChildIndexLocked(const uint8_t* mac) {
    if (mac == nullptr) {
        return RELAY_MAX_CHILDREN;
    }
    for (size_t index = 0; index < childCount; ++index) {
        if (std::memcmp(childPeers[index].mac, mac, 6) == 0) {
            return index;
        }
    }
    return RELAY_MAX_CHILDREN;
}

size_t findChildIndex(const uint8_t* mac) {
    portENTER_CRITICAL(&relayMux);
    const size_t index = findChildIndexLocked(mac);
    portEXIT_CRITICAL(&relayMux);
    return index;
}

size_t copyChildren(RelayChildStatus* destination, size_t capacity) {
    if (destination == nullptr || capacity == 0) {
        return 0;
    }
    portENTER_CRITICAL(&relayMux);
    const size_t count = childCount < capacity ? childCount : capacity;
    for (size_t index = 0; index < count; ++index) {
        destination[index] = childPeers[index];
    }
    portEXIT_CRITICAL(&relayMux);
    return count;
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

void childNvsKey(size_t index, char key[16]) {
    snprintf(key, 16, "%s%u", ESPNOW_NVS_CHILD_MAC_PREFIX,
             static_cast<unsigned>(index));
}

bool saveStoredChildren() {
    RelayChildStatus snapshot[RELAY_MAX_CHILDREN] = {};
    const size_t count = copyChildren(snapshot, RELAY_MAX_CHILDREN);
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, false)) {
        return false;
    }
    bool ok = preferences.putUChar(ESPNOW_NVS_CHILD_COUNT_KEY,
                                   static_cast<uint8_t>(count)) == 1;
    for (size_t index = 0; index < RELAY_MAX_CHILDREN; ++index) {
        char key[16] = {};
        childNvsKey(index, key);
        if (index < count) {
            ok = preferences.putBytes(key, snapshot[index].mac, 6) == 6 && ok;
        } else {
            preferences.remove(key);
        }
    }
    preferences.remove(ESPNOW_NVS_CHILD_MAC_KEY);
    preferences.end();
    return ok;
}

bool loadStoredChildren() {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, true)) {
        return false;
    }
    const uint8_t storedCount = preferences.getUChar(ESPNOW_NVS_CHILD_COUNT_KEY, 0);
    bool migratedLegacy = false;
    childCount = 0;
    for (size_t index = 0;
         index < storedCount && index < RELAY_MAX_CHILDREN;
         ++index) {
        char key[16] = {};
        childNvsKey(index, key);
        uint8_t mac[6] = {};
        if (preferences.getBytesLength(key) != 6 ||
            preferences.getBytes(key, mac, 6) != 6 ||
            !macIsConfigured(mac)) {
            continue;
        }
        bool duplicate = false;
        for (size_t existing = 0; existing < childCount; ++existing) {
            duplicate = duplicate || std::memcmp(childPeers[existing].mac, mac, 6) == 0;
        }
        if (!duplicate) {
            std::memcpy(childPeers[childCount].mac, mac, 6);
            childPeers[childCount].stored = true;
            ++childCount;
        }
    }
    if (childCount == 0) {
        uint8_t legacyMac[6] = {};
        if (preferences.getBytesLength(ESPNOW_NVS_CHILD_MAC_KEY) == 6 &&
            preferences.getBytes(ESPNOW_NVS_CHILD_MAC_KEY, legacyMac, 6) == 6 &&
            macIsConfigured(legacyMac)) {
            std::memcpy(childPeers[0].mac, legacyMac, 6);
            childPeers[0].stored = true;
            childCount = 1;
            migratedLegacy = true;
        }
    }
    preferences.end();
    updateChildCountStats();
    if (migratedLegacy) {
        Serial.println("[RELAY] Migrating legacy child_mac NVS entry");
        return saveStoredChildren();
    }
    return childCount > 0;
}

bool clearStoredChildrenNvs() {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, false)) {
        return false;
    }
    preferences.remove(ESPNOW_NVS_CHILD_COUNT_KEY);
    preferences.remove(ESPNOW_NVS_CHILD_MAC_KEY);
    for (size_t index = 0; index < RELAY_MAX_CHILDREN; ++index) {
        char key[16] = {};
        childNvsKey(index, key);
        preferences.remove(key);
    }
    preferences.end();
    return true;
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
    const size_t existingIndex = findChildIndex(sourceMac);
    if (existingIndex >= RELAY_MAX_CHILDREN) {
        portENTER_CRITICAL(&relayMux);
        const bool full = childCount >= RELAY_MAX_CHILDREN;
        portEXIT_CRITICAL(&relayMux);
        if (full) {
            Serial.printf("[RELAY][PAIR][REJECT] Child list full (%u)\n",
                          static_cast<unsigned>(RELAY_MAX_CHILDREN));
            stopChildPairing("child list full");
            return;
        }
    }
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
    size_t selectedIndex = existingIndex;
    bool added = false;
    if (selectedIndex >= RELAY_MAX_CHILDREN) {
        portENTER_CRITICAL(&relayMux);
        if (childCount < RELAY_MAX_CHILDREN) {
            selectedIndex = childCount++;
            std::memcpy(childPeers[selectedIndex].mac, sourceMac, 6);
            childPeers[selectedIndex].stored = true;
            childLlhSlots[selectedIndex] = {};
            added = true;
        }
        portEXIT_CRITICAL(&relayMux);
    }
    if (selectedIndex >= RELAY_MAX_CHILDREN) {
        Serial.println("[RELAY][PAIR][ERROR] Khong con slot child sau PAIR_CONFIRM");
        stopChildPairing("no child slot");
        return;
    }
    updateChildCountStats();
    if (added && !saveStoredChildren()) {
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            childPeers[selectedIndex].stored = false;
        }
        portEXIT_CRITICAL(&relayMux);
        Serial.println("[RELAY][PAIR][ERROR] Khong luu duoc danh sach child vao NVS");
    }
    stopChildPairing(added ? "paired new child" : "reconfirmed child");
    if (!addPeer(sourceMac, ESPNOW_ENCRYPTION_ENABLED)) {
        Serial.println("[RELAY][PAIR][ERROR] Child da luu nhung runtime peer chua san sang");
        return;
    }
    Serial.printf("[RELAY][PAIR] Child %s index=%u count=%u\n",
                  macToString(sourceMac).c_str(),
                  static_cast<unsigned>(selectedIndex),
                  static_cast<unsigned>(childCount));
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

bool childIsCoolingDown(size_t childIndex, uint32_t now) {
    bool coolingDown = false;
    portENTER_CRITICAL(&relayMux);
    if (childIndex < childCount && childPeers[childIndex].cooldownUntilMs != 0 &&
        static_cast<int32_t>(now - childPeers[childIndex].cooldownUntilMs) < 0) {
        coolingDown = true;
        ++childPeers[childIndex].framesSkippedCooldown;
    }
    portEXIT_CRITICAL(&relayMux);
    if (coolingDown) {
        incrementStat(&RelayStats::framesSkippedCooldown);
    }
    return coolingDown;
}

void recordChildFrameOutcome(size_t childIndex, bool success) {
    portENTER_CRITICAL(&relayMux);
    if (childIndex < childCount) {
        RelayChildStatus& child = childPeers[childIndex];
        if (success) {
            child.consecutiveFailures = 0;
            child.cooldownUntilMs = 0;
        } else {
            ++child.sendFailures;
            if (child.consecutiveFailures < UINT8_MAX) {
                ++child.consecutiveFailures;
            }
            if (child.consecutiveFailures >= RELAY_CHILD_FAILURES_BEFORE_COOLDOWN) {
                child.cooldownUntilMs = millis() + RELAY_CHILD_FAILURE_COOLDOWN_MS;
            }
        }
    }
    portEXIT_CRITICAL(&relayMux);
}

bool sendFrameToChild(const RelayFrame& relayFrame,
                      const uint8_t* destinationMac,
                      size_t childIndex) {
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
        std::memcpy(expectedAckMac, destinationMac, sizeof(expectedAckMac));
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
            if (!sendFragment(destinationMac, packet, sizeof(header) + payloadLength)) {
                allFragmentsSent = false;
                break;
            }
        }

        if (allFragmentsSent) {
            incrementStat(&RelayStats::framesSent);
            portENTER_CRITICAL(&relayMux);
            if (childIndex < childCount) {
                ++childPeers[childIndex].framesSent;
            }
            portEXIT_CRITICAL(&relayMux);
            RelayAckEvent ack{};
            if (xQueueReceive(relayAckQueue, &ack,
                              pdMS_TO_TICKS(RELAY_ACK_TIMEOUT_MS)) == pdTRUE) {
                portENTER_CRITICAL(&relayMux);
                waitingForAck = false;
                portEXIT_CRITICAL(&relayMux);
                const uint32_t ackAtMs = millis();
                portENTER_CRITICAL(&statsMux);
                ++stats.framesAcked;
                stats.lastAckMillis = ackAtMs;
                portEXIT_CRITICAL(&statsMux);
                portENTER_CRITICAL(&relayMux);
                if (childIndex < childCount) {
                    ++childPeers[childIndex].framesAcked;
                    childPeers[childIndex].lastAckMillis = ackAtMs;
                    childPeers[childIndex].pendingAckStreamId = ack.streamId;
                    childPeers[childIndex].pendingAckFrameSequence =
                        ack.frameSequence;
                    childPeers[childIndex].ackStatusPending = true;
                }
                portEXIT_CRITICAL(&relayMux);
                recordChildFrameOutcome(childIndex, true);
                return true;
            }
            if (isChildPairingActive()) {
                clearWaitingForAck();
                return false;
            }
            incrementStat(&RelayStats::ackTimeouts);
            portENTER_CRITICAL(&relayMux);
            if (childIndex < childCount) {
                ++childPeers[childIndex].ackTimeouts;
            }
            portEXIT_CRITICAL(&relayMux);
        }
        if (attempt < RELAY_FRAME_RETRY_COUNT) {
            incrementStat(&RelayStats::frameRetries);
            portENTER_CRITICAL(&relayMux);
            if (childIndex < childCount) {
                ++childPeers[childIndex].frameRetries;
            }
            portEXIT_CRITICAL(&relayMux);
        }
    }

    clearWaitingForAck();
    recordChildFrameOutcome(childIndex, false);
    return false;
}

bool sendFrame(const RelayFrame& relayFrame) {
    RelayChildStatus children[RELAY_MAX_CHILDREN] = {};
    const size_t count = copyChildren(children, RELAY_MAX_CHILDREN);
    bool anyDelivered = false;
    for (size_t index = 0; index < count; ++index) {
        if (isChildPairingActive()) {
            clearWaitingForAck();
            return anyDelivered;
        }
        if (childIsCoolingDown(index, millis())) {
            continue;
        }
        const bool delivered = sendFrameToChild(relayFrame, children[index].mac, index);
        anyDelivered = anyDelivered || delivered;
        if (!delivered) {
            Serial.printf("[RELAY][RTCM][WARN] child=%s frame=%lu delivery=failed\n",
                          macToString(children[index].mac).c_str(),
                          static_cast<unsigned long>(relayFrame.frameSequence));
        }
    }
    return anyDelivered;
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
    relayAckQueue = xQueueCreate(RELAY_MAX_CHILDREN * 2, sizeof(RelayAckEvent));
    if (relayFrameQueue == nullptr || relayAckQueue == nullptr) {
        Serial.println("[RELAY][ERROR] Khong tao duoc relay queue");
        if (relayFrameQueue != nullptr) {
            vQueueDelete(relayFrameQueue);
            relayFrameQueue = nullptr;
        }
        if (relayAckQueue != nullptr) {
            vQueueDelete(relayAckQueue);
            relayAckQueue = nullptr;
        }
        return false;
    }

    loadStoredChildren();
    size_t readyChildren = 0;
    for (size_t index = 0; index < childCount; ++index) {
        if (addPeer(childPeers[index].mac, ESPNOW_ENCRYPTION_ENABLED)) {
            ++readyChildren;
            Serial.printf("[RELAY] Loaded child[%u] MAC=%s\n",
                          static_cast<unsigned>(index),
                          macToString(childPeers[index].mac).c_str());
        }
    }
    if (childCount == 0) {
        Serial.println("[RELAY] No stored child MAC; hold pairing button after Base pairing");
    } else if (readyChildren != childCount) {
        Serial.printf("[RELAY][WARN] Runtime peers ready=%u stored=%u\n",
                      static_cast<unsigned>(readyChildren),
                      static_cast<unsigned>(childCount));
    }
    relayReady = true;
    Serial.printf("[RELAY] Downstream relay ready children=%u max=%u clear_hold_ms=%lu\n",
                  static_cast<unsigned>(childCount),
                  static_cast<unsigned>(RELAY_MAX_CHILDREN),
                  static_cast<unsigned long>(RELAY_CHILD_CLEAR_HOLD_MS));
    return true;
}

bool relayIsReady() {
    return relayReady;
}

bool relayIsChildPairingActive() {
    return ROVER_RELAY_MODE && isChildPairingActive();
}

void clearAllChildren() {
    if (isChildPairingActive()) {
        stopChildPairing("clear all children");
    }
    uint8_t removedMacs[RELAY_MAX_CHILDREN][6] = {};
    size_t removedCount = 0;
    portENTER_CRITICAL(&relayMux);
    removedCount = childCount;
    for (size_t index = 0; index < childCount; ++index) {
        std::memcpy(removedMacs[index], childPeers[index].mac, 6);
    }
    std::memset(childPeers, 0, sizeof(childPeers));
    std::memset(childLlhSlots, 0, sizeof(childLlhSlots));
    childCount = 0;
    llhRoundRobinIndex = 0;
    waitingForAck = false;
    std::memset(expectedAckMac, 0, sizeof(expectedAckMac));
    portEXIT_CRITICAL(&relayMux);

    for (size_t index = 0; index < removedCount; ++index) {
        if (esp_now_is_peer_exist(removedMacs[index])) {
            esp_now_del_peer(removedMacs[index]);
        }
    }
    if (relayFrameQueue != nullptr) {
        xQueueReset(relayFrameQueue);
    }
    if (relayAckQueue != nullptr) {
        xQueueReset(relayAckQueue);
    }
    portENTER_CRITICAL(&statsMux);
    stats.queueDepth = 0;
    portEXIT_CRITICAL(&statsMux);
    updateChildCountStats();
    incrementStat(&RelayStats::childClearEvents);
    if (!clearStoredChildrenNvs()) {
        Serial.println("[RELAY][CHILD_CLEAR][ERROR] Khong xoa duoc danh sach child trong NVS");
    }
    Serial.printf("[RELAY][CHILD_CLEAR] Removed %u child(s) after %lu ms hold\n",
                  static_cast<unsigned>(removedCount),
                  static_cast<unsigned long>(RELAY_CHILD_CLEAR_HOLD_MS));
}

void relayLoop() {
    if (!ROVER_RELAY_MODE || !relayReady) {
        return;
    }

    static uint32_t pressedSinceMs = 0;
    static bool pairingStartHandled = false;
    static bool clearHandled = false;
    uint8_t baseMac[6] = {};
    const bool hasBase = espnowGetBaseMac(baseMac);
    const bool rawLevel = digitalRead(PAIRING_BUTTON_PIN) == HIGH;
    const bool pressed = PAIRING_BUTTON_ACTIVE_LOW ? !rawLevel : rawLevel;
    const uint32_t now = millis();

    if (pressed) {
        if (pressedSinceMs == 0) {
            pressedSinceMs = now;
        } else {
            const uint32_t heldMs = now - pressedSinceMs;
            if (!clearHandled && heldMs >= RELAY_CHILD_CLEAR_HOLD_MS) {
                clearAllChildren();
                clearHandled = true;
                pairingStartHandled = true;
            } else if (hasBase && !pairingStartHandled &&
                       heldMs >= PAIRING_BUTTON_HOLD_MS) {
                startChildPairing();
                pairingStartHandled = true;
            }
        }
    } else if (!pressed) {
        pressedSinceMs = 0;
        pairingStartHandled = false;
        clearHandled = false;
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
    portENTER_CRITICAL(&relayMux);
    const bool hasChildren = childCount > 0;
    portEXIT_CRITICAL(&relayMux);
    if (!hasChildren) {
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

bool relayProcessNextAckStatus() {
    if (!ROVER_RELAY_MODE || !relayReady || isChildPairingActive()) {
        return false;
    }

    const uint32_t now = millis();
    size_t selectedIndex = RELAY_MAX_CHILDREN;
    uint8_t childMac[6] = {};
    uint16_t streamId = 0;
    uint32_t frameSequence = 0;
    uint32_t ackAtMs = 0;
    portENTER_CRITICAL(&relayMux);
    for (size_t offset = 0; offset < childCount; ++offset) {
        const size_t index = (ackStatusRoundRobinIndex + offset) % childCount;
        const RelayChildStatus& child = childPeers[index];
        if (child.ackStatusPending &&
            (child.lastAckStatusSentMillis == 0 ||
             now - child.lastAckStatusSentMillis >=
                 RELAY_CHILD_ACK_STATUS_INTERVAL_MS)) {
            selectedIndex = index;
            std::memcpy(childMac, child.mac, sizeof(childMac));
            streamId = child.pendingAckStreamId;
            frameSequence = child.pendingAckFrameSequence;
            ackAtMs = child.lastAckMillis;
            ackStatusRoundRobinIndex = (index + 1) % childCount;
            break;
        }
    }
    portEXIT_CRITICAL(&relayMux);
    if (selectedIndex >= RELAY_MAX_CHILDREN) {
        return false;
    }

    uint8_t baseMac[6] = {};
    if (!espnowGetBaseMac(baseMac)) {
        incrementStat(&RelayStats::childAckStatusFailures);
        return false;
    }

    rtcm_espnow::RelayedRoverRtcmAckStatusPacket packet{};
    packet.common.magic = rtcm_espnow::MAGIC;
    packet.common.version = rtcm_espnow::VERSION;
    packet.common.packetType =
        rtcm_espnow::PACKET_TYPE_RELAYED_ROVER_RTCM_ACK_STATUS;
    std::memcpy(packet.roverMac, childMac, sizeof(packet.roverMac));
    packet.streamId = streamId;
    packet.frameSequence = frameSequence;
    packet.ackAgeMs = now - ackAtMs;
    if (!rtcm_espnow::validateRelayedRoverRtcmAckStatus(packet,
                                                        sizeof(packet))) {
        incrementStat(&RelayStats::childAckStatusFailures);
        return false;
    }

    const EspNowTxResult result = espnowTxTrySend(
        baseMac,
        reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet));
    if (result == EspNowTxResult::Busy) {
        incrementStat(&RelayStats::childAckStatusBusy);
        return false;
    }
    if (result != EspNowTxResult::Success) {
        incrementStat(&RelayStats::childAckStatusFailures);
        Serial.printf("[RELAY][ACK-STATUS][WARN] child=%s seq=%lu result=%s\n",
                      macToString(childMac).c_str(),
                      static_cast<unsigned long>(frameSequence),
                      espnowTxResultToString(result));
        return false;
    }

    portENTER_CRITICAL(&relayMux);
    if (selectedIndex < childCount &&
        std::memcmp(childPeers[selectedIndex].mac, childMac, 6) == 0) {
        RelayChildStatus& child = childPeers[selectedIndex];
        child.lastAckStatusSentMillis = millis();
        if (child.pendingAckStreamId == streamId &&
            child.pendingAckFrameSequence == frameSequence) {
            child.ackStatusPending = false;
        }
    }
    portEXIT_CRITICAL(&relayMux);
    incrementStat(&RelayStats::childAckStatusForwarded);
    Serial.printf("[RELAY][ACK-STATUS] Forwarded child=%s stream=%u seq=%lu age_ms=%lu\n",
                  macToString(childMac).c_str(),
                  static_cast<unsigned>(streamId),
                  static_cast<unsigned long>(frameSequence),
                  static_cast<unsigned long>(packet.ackAgeMs));
    return true;
}

bool relayProcessNextLlh(TickType_t waitTicks) {
    if (!ROVER_RELAY_MODE || !relayReady) {
        vTaskDelay(waitTicks);
        return false;
    }

    rtcm_espnow::RoverEcefStatusPacket childPacket{};
    uint8_t sourceChildMac[6] = {};
    size_t selectedIndex = RELAY_MAX_CHILDREN;
    portENTER_CRITICAL(&relayMux);
    for (size_t offset = 0; offset < childCount; ++offset) {
        const size_t index = (llhRoundRobinIndex + offset) % childCount;
        if (childLlhSlots[index].pending) {
            selectedIndex = index;
            childPacket = childLlhSlots[index].packet;
            childLlhSlots[index].pending = false;
            std::memcpy(sourceChildMac, childPeers[index].mac, 6);
            llhRoundRobinIndex = (index + 1) % childCount;
            break;
        }
    }
    portEXIT_CRITICAL(&relayMux);
    if (selectedIndex >= RELAY_MAX_CHILDREN) {
        vTaskDelay(waitTicks);
        return false;
    }
    if (isChildPairingActive()) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            ++childPeers[selectedIndex].llhForwardSkipped;
        }
        portEXIT_CRITICAL(&relayMux);
        return false;
    }

    uint8_t baseMac[6] = {};
    if (!espnowGetBaseMac(baseMac)) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            ++childPeers[selectedIndex].llhForwardSkipped;
        }
        portEXIT_CRITICAL(&relayMux);
        return false;
    }

    rtcm_espnow::RelayedRoverEcefStatusPacket forwarded{};
    forwarded.common.magic = rtcm_espnow::MAGIC;
    forwarded.common.version = rtcm_espnow::VERSION;
    forwarded.common.packetType =
        rtcm_espnow::PACKET_TYPE_RELAYED_ROVER_ECEF_STATUS;
    forwarded.sequence = childPacket.sequence;
    std::memcpy(forwarded.roverMac, sourceChildMac, sizeof(forwarded.roverMac));
    forwarded.gnssTimeMsOfDay = childPacket.gnssTimeMsOfDay;
    forwarded.correctionStreamId = childPacket.correctionStreamId;
    forwarded.ecefXScaled = childPacket.ecefXScaled;
    forwarded.ecefYScaled = childPacket.ecefYScaled;
    forwarded.ecefZScaled = childPacket.ecefZScaled;
    forwarded.fixQuality = childPacket.fixQuality;
    if (!rtcm_espnow::validateRelayedRoverEcefStatus(forwarded,
                                                     sizeof(forwarded))) {
        incrementStat(&RelayStats::childLlhForwardFailures);
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            ++childPeers[selectedIndex].llhForwardFailures;
        }
        portEXIT_CRITICAL(&relayMux);
        return false;
    }

    const EspNowTxResult result = espnowTxTrySend(
        baseMac,
        reinterpret_cast<const uint8_t*>(&forwarded),
        sizeof(forwarded));
    if (result == EspNowTxResult::Busy) {
        incrementStat(&RelayStats::childLlhForwardSkipped);
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            ++childPeers[selectedIndex].llhForwardSkipped;
        }
        portEXIT_CRITICAL(&relayMux);
        return false;
    }
    if (result != EspNowTxResult::Success) {
        incrementStat(&RelayStats::childLlhForwardFailures);
        Serial.printf("[RELAY][LLH][WARN] child=%s seq=%lu result=%s\n",
                      macToString(sourceChildMac).c_str(),
                      static_cast<unsigned long>(forwarded.sequence),
                      espnowTxResultToString(result));
        portENTER_CRITICAL(&relayMux);
        if (selectedIndex < childCount) {
            ++childPeers[selectedIndex].llhForwardFailures;
        }
        portEXIT_CRITICAL(&relayMux);
        return false;
    }

    incrementStat(&RelayStats::childLlhForwarded);
    portENTER_CRITICAL(&relayMux);
    if (selectedIndex < childCount) {
        ++childPeers[selectedIndex].llhForwarded;
    }
    portEXIT_CRITICAL(&relayMux);
    Serial.printf("[RELAY][LLH] Forwarded child=%s seq=%lu fix_quality=%u\n",
                  macToString(sourceChildMac).c_str(),
                  static_cast<unsigned long>(forwarded.sequence),
                  static_cast<unsigned>(forwarded.fixQuality));
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

    const size_t sourceChildIndex = findChildIndex(sourceMac);
    if (common.packetType == rtcm_espnow::PACKET_TYPE_ROVER_ECEF_STATUS &&
        sourceChildIndex < RELAY_MAX_CHILDREN) {
        if (length !=
            static_cast<int>(sizeof(rtcm_espnow::RoverEcefStatusPacket))) {
            incrementStat(&RelayStats::childLlhInvalid);
            return true;
        }
        rtcm_espnow::RoverEcefStatusPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        if (!rtcm_espnow::validateRoverEcefStatus(packet, sizeof(packet))) {
            incrementStat(&RelayStats::childLlhInvalid);
            return true;
        }
        bool overwritten = false;
        portENTER_CRITICAL(&relayMux);
        if (sourceChildIndex < childCount) {
            overwritten = childLlhSlots[sourceChildIndex].pending;
            childLlhSlots[sourceChildIndex].packet = packet;
            childLlhSlots[sourceChildIndex].pending = true;
            ++childPeers[sourceChildIndex].llhReceived;
        }
        portEXIT_CRITICAL(&relayMux);
        if (overwritten) {
            incrementStat(&RelayStats::childLlhQueueOverwrites);
        }
        incrementStat(&RelayStats::childLlhReceived);
        return true;
    }

    if (common.packetType != rtcm_espnow::PACKET_TYPE_FRAME_ACK ||
        sourceChildIndex >= RELAY_MAX_CHILDREN) {
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
              ack.frameSequence == expectedAckSequence &&
              std::memcmp(sourceMac, expectedAckMac, 6) == 0;
    portEXIT_CRITICAL(&relayMux);
    if (matches && relayAckQueue != nullptr) {
        RelayAckEvent event{};
        std::memcpy(event.sourceMac, sourceMac, sizeof(event.sourceMac));
        event.streamId = ack.streamId;
        event.frameSequence = ack.frameSequence;
        xQueueSend(relayAckQueue, &event, 0);
    }
    return true;
}

bool relayRequestChildrenRtkReset(uint32_t parentTransactionId) {
    if (!ROVER_RELAY_MODE || !relayReady || parentTransactionId == 0) {
        return false;
    }
    RelayChildStatus children[RELAY_MAX_CHILDREN] = {};
    const size_t count = copyChildren(children, RELAY_MAX_CHILDREN);
    bool allSent = true;
    for (size_t index = 0; index < count; ++index) {
        rtcm_espnow::GnssCommandRequestPacket request{};
        request.common.magic = rtcm_espnow::MAGIC;
        request.common.version = rtcm_espnow::VERSION;
        request.common.packetType =
            rtcm_espnow::PACKET_TYPE_GNSS_COMMAND_REQUEST;
        request.networkId = ESPNOW_NETWORK_ID;
        request.transactionId =
            parentTransactionId ^ (0x524C0000UL |
                                   static_cast<uint32_t>(index + 1));
        request.commandId = rtcm_espnow::GNSS_COMMAND_RESET_RTK;
        request.targetPort = rtcm_espnow::GNSS_PORT_COM2;
        request.authTag = rtcm_espnow::pairingAuthTag(
            request,
            ESPNOW_PAIRING_KEY,
            sizeof(ESPNOW_PAIRING_KEY));
        const EspNowTxResult result = espnowTxSend(
            children[index].mac,
            reinterpret_cast<const uint8_t*>(&request),
            sizeof(request));
        const bool sent = result == EspNowTxResult::Success;
        allSent = allSent && sent;
        Serial.printf("[RELAY][GNSS_CMD] child=%s action=rtk_reset "
                      "txn=%lu result=%s\n",
                      macToString(children[index].mac).c_str(),
                      static_cast<unsigned long>(request.transactionId),
                      espnowTxResultToString(result));
    }
    return allSent;
}

bool relayRequestChildrenRtkResume(uint32_t parentTransactionId) {
    if (!ROVER_RELAY_MODE || !relayReady || parentTransactionId == 0) {
        return false;
    }
    RelayChildStatus children[RELAY_MAX_CHILDREN] = {};
    const size_t count = copyChildren(children, RELAY_MAX_CHILDREN);
    bool allSent = true;
    for (size_t index = 0; index < count; ++index) {
        rtcm_espnow::GnssCommandRequestPacket request{};
        request.common.magic = rtcm_espnow::MAGIC;
        request.common.version = rtcm_espnow::VERSION;
        request.common.packetType =
            rtcm_espnow::PACKET_TYPE_GNSS_COMMAND_REQUEST;
        request.networkId = ESPNOW_NETWORK_ID;
        request.transactionId =
            parentTransactionId ^ (0x52530000UL |
                                   static_cast<uint32_t>(index + 1));
        request.commandId = rtcm_espnow::GNSS_COMMAND_RESUME_RTK;
        request.targetPort = rtcm_espnow::GNSS_PORT_COM2;
        request.authTag = rtcm_espnow::pairingAuthTag(
            request,
            ESPNOW_PAIRING_KEY,
            sizeof(ESPNOW_PAIRING_KEY));
        const EspNowTxResult result = espnowTxSend(
            children[index].mac,
            reinterpret_cast<const uint8_t*>(&request),
            sizeof(request));
        const bool sent = result == EspNowTxResult::Success;
        allSent = allSent && sent;
        Serial.printf("[RELAY][GNSS_CMD] child=%s action=rtk_resume "
                      "txn=%lu result=%s\n",
                      macToString(children[index].mac).c_str(),
                      static_cast<unsigned long>(request.transactionId),
                      espnowTxResultToString(result));
    }
    return allSent;
}

RelayStats relayGetStats() {
    portENTER_CRITICAL(&statsMux);
    RelayStats copy = stats;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}

bool relayGetChildMac(uint8_t mac[6]) {
    if (mac == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&relayMux);
    const bool available = childCount > 0;
    if (available) {
        std::memcpy(mac, childPeers[0].mac, 6);
    }
    portEXIT_CRITICAL(&relayMux);
    if (!available) {
        return false;
    }
    return true;
}

size_t relayCopyChildren(RelayChildStatus* destination, size_t capacity) {
    return copyChildren(destination, capacity);
}
