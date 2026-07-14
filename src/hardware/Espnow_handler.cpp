#include "hardware/Espnow_handler.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <cstddef>
#include <cstring>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#include "Prog_Config.h"
#include "hardware/Relay_handler.h"
#include "hardware/Wifi_handler.h"

namespace {

QueueHandle_t receiveQueue = nullptr;
bool ready = false;
bool rssiMonitorReady = false;
EspNowRtcmStats stats{};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE pairingMux = portMUX_INITIALIZER_UNLOCKED;

constexpr std::size_t IEEE80211_ADDR2_OFFSET = 10;
constexpr std::size_t IEEE80211_MIN_ADDR2_LENGTH = IEEE80211_ADDR2_OFFSET + 6;

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
    if (isExpectedRuntimeBase(sourceMac)) {
        recordRssi(static_cast<int8_t>(packet->rx_ctrl.rssi));
    } else if constexpr (ROVER_RELAY_MODE) {
        relayRecordChildRssi(sourceMac, static_cast<int8_t>(packet->rx_ctrl.rssi));
    }
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

bool isPairingActive() {
    portENTER_CRITICAL(&pairingMux);
    const bool active = pairingActive;
    portEXIT_CRITICAL(&pairingMux);
    return active;
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

void setupRssiMonitor() {
    if (rssiMonitorReady) {
        return;
    }
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

    rssiMonitorReady = true;
    Serial.println("[ESP-NOW] RSSI monitor enabled for Base MAC");
}

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
    if constexpr (DEBUG_WEB_ENABLED && ESPNOW_RSSI_MONITOR_ENABLED) {
        setupRssiMonitor();
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

    if (!rtcm_espnow::validatePairDiscovery(discovery,
                                            sizeof(discovery),
                                            ESPNOW_NETWORK_ID,
                                            ESPNOW_PAIRING_KEY,
                                            sizeof(ESPNOW_PAIRING_KEY))) {
        updateCounter(&EspNowRtcmStats::pairAuthFailures);
        return;
    }

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

    const esp_err_t result = esp_now_send(sourceMac,
                                         reinterpret_cast<const uint8_t*>(&response),
                                         sizeof(response));
    if (result != ESP_OK) {
        Serial.printf("[PAIR][ERROR] Gui PAIR_RESPONSE that bai: %d\n", result);
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

    uint32_t expectedBaseNonce = 0;
    uint32_t expectedRoverNonce = 0;
    portENTER_CRITICAL(&pairingMux);
    expectedBaseNonce = pendingBaseNonce;
    expectedRoverNonce = pendingRoverNonce;
    portEXIT_CRITICAL(&pairingMux);

    if (!rtcm_espnow::validatePairConfirm(confirm,
                                          sizeof(confirm),
                                          ESPNOW_NETWORK_ID,
                                          expectedBaseNonce,
                                          expectedRoverNonce,
                                          ESPNOW_PAIRING_KEY,
                                          sizeof(ESPNOW_PAIRING_KEY))) {
        updateCounter(&EspNowRtcmStats::pairAuthFailures);
        return;
    }

    queuePairSave(sourceMac);
    updateCounter(&EspNowRtcmStats::pairConfirmsAccepted);
    Serial.println("[PAIR] PAIR_CONFIRM hop le tu Base " + macToString(sourceMac));
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

    setupPairingButton();
    loadActiveBaseMac();
    if (!hasActiveBaseMac && !ESPNOW_PAIRING_ENABLED) {
        Serial.println("[ESP-NOW][ERROR] Chua co MAC Base va pairing dang tat");
        return false;
    }
    if (!hasActiveBaseMac) {
        Serial.println("[ESP-NOW][WARN] Chua co MAC Base; chi cho pairing mode");
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
    if (result != ESP_OK) {
        Serial.printf("[ESP-NOW][ERROR] Khong dang ky duoc receiver: %d\n", result);
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if (hasActiveBaseMac && !configurePeer(getWiFiChannel())) {
        esp_now_deinit();
        vQueueDelete(receiveQueue);
        receiveQueue = nullptr;
        return false;
    }
    if constexpr (DEBUG_WEB_ENABLED && ESPNOW_RSSI_MONITOR_ENABLED) {
        if (hasActiveBaseMac) {
            setupRssiMonitor();
        } else {
            Serial.println("[ESP-NOW] RSSI monitor deferred until pairing completes");
        }
    } else if constexpr (DEBUG_WEB_ENABLED) {
        Serial.println("[ESP-NOW] RSSI monitor disabled while debug web is enabled");
    }

    ready = true;
    Serial.printf("[ESP-NOW] Ready, STA channel=%u, LR=%u Kbps, peer=%s\n",
                  getWiFiChannel(),
                  ESPNOW_USE_LR_250KBPS ? 250U : 500U,
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

    const esp_err_t result = esp_now_send(
        activeBaseMac,
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
