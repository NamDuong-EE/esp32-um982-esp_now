#include "hardware/Espnow_handler.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#include "Prog_Config.h"
#include "hardware/Espnow_tx_manager.h"
#include "hardware/Relay_handler.h"
#include "hardware/Wifi_handler.h"

namespace {

QueueHandle_t receiveQueue = nullptr;
QueueHandle_t pairDiagnosticQueue = nullptr;
bool coreReady = false;
bool ready = false;
EspNowRtcmStats stats{};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE pairingMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t activeBaseMac[6] = {};
bool hasActiveBaseMac = false;
bool pairingButtonReady = false;
bool pairingActive = false;
bool awaitingPairConfirm = false;
bool pendingPairSave = false;
bool pendingPairDiscovery = false;
bool pendingPairConfirm = false;
uint32_t pairingEndsAtMs = 0;
uint32_t pendingBaseNonce = 0;
uint32_t pendingRoverNonce = 0;
uint8_t pendingBaseMac[6] = {};
uint8_t confirmedBaseMac[6] = {};
uint8_t pendingPairDiscoveryMac[6] = {};
uint8_t pendingPairConfirmMac[6] = {};
rtcm_espnow::PairDiscoveryPacket pendingPairDiscoveryPacket{};
rtcm_espnow::PairConfirmPacket pendingPairConfirmPacket{};

struct PairRxDiagnosticEvent {
    uint8_t sourceMac[6];
    int16_t length;
    uint16_t magic;
    uint8_t version;
    uint8_t packetType;
    bool headerAvailable;
    bool upstreamPairingActive;
    bool childPairingActive;
};

constexpr UBaseType_t PAIR_DIAGNOSTIC_QUEUE_LENGTH = 16;

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

bool macIsConfigured(const uint8_t* mac) {
    return mac != nullptr &&
           (mac[0] != 0 || mac[1] != 0 || mac[2] != 0 ||
            mac[3] != 0 || mac[4] != 0 || mac[5] != 0);
}

String macToString(const uint8_t* mac) {
    if (mac == nullptr) {
        return String("<null>");
    }
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buffer);
}

bool loadStoredBaseMac(uint8_t mac[6]) {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, true)) {
        Serial.println("[PAIR][WARN] Khong mo duoc NVS de doc MAC Base");
        return false;
    }
    const bool ok = preferences.getBytesLength(ESPNOW_NVS_BASE_MAC_KEY) == 6 &&
                    preferences.getBytes(ESPNOW_NVS_BASE_MAC_KEY, mac, 6) == 6 &&
                    macIsConfigured(mac);
    preferences.end();
    return ok;
}

bool saveStoredBaseMac(const uint8_t mac[6]) {
    Preferences preferences;
    if (!preferences.begin(ESPNOW_NVS_NAMESPACE, false)) {
        Serial.println("[PAIR][ERROR] Khong mo duoc NVS de luu MAC Base");
        return false;
    }
    const bool ok = preferences.putBytes(ESPNOW_NVS_BASE_MAC_KEY, mac, 6) == 6;
    preferences.end();
    return ok;
}

void setStoredBaseStat(bool stored) {
    portENTER_CRITICAL(&statsMux);
    stats.hasStoredBaseMac = stored;
    portEXIT_CRITICAL(&statsMux);
}

void loadActiveBaseMac() {
    uint8_t storedMac[6] = {};
    if (loadStoredBaseMac(storedMac)) {
        std::memcpy(activeBaseMac, storedMac, sizeof(activeBaseMac));
        hasActiveBaseMac = true;
        setStoredBaseStat(true);
        Serial.println("[PAIR] Loaded Base MAC from NVS: " + macToString(activeBaseMac));
        return;
    }

    std::memset(activeBaseMac, 0, sizeof(activeBaseMac));
    hasActiveBaseMac = false;
    setStoredBaseStat(false);
    Serial.println("[PAIR] No stored Base MAC; hold pairing button to pair");
}

