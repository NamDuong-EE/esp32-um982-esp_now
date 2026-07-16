#include "hardware/Espnow_tx_manager.h"

#include <Arduino.h>
#include <cstring>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#include "Prog_Config.h"
#include "protocol/RtcmEspNowProtocol.h"

namespace {

SemaphoreHandle_t txMutex = nullptr;
SemaphoreHandle_t callbackSemaphore = nullptr;
portMUX_TYPE txStateMux = portMUX_INITIALIZER_UNLOCKED;
bool txReady = false;
bool callbackPending = false;
bool callbackSucceeded = false;
uint8_t pendingDestinationMac[6] = {};

const char* pairingPacketTypeToText(uint8_t packetType) {
    switch (packetType) {
        case rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY: return "PAIR_DISCOVERY";
        case rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE: return "PAIR_RESPONSE";
        case rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM: return "PAIR_CONFIRM";
        default: return "not_pairing";
    }
}

bool getPairingPacketType(const uint8_t* data,
                          std::size_t length,
                          uint8_t& packetType) {
    if (data == nullptr || length < sizeof(rtcm_espnow::EspNowCommonHeader)) {
        return false;
    }
    rtcm_espnow::EspNowCommonHeader common{};
    std::memcpy(&common, data, sizeof(common));
    packetType = common.packetType;
    return packetType == rtcm_espnow::PACKET_TYPE_PAIR_DISCOVERY ||
           packetType == rtcm_espnow::PACKET_TYPE_PAIR_RESPONSE ||
           packetType == rtcm_espnow::PACKET_TYPE_PAIR_CONFIRM;
}

void logPairingTx(uint8_t packetType,
                  const uint8_t destinationMac[6],
                  const char* result) {
    Serial.printf("[ESP-NOW][TX_DIAG] type=%s(%u) dst=%02X:%02X:%02X:%02X:%02X:%02X %s\n",
                  pairingPacketTypeToText(packetType),
                  static_cast<unsigned>(packetType),
                  destinationMac[0], destinationMac[1], destinationMac[2],
                  destinationMac[3], destinationMac[4], destinationMac[5],
                  result);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
    const uint8_t* destinationMac = info == nullptr ? nullptr : info->des_addr;
#else
void onDataSent(const uint8_t* destinationMac, esp_now_send_status_t status) {
#endif
    bool matches = false;
    portENTER_CRITICAL(&txStateMux);
    matches = callbackPending && destinationMac != nullptr &&
              std::memcmp(destinationMac, pendingDestinationMac, 6) == 0;
    if (matches) {
        callbackSucceeded = status == ESP_NOW_SEND_SUCCESS;
        callbackPending = false;
    }
    portEXIT_CRITICAL(&txStateMux);

    if (matches && callbackSemaphore != nullptr) {
        xSemaphoreGive(callbackSemaphore);
    }
}

bool waitForPreviousCallback() {
    bool pending = false;
    portENTER_CRITICAL(&txStateMux);
    pending = callbackPending;
    portEXIT_CRITICAL(&txStateMux);
    if (!pending) {
        return true;
    }

    xSemaphoreTake(callbackSemaphore, pdMS_TO_TICKS(ESPNOW_TX_CALLBACK_TIMEOUT_MS));
    portENTER_CRITICAL(&txStateMux);
    pending = callbackPending;
    portEXIT_CRITICAL(&txStateMux);
    return !pending;
}

bool hasPendingCallback() {
    portENTER_CRITICAL(&txStateMux);
    const bool pending = callbackPending;
    portEXIT_CRITICAL(&txStateMux);
    return pending;
}

} // namespace

bool espnowTxSetup() {
    if (txReady) {
        return true;
    }

    txMutex = xSemaphoreCreateMutex();
    callbackSemaphore = xSemaphoreCreateBinary();
    if (txMutex == nullptr || callbackSemaphore == nullptr) {
        if (txMutex != nullptr) {
            vSemaphoreDelete(txMutex);
            txMutex = nullptr;
        }
        if (callbackSemaphore != nullptr) {
            vSemaphoreDelete(callbackSemaphore);
            callbackSemaphore = nullptr;
        }
        return false;
    }

    const esp_err_t result = esp_now_register_send_cb(onDataSent);
    if (result != ESP_OK) {
        vSemaphoreDelete(txMutex);
        vSemaphoreDelete(callbackSemaphore);
        txMutex = nullptr;
        callbackSemaphore = nullptr;
        return false;
    }

    txReady = true;
    Serial.println("[ESP-NOW][TX] Shared TX manager ready");
    return true;
}

EspNowTxResult sendInternal(const uint8_t destinationMac[6],
                            const uint8_t* data,
                            std::size_t length,
                            bool lowPriorityTry) {
    uint8_t pairingPacketType = 0;
    const bool pairingPacket = getPairingPacketType(data, length, pairingPacketType);
    if (!txReady || txMutex == nullptr || callbackSemaphore == nullptr) {
        if (pairingPacket && destinationMac != nullptr) {
            logPairingTx(pairingPacketType, destinationMac, "manager=not_ready");
        }
        return EspNowTxResult::NotReady;
    }
    if (destinationMac == nullptr || data == nullptr || length == 0 ||
        length > rtcm_espnow::ESPNOW_V1_MAX_PACKET_SIZE) {
        return EspNowTxResult::InvalidArgument;
    }
    const TickType_t mutexWait = lowPriorityTry
                                     ? 0
                                     : pdMS_TO_TICKS(ESPNOW_TX_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(txMutex, mutexWait) != pdTRUE) {
        if (pairingPacket) {
            logPairingTx(pairingPacketType, destinationMac, "mutex=timeout");
        }
        return lowPriorityTry ? EspNowTxResult::Busy : EspNowTxResult::MutexTimeout;
    }

    if (lowPriorityTry && hasPendingCallback()) {
        xSemaphoreGive(txMutex);
        return EspNowTxResult::Busy;
    }
    if (!lowPriorityTry && !waitForPreviousCallback()) {
        if (pairingPacket) {
            logPairingTx(pairingPacketType, destinationMac,
                         "previous_callback=still_pending timeout");
        }
        xSemaphoreGive(txMutex);
        return EspNowTxResult::CallbackTimeout;
    }
    while (xSemaphoreTake(callbackSemaphore, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&txStateMux);
    std::memcpy(pendingDestinationMac, destinationMac, 6);
    callbackSucceeded = false;
    callbackPending = true;
    portEXIT_CRITICAL(&txStateMux);

    const esp_err_t queued = esp_now_send(destinationMac, data, length);
    if (queued != ESP_OK) {
        portENTER_CRITICAL(&txStateMux);
        callbackPending = false;
        portEXIT_CRITICAL(&txStateMux);
        if (pairingPacket) {
            char resultText[48] = {};
            snprintf(resultText, sizeof(resultText),
                     "esp_now_send=%d queued=fail", static_cast<int>(queued));
            logPairingTx(pairingPacketType, destinationMac, resultText);
        }
        xSemaphoreGive(txMutex);
        return EspNowTxResult::QueueError;
    }

    const BaseType_t callbackReceived = xSemaphoreTake(
        callbackSemaphore,
        pdMS_TO_TICKS(ESPNOW_TX_CALLBACK_TIMEOUT_MS));
    if (callbackReceived != pdTRUE) {
        // Keep callbackPending set. The next sender must wait for this callback
        // instead of misattributing a late callback to a newer packet.
        if (pairingPacket) {
            logPairingTx(pairingPacketType, destinationMac,
                         "esp_now_send=0 queued=ok callback=timeout");
        }
        xSemaphoreGive(txMutex);
        return EspNowTxResult::CallbackTimeout;
    }

    portENTER_CRITICAL(&txStateMux);
    const bool succeeded = callbackSucceeded;
    portEXIT_CRITICAL(&txStateMux);
    if (pairingPacket) {
        logPairingTx(pairingPacketType, destinationMac,
                     succeeded
                         ? "esp_now_send=0 queued=ok callback=received status=SUCCESS"
                         : "esp_now_send=0 queued=ok callback=received status=FAIL");
    }
    xSemaphoreGive(txMutex);
    return succeeded ? EspNowTxResult::Success : EspNowTxResult::DeliveryFailed;
}

EspNowTxResult espnowTxSend(const uint8_t destinationMac[6],
                            const uint8_t* data,
                            std::size_t length) {
    return sendInternal(destinationMac, data, length, false);
}

EspNowTxResult espnowTxTrySend(const uint8_t destinationMac[6],
                               const uint8_t* data,
                               std::size_t length) {
    return sendInternal(destinationMac, data, length, true);
}

const char* espnowTxResultToString(EspNowTxResult result) {
    switch (result) {
        case EspNowTxResult::Success: return "success";
        case EspNowTxResult::NotReady: return "not_ready";
        case EspNowTxResult::InvalidArgument: return "invalid_argument";
        case EspNowTxResult::Busy: return "busy";
        case EspNowTxResult::MutexTimeout: return "mutex_timeout";
        case EspNowTxResult::QueueError: return "queue_error";
        case EspNowTxResult::CallbackTimeout: return "callback_timeout";
        case EspNowTxResult::DeliveryFailed: return "delivery_failed";
        default: return "unknown";
    }
}