bool isExpectedRuntimeBase(const uint8_t* mac) {
    return hasActiveBaseMac && mac != nullptr && std::memcmp(mac, activeBaseMac, 6) == 0;
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

bool addPeerForMac(const uint8_t* mac, uint8_t channel, bool encrypted) {
    if (!macIsConfigured(mac)) {
        return false;
    }

    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypted;
    if (peer.encrypt) {
        std::memcpy(peer.lmk, ESPNOW_LMK, sizeof(peer.lmk));
    }

    const esp_err_t result = esp_now_is_peer_exist(mac)
                                 ? esp_now_mod_peer(&peer)
                                 : esp_now_add_peer(&peer);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong cau hinh duoc peer %s: %d\n",
                      macToString(mac).c_str(),
                      result);
        return false;
    }
    return true;
}

bool configurePeer(uint8_t channel) {
    if (!hasActiveBaseMac) {
        return false;
    }
    if (!addPeerForMac(activeBaseMac, channel, ESPNOW_ENCRYPTION_ENABLED)) {
        return false;
    }
    return true;
}

bool configureEspNowRate() {
    if constexpr (!ESPNOW_FORCE_LR_RATE) {
        Serial.println("[ESP-NOW] TX rate=default; LR protocol capability remains enabled");
        return true;
    }
    const wifi_phy_rate_t rate = ESPNOW_USE_LR_250KBPS
                                     ? WIFI_PHY_RATE_LORA_250K
                                     : WIFI_PHY_RATE_LORA_500K;
    const esp_err_t result = esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong dat duoc LR PHY rate: %d\n", result);
        return false;
    }
    return true;
}

const char* espNowTxRateToText() {
    if constexpr (!ESPNOW_FORCE_LR_RATE) {
        return "default";
    }
    return ESPNOW_USE_LR_250KBPS ? "LR-250K" : "LR-500K";
}

bool isPairingActive() {
    portENTER_CRITICAL(&pairingMux);
    const bool active = pairingActive;
    portEXIT_CRITICAL(&pairingMux);
    return active;
}

const char* pairingPacketTypeToText(uint8_t packetType) {
    switch (packetType) {
        case rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY: return "PAIR_DISCOVERY";
        case rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE: return "PAIR_RESPONSE";
        case rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM: return "PAIR_CONFIRM";
        default: return "UNKNOWN";
    }
}

int expectedPairingPacketLength(uint8_t packetType) {
    switch (packetType) {
        case rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY:
            return static_cast<int>(sizeof(rtcm_espnow::PairDiscoveryPacket));
        case rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE:
            return static_cast<int>(sizeof(rtcm_espnow::PairResponsePacket));
        case rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM:
            return static_cast<int>(sizeof(rtcm_espnow::PairConfirmPacket));
        default:
            return -1;
    }
}

void queuePairRxDiagnostic(const uint8_t* sourceMac,
                           const uint8_t* data,
                           int length) {
    if (pairDiagnosticQueue == nullptr) {
        return;
    }

    PairRxDiagnosticEvent event{};
    event.length = static_cast<int16_t>(length);
    if (sourceMac != nullptr) {
        std::memcpy(event.sourceMac, sourceMac, sizeof(event.sourceMac));
    }
    event.upstreamPairingActive = isPairingActive();
    if constexpr (ROVER_RELAY_MODE) {
        event.childPairingActive = relayIsChildPairingActive();
    }

    if (data != nullptr &&
        length >= static_cast<int>(sizeof(rtcm_espnow::EspNowCommonHeader))) {
        rtcm_espnow::EspNowCommonHeader common{};
        std::memcpy(&common, data, sizeof(common));
        event.headerAvailable = true;
        event.magic = common.magic;
        event.version = common.version;
        event.packetType = common.packetType;
        if (expectedPairingPacketLength(common.packetType) < 0) {
            return;
        }
    } else if (!event.upstreamPairingActive && !event.childPairingActive) {
        return;
    }

    xQueueSend(pairDiagnosticQueue, &event, 0);
}

void drainPairRxDiagnostics() {
    if (pairDiagnosticQueue == nullptr) {
        return;
    }

    PairRxDiagnosticEvent event{};
    while (xQueueReceive(pairDiagnosticQueue, &event, 0) == pdTRUE) {
        const char* prefix =
            ROVER_RELAY_MODE && event.headerAvailable &&
                    event.packetType == rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE
                ? "[RELAY][PAIR][RX_CB]"
                : "[PAIR][RX_CB]";
        if (!event.headerAvailable) {
            Serial.printf("%s src=%s len=%d header=missing upstream_pairing=%u child_pairing=%u\n",
                          prefix,
                          macToString(event.sourceMac).c_str(),
                          static_cast<int>(event.length),
                          event.upstreamPairingActive ? 1U : 0U,
                          event.childPairingActive ? 1U : 0U);
            continue;
        }

        const int expectedLength = expectedPairingPacketLength(event.packetType);
        Serial.printf("%s src=%s len=%d expected_len=%d length=%s magic=0x%04X(%s) version=%u(%s) type=%s(%u) upstream_pairing=%u child_pairing=%u\n",
                      prefix,
                      macToString(event.sourceMac).c_str(),
                      static_cast<int>(event.length),
                      expectedLength,
                      event.length == expectedLength ? "ok" : "bad",
                      static_cast<unsigned>(event.magic),
                      event.magic == rtcm_espnow::MAGIC ? "ok" : "bad",
                      static_cast<unsigned>(event.version),
                      event.version == rtcm_espnow::VERSION ? "ok" : "bad",
                      pairingPacketTypeToText(event.packetType),
                      static_cast<unsigned>(event.packetType),
                      event.upstreamPairingActive ? 1U : 0U,
                      event.childPairingActive ? 1U : 0U);
    }
}

void updatePairingActiveStat(bool active) {
    portENTER_CRITICAL(&statsMux);
    stats.pairingActive = active;
    portEXIT_CRITICAL(&statsMux);
}

void startPairingMode() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&pairingMux);
    pairingActive = true;
    awaitingPairConfirm = false;
    pairingEndsAtMs = now + PAIRING_WINDOW_MS;
    pendingBaseNonce = 0;
    pendingRoverNonce = 0;
    std::memset(pendingBaseMac, 0, sizeof(pendingBaseMac));
    portEXIT_CRITICAL(&pairingMux);
    updatePairingActiveStat(true);
    Serial.printf("[PAIR] Rover pairing mode ON for %lu ms, network_id=0x%08lX\n",
                  static_cast<unsigned long>(PAIRING_WINDOW_MS),
                  static_cast<unsigned long>(ESPNOW_NETWORK_ID));
}

void stopPairingMode(const char* reason) {
    portENTER_CRITICAL(&pairingMux);
    pairingActive = false;
    awaitingPairConfirm = false;
    portEXIT_CRITICAL(&pairingMux);
    updatePairingActiveStat(false);
    Serial.print("[PAIR] Rover pairing mode OFF");
    if (reason != nullptr) {
        Serial.print(": ");
        Serial.print(reason);
    }
    Serial.println();
}

void queuePairSave(const uint8_t* baseMac) {
    portENTER_CRITICAL(&pairingMux);
    std::memcpy(confirmedBaseMac, baseMac, sizeof(confirmedBaseMac));
    pendingPairSave = true;
    pairingActive = false;
    awaitingPairConfirm = false;
    portEXIT_CRITICAL(&pairingMux);
    updatePairingActiveStat(false);
}

bool handlePairDiscovery(const uint8_t* sourceMac, const uint8_t* data, int length) {
    if (!ESPNOW_PAIRING_ENABLED || !isPairingActive() ||
        sourceMac == nullptr || data == nullptr ||
        length != static_cast<int>(sizeof(rtcm_espnow::PairDiscoveryPacket))) {
        return false;
    }

    portENTER_CRITICAL(&pairingMux);
    std::memcpy(&pendingPairDiscoveryPacket, data, sizeof(pendingPairDiscoveryPacket));
    std::memcpy(pendingPairDiscoveryMac, sourceMac, sizeof(pendingPairDiscoveryMac));
    pendingPairDiscovery = true;
    portEXIT_CRITICAL(&pairingMux);
    return true;
}

bool handlePairConfirm(const uint8_t* sourceMac, const uint8_t* data, int length) {
    if (!ESPNOW_PAIRING_ENABLED || !isPairingActive() ||
        sourceMac == nullptr || data == nullptr ||
        length != static_cast<int>(sizeof(rtcm_espnow::PairConfirmPacket))) {
        return false;
    }

    uint8_t expectedBaseMac[6] = {};
    bool expectingConfirm = false;
    portENTER_CRITICAL(&pairingMux);
    expectingConfirm = awaitingPairConfirm;
    std::memcpy(expectedBaseMac, pendingBaseMac, sizeof(expectedBaseMac));
    portEXIT_CRITICAL(&pairingMux);

    if (!expectingConfirm || std::memcmp(sourceMac, expectedBaseMac, 6) != 0) {
        return true;
    }

    portENTER_CRITICAL(&pairingMux);
    std::memcpy(&pendingPairConfirmPacket, data, sizeof(pendingPairConfirmPacket));
    std::memcpy(pendingPairConfirmMac, sourceMac, sizeof(pendingPairConfirmMac));
    pendingPairConfirm = true;
    portEXIT_CRITICAL(&pairingMux);
    return true;
}

void handleReceivedPacket(const uint8_t* sourceMac, const uint8_t* data, int length) {
    queuePairRxDiagnostic(sourceMac, data, length);
    if (data == nullptr ||
        length < static_cast<int>(sizeof(rtcm_espnow::EspNowCommonHeader)) ||
        length > static_cast<int>(rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE)) {
        updateCounter(&EspNowRtcmStats::packetsInvalidHeader);
        return;
    }

    rtcm_espnow::EspNowCommonHeader common{};
    std::memcpy(&common, data, sizeof(common));
    if (common.magic != rtcm_espnow::MAGIC || common.version != rtcm_espnow::VERSION) {
        updateCounter(&EspNowRtcmStats::packetsInvalidHeader);
        return;
    }

    if constexpr (ROVER_RELAY_MODE) {
        if (relayHandleReceivedPacket(sourceMac, data, length)) {
            return;
        }
    }

    if (common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY) {
        if (handlePairDiscovery(sourceMac, data, length)) {
            return;
        }
    } else if (common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM) {
        if (handlePairConfirm(sourceMac, data, length)) {
            return;
        }
    }

    if (!isExpectedRuntimeBase(sourceMac)) {
        updateCounter(&EspNowRtcmStats::packetsWrongSource);
        return;
    }
    if (length < static_cast<int>(sizeof(rtcm_espnow::RtcmEspNowHeader))) {
        updateCounter(&EspNowRtcmStats::packetsInvalidHeader);
        return;
    }

    rtcm_espnow::RtcmEspNowHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (!rtcm_espnow::validatePacketHeader(header, static_cast<std::size_t>(length))) {
        espnowRecordInvalidHeader();
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

void setupPairingButton() {
    if (!ESPNOW_PAIRING_ENABLED || pairingButtonReady) {
        return;
    }
    pinMode(PAIRING_BUTTON_PIN, PAIRING_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
    pairingButtonReady = true;
    Serial.printf("[PAIR] Pairing button GPIO=%d active_%s hold_ms=%lu\n",
                  PAIRING_BUTTON_PIN,
                  PAIRING_BUTTON_ACTIVE_LOW ? "low" : "high",
                  static_cast<unsigned long>(PAIRING_BUTTON_HOLD_MS));
}

void commitPendingPairSave() {
    uint8_t macToSave[6] = {};
    bool shouldSave = false;
    portENTER_CRITICAL(&pairingMux);
    shouldSave = pendingPairSave;
    if (shouldSave) {
        std::memcpy(macToSave, confirmedBaseMac, sizeof(macToSave));
        pendingPairSave = false;
    }
    portEXIT_CRITICAL(&pairingMux);

    if (!shouldSave) {
        return;
    }

    if (!saveStoredBaseMac(macToSave)) {
        Serial.println("[PAIR][ERROR] Luu MAC Base vao NVS that bai");
        return;
    }

    std::memcpy(activeBaseMac, macToSave, sizeof(activeBaseMac));
    hasActiveBaseMac = true;
    setStoredBaseStat(true);
    if (!configurePeer(getWiFiChannel())) {
        Serial.println("[PAIR][ERROR] Cau hinh peer Base sau pairing that bai");
        return;
    }
    Serial.println("[PAIR] Saved Base MAC to NVS and switched runtime peer to " + macToString(activeBaseMac));
}

void processPendingPairDiscovery() {
    rtcm_espnow::PairDiscoveryPacket discovery{};
    uint8_t sourceMac[6] = {};
    bool hasPending = false;
    portENTER_CRITICAL(&pairingMux);
    hasPending = pendingPairDiscovery;
    if (hasPending) {
        discovery = pendingPairDiscoveryPacket;
        std::memcpy(sourceMac, pendingPairDiscoveryMac, sizeof(sourceMac));
        pendingPairDiscovery = false;
    }
    portEXIT_CRITICAL(&pairingMux);

    if (!hasPending) {
        return;
    }

    Serial.printf("[PAIR][DIAG] Process PAIR_DISCOVERY src=%s len=%u\n",
                  macToString(sourceMac).c_str(),
                  static_cast<unsigned>(sizeof(discovery)));

    const uint32_t expectedAuth = rtcm_espnow::pairingAuthTag(
        discovery, ESPNOW_PAIRING_KEY, sizeof(ESPNOW_PAIRING_KEY));
    if (!rtcm_espnow::validatePairDiscovery(discovery,
                                            sizeof(discovery),
                                            ESPNOW_NETWORK_ID,
                                            ESPNOW_PAIRING_KEY,
                                            sizeof(ESPNOW_PAIRING_KEY))) {
        updateCounter(&EspNowRtcmStats::pairAuthFailures);
        Serial.printf("[PAIR][DIAG][REJECT] PAIR_DISCOVERY magic=%s version=%s type=%s role=%s network=%s auth=%s received_auth=0x%08lX expected_auth=0x%08lX\n",
                      discovery.common.magic == rtcm_espnow::MAGIC ? "ok" : "bad",
                      discovery.common.version == rtcm_espnow::VERSION ? "ok" : "bad",
                      discovery.common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY ? "ok" : "bad",
                      discovery.role == rtcm_espnow::ROLE_BASE ? "ok" : "bad",
                      discovery.networkId == ESPNOW_NETWORK_ID ? "ok" : "bad",
                      discovery.authTag == expectedAuth ? "ok" : "bad",
                      static_cast<unsigned long>(discovery.authTag),
                      static_cast<unsigned long>(expectedAuth));
        return;
    }
    Serial.println("[PAIR][DIAG] PAIR_DISCOVERY validation=ok");

    rtcm_espnow::PairResponsePacket response{};
    response.common.magic = rtcm_espnow::MAGIC;
    response.common.version = rtcm_espnow::VERSION;
    response.common.packetType = rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE;
    response.role = rtcm_espnow::ROLE_ROVER;
    response.networkId = ESPNOW_NETWORK_ID;
    response.roverDeviceId = localDeviceId();
    response.roverNonce = esp_random();
    response.baseNonceEcho = discovery.baseNonce;
    response.authTag = rtcm_espnow::pairingAuthTag(response,
                                                   ESPNOW_PAIRING_KEY,
                                                   sizeof(ESPNOW_PAIRING_KEY));

    if (!addPeerForMac(sourceMac, getWiFiChannel(), false)) {
        Serial.println("[PAIR][ERROR] Khong them duoc Base peer tam thoi");
        return;
    }
    Serial.printf("[PAIR][DIAG] Temporary Base peer add=ok mac=%s channel=%u\n",
                  macToString(sourceMac).c_str(),
                  static_cast<unsigned>(getWiFiChannel()));

    Serial.println("[PAIR][DIAG] Calling TX PAIR_RESPONSE");
    const EspNowTxResult result = espnowTxSend(
        sourceMac,
        reinterpret_cast<const uint8_t*>(&response),
        sizeof(response));
    if (result != EspNowTxResult::Success) {
        Serial.printf("[PAIR][ERROR] Gui PAIR_RESPONSE that bai: %s\n",
                      espnowTxResultToString(result));
        return;
    }

    portENTER_CRITICAL(&pairingMux);
    awaitingPairConfirm = true;
    pendingBaseNonce = discovery.baseNonce;
    pendingRoverNonce = response.roverNonce;
    std::memcpy(pendingBaseMac, sourceMac, sizeof(pendingBaseMac));
    portEXIT_CRITICAL(&pairingMux);

    updateCounter(&EspNowRtcmStats::pairDiscoveryReceived);
    updateCounter(&EspNowRtcmStats::pairResponsesSent);
    Serial.println("[PAIR] PAIR_DISCOVERY hop le, da gui PAIR_RESPONSE toi " + macToString(sourceMac));
}

void processPendingPairConfirm() {
    rtcm_espnow::PairConfirmPacket confirm{};
    uint8_t sourceMac[6] = {};
    bool hasPending = false;
    portENTER_CRITICAL(&pairingMux);
    hasPending = pendingPairConfirm;
    if (hasPending) {
        confirm = pendingPairConfirmPacket;
        std::memcpy(sourceMac, pendingPairConfirmMac, sizeof(sourceMac));
        pendingPairConfirm = false;
    }
    portEXIT_CRITICAL(&pairingMux);

    if (!hasPending) {
        return;
    }

    Serial.printf("[PAIR][DIAG] Process PAIR_CONFIRM src=%s len=%u\n",
                  macToString(sourceMac).c_str(),
                  static_cast<unsigned>(sizeof(confirm)));

    uint32_t expectedBaseNonce = 0;
    uint32_t expectedRoverNonce = 0;
    portENTER_CRITICAL(&pairingMux);
    expectedBaseNonce = pendingBaseNonce;
    expectedRoverNonce = pendingRoverNonce;
    portEXIT_CRITICAL(&pairingMux);

    const uint32_t expectedAuth = rtcm_espnow::pairingAuthTag(
        confirm, ESPNOW_PAIRING_KEY, sizeof(ESPNOW_PAIRING_KEY));
    if (!rtcm_espnow::validatePairConfirm(confirm,
                                          sizeof(confirm),
                                          ESPNOW_NETWORK_ID,
                                          expectedBaseNonce,
                                          expectedRoverNonce,
                                          ESPNOW_PAIRING_KEY,
                                          sizeof(ESPNOW_PAIRING_KEY))) {
        updateCounter(&EspNowRtcmStats::pairAuthFailures);
        Serial.printf("[PAIR][DIAG][REJECT] PAIR_CONFIRM magic=%s version=%s type=%s role=%s network=%s base_nonce=%s rover_nonce=%s auth=%s received_auth=0x%08lX expected_auth=0x%08lX\n",
                      confirm.common.magic == rtcm_espnow::MAGIC ? "ok" : "bad",
                      confirm.common.version == rtcm_espnow::VERSION ? "ok" : "bad",
                      confirm.common.packetType == rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM ? "ok" : "bad",
                      confirm.role == rtcm_espnow::ROLE_BASE ? "ok" : "bad",
                      confirm.networkId == ESPNOW_NETWORK_ID ? "ok" : "bad",
                      confirm.baseNonce == expectedBaseNonce ? "ok" : "bad",
                      confirm.roverNonce == expectedRoverNonce ? "ok" : "bad",
                      confirm.authTag == expectedAuth ? "ok" : "bad",
                      static_cast<unsigned long>(confirm.authTag),
                      static_cast<unsigned long>(expectedAuth));
        return;
    }
    Serial.println("[PAIR][DIAG] PAIR_CONFIRM validation=ok");

    queuePairSave(sourceMac);
    updateCounter(&EspNowRtcmStats::pairConfirmsAccepted);
    Serial.println("[PAIR] PAIR_CONFIRM hop le tu Base " + macToString(sourceMac));
}

} // namespace

bool espnowPrepare() {
    if (coreReady) {
        return true;
    }
    if constexpr (WIFI_CONNECT_TO_ROUTER_ENABLED) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[ESP-NOW][ERROR] Wi-Fi STA chua ket noi router/AP");
            return false;
        }
    }
    if (!wifiRadioIsReady()) {
        Serial.println("[ESP-NOW][ERROR] Wi-Fi radio chua duoc cau hinh");
        return false;
    }
    Serial.printf("[ESP-NOW] Reuse preconfigured Wi-Fi radio, channel=%u\n",
                  getWiFiChannel());

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
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong dang ky duoc receiver: %d\n", result);
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if (!configureEspNowRate()) {
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if (!espnowTxSetup()) {
        Serial.println("[ESP-NOW][ERROR] Khong khoi tao duoc shared TX manager");
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    pairDiagnosticQueue = xQueueCreate(PAIR_DIAGNOSTIC_QUEUE_LENGTH,
                                       sizeof(PairRxDiagnosticEvent));
    if (pairDiagnosticQueue == nullptr) {
        Serial.println("[PAIR][DIAG][WARN] Khong tao duoc RX diagnostic queue");
    } else {
        Serial.println("[PAIR][DIAG] RX callback diagnostic queue ready");
    }
    coreReady = true;
    Serial.printf("[ESP-NOW] Core ready, TX rate=%s\n", espNowTxRateToText());
    return true;
}

bool espnowSetup() {
    if (ready) {
        return true;
    }
    if (!coreReady) {
        Serial.println("[ESP-NOW][ERROR] Core/rate chua duoc khoi tao truoc SoftAP");
        return false;
    }

    setupPairingButton();
    loadActiveBaseMac();
    if (!hasActiveBaseMac && !ESPNOW_PAIRING_ENABLED) {
        Serial.println("[ESP-NOW][ERROR] Chua co MAC Base va pairing dang tat");
        return false;
    }
    if (!hasActiveBaseMac) {
        Serial.println("[ESP-NOW][WARN] Chua co MAC Base; chi cho pairing mode");
    }
    if (hasActiveBaseMac && !configurePeer(getWiFiChannel())) {
        return false;
    }

    ready = true;
    Serial.printf("[ESP-NOW] Ready, STA channel=%u, TX rate=%s, peer=%s\n",
                  getWiFiChannel(),
                  espNowTxRateToText(),
                  hasActiveBaseMac ? macToString(activeBaseMac).c_str() : "<none>");
    return true;
}

bool espnowRefreshPeerChannel() {
    if (!ready || !hasActiveBaseMac) {
        return false;
    }
    return configureWiFiForEspNowLongRange() && configurePeer(getWiFiChannel());
}

bool espnowIsReady() {
    return ready;
}

void espnowLoop() {
    if (!ready) {
        return;
    }

    drainPairRxDiagnostics();
    commitPendingPairSave();
    processPendingPairDiscovery();
    processPendingPairConfirm();

    if constexpr (ESPNOW_PAIRING_ENABLED) {
        static uint32_t pressedSinceMs = 0;
        static bool pairingStartHandled = false;
        const bool rawLevel = digitalRead(PAIRING_BUTTON_PIN) == HIGH;
        const bool pressed = PAIRING_BUTTON_ACTIVE_LOW ? !rawLevel : rawLevel;
        const uint32_t now = millis();

        const bool upstreamButtonOwnsPress = !ROVER_RELAY_MODE || !hasActiveBaseMac;
        if (pressed && upstreamButtonOwnsPress) {
            if (pressedSinceMs == 0) {
                pressedSinceMs = now;
            } else if (!pairingStartHandled &&
                       now - pressedSinceMs >= PAIRING_BUTTON_HOLD_MS) {
                startPairingMode();
                pairingStartHandled = true;
            }
        } else if (!pressed) {
            pressedSinceMs = 0;
            pairingStartHandled = false;
        }

        bool active = false;
        uint32_t endsAt = 0;
        portENTER_CRITICAL(&pairingMux);
        active = pairingActive;
        endsAt = pairingEndsAtMs;
        portEXIT_CRITICAL(&pairingMux);
        if (active && static_cast<int32_t>(now - endsAt) >= 0) {
            stopPairingMode("timeout");
        }
    }
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

bool espnowGetBaseMac(uint8_t mac[6]) {
    if (mac == nullptr || !hasActiveBaseMac) {
        return false;
    }
    std::memcpy(mac, activeBaseMac, sizeof(activeBaseMac));
    return true;
}

bool espnowSendFrameAck(uint16_t streamId, uint32_t frameSequence) {
    if (!ready || !hasActiveBaseMac) {
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

    const EspNowTxResult result = espnowTxSend(
        activeBaseMac,
        reinterpret_cast<const uint8_t*>(&ack),
        sizeof(ack));
    if (result != EspNowTxResult::Success) {
        updateCounter(&EspNowRtcmStats::ackSendFailures);
        return false;
    }
    updateCounter(&EspNowRtcmStats::ackPacketsQueued);
    return true;
}

bool espnowTrySendRoverLlhStatus(double latitude,
                                 double longitude,
                                 double heightM) {
    static uint32_t statusSequence = 0;
    if (!ready || !hasActiveBaseMac || ROVER_RELAY_MODE) {
        updateCounter(&EspNowRtcmStats::llhStatusSkipped);
        return false;
    }

    constexpr double minHeightM =
        static_cast<double>(std::numeric_limits<int32_t>::min()) /
        rtcm_espnow::LLH_HEIGHT_SCALE;
    constexpr double maxHeightM =
        static_cast<double>(std::numeric_limits<int32_t>::max()) /
        rtcm_espnow::LLH_HEIGHT_SCALE;
    if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
        !std::isfinite(heightM) || latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0 ||
        heightM < minHeightM || heightM > maxHeightM) {
        updateCounter(&EspNowRtcmStats::llhStatusFailures);
        return false;
    }

    rtcm_espnow::RoverLlhStatusPacket packet{};
    packet.common.magic = rtcm_espnow::MAGIC;
    packet.common.version = rtcm_espnow::VERSION;
    packet.common.packetType = rtcm_espnow::PACKET_TYPE_ROVER_LLH_STATUS;
    packet.sequence = statusSequence++;
    packet.latitudeE7 = static_cast<int32_t>(
        std::llround(latitude * rtcm_espnow::LLH_COORDINATE_SCALE));
    packet.longitudeE7 = static_cast<int32_t>(
        std::llround(longitude * rtcm_espnow::LLH_COORDINATE_SCALE));
    packet.heightMm = static_cast<int32_t>(
        std::llround(heightM * rtcm_espnow::LLH_HEIGHT_SCALE));
    if (!rtcm_espnow::validateRoverLlhStatus(packet, sizeof(packet))) {
        updateCounter(&EspNowRtcmStats::llhStatusFailures);
        return false;
    }

    const EspNowTxResult result = espnowTxTrySend(
        activeBaseMac,
        reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet));
    if (result == EspNowTxResult::Busy) {
        updateCounter(&EspNowRtcmStats::llhStatusSkipped);
        return false;
    }
    if (result != EspNowTxResult::Success) {
        updateCounter(&EspNowRtcmStats::llhStatusFailures);
        Serial.printf("[ROVER][LLH_TX][WARN] seq=%lu result=%s\n",
                      static_cast<unsigned long>(packet.sequence),
                      espnowTxResultToString(result));
        return false;
    }

    updateCounter(&EspNowRtcmStats::llhStatusSent);
    Serial.printf("[ROVER][LLH_TX] seq=%lu lat=%.7f lon=%.7f height_m=%.3f\n",
                  static_cast<unsigned long>(packet.sequence),
                  static_cast<double>(packet.latitudeE7) /
                      rtcm_espnow::LLH_COORDINATE_SCALE,
                  static_cast<double>(packet.longitudeE7) /
                      rtcm_espnow::LLH_COORDINATE_SCALE,
                  static_cast<double>(packet.heightMm) /
                      rtcm_espnow::LLH_HEIGHT_SCALE);
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
