#include "functions/Gnss_Command_Handler.h"
#include "functions/Gnss_Command_Verifier.h"

#include <cstring>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "Prog_Config.h"
#include "hardware/Espnow_tx_manager.h"
#include "hardware/Relay_handler.h"
#include "hardware/TemporaryBaseUplink.h"
#include "helper.h"
#include "protocol/RtcmEspNowProtocol.h"

extern SemaphoreHandle_t gnssTxMutex;

namespace {
struct QueuedGnssCommand {
    uint8_t sourceMac[6];
    rtcm_espnow::GnssCommandRequestPacket packet;
};

struct CommandStep {
    const char* command;
    uint32_t delayAfterMs;
};

constexpr uint8_t BASE_COMMAND_STEPS = 14;
constexpr uint8_t ROVER_COMMAND_STEPS = 4;
QueueHandle_t commandQueue = nullptr;
GnssCommandStats stats{};
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
bool haveLastResult = false;
uint8_t lastResultMac[6] = {};
rtcm_espnow::GnssCommandResultPacket lastResult{};

struct ObservationState {
    uint32_t okGeneration = 0;
    uint32_t errorGeneration = 0;
    uint32_t modeGeneration = 0;
    uint32_t ggaGeneration = 0;
    GnssObservedMode mode = GnssObservedMode::Unknown;
    uint8_t ggaQuality = 0;
};

ObservationState observations{};

void incrementStat(uint32_t GnssCommandStats::*member)
{
    portENTER_CRITICAL(&statsMux);
    ++(stats.*member);
    portEXIT_CRITICAL(&statsMux);
}

void setPromotedToBase(bool promoted)
{
    portENTER_CRITICAL(&statsMux);
    stats.promotedToBase = promoted;
    portEXIT_CRITICAL(&statsMux);
    temporaryBaseUplinkSetEnabled(promoted);
}

void setRtkCorrectionHeld(bool held)
{
    portENTER_CRITICAL(&statsMux);
    stats.rtkCorrectionHeld = held;
    portEXIT_CRITICAL(&statsMux);
}

void setRoleTransitionActive(bool active)
{
    portENTER_CRITICAL(&statsMux);
    stats.roleTransitionActive = active;
    portEXIT_CRITICAL(&statsMux);
}

ObservationState getObservations()
{
    portENTER_CRITICAL(&statsMux);
    const ObservationState copy = observations;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}

bool macEquals(const uint8_t* left, const uint8_t* right)
{
    return left != nullptr && right != nullptr && std::memcmp(left, right, 6) == 0;
}

bool sendResult(const uint8_t destinationMac[6],
                rtcm_espnow::GnssCommandResultPacket& result)
{
    result.authTag = rtcm_espnow::pairingAuthTag(result,
                                                 ESPNOW_PAIRING_KEY,
                                                 sizeof(ESPNOW_PAIRING_KEY));
    const EspNowTxResult sendResult = espnowTxSend(
        destinationMac,
        reinterpret_cast<const uint8_t*>(&result),
        sizeof(result));
    if (sendResult != EspNowTxResult::Success) {
        incrementStat(&GnssCommandStats::resultSendFailures);
        Serial.printf("[ROVER][GNSS_CMD][WARN] Result TX failed txn=%lu result=%s\n",
                      static_cast<unsigned long>(result.transactionId),
                      espnowTxResultToString(sendResult));
        return false;
    }
    incrementStat(&GnssCommandStats::resultsSent);
    return true;
}

rtcm_espnow::GnssCommandResultPacket makeResult(
    const rtcm_espnow::GnssCommandRequestPacket& request)
{
    rtcm_espnow::GnssCommandResultPacket result{};
    result.common.magic = rtcm_espnow::MAGIC;
    result.common.version = rtcm_espnow::VERSION;
    result.common.packetType = rtcm_espnow::PACKET_TYPE_GNSS_COMMAND_RESULT;
    result.networkId = ESPNOW_NETWORK_ID;
    result.transactionId = request.transactionId;
    result.commandId = request.commandId;
    return result;
}

bool writeCommand(const char* command)
{
    const size_t length = std::strlen(command);
    const size_t written = Serial1.write(
        reinterpret_cast<const uint8_t*>(command), length);
    Serial1.flush();
    return written == length;
}

bool writeCommandAndWaitResponse(const char* command,
                                 rtcm_espnow::GnssCommandResultPacket& result);
bool verifyPersistedRole(bool promotedToBase,
                         rtcm_espnow::GnssCommandResultPacket& result);

bool executeCommandSteps(const QueuedGnssCommand& queued,
                         rtcm_espnow::GnssCommandResultPacket& result,
                         const CommandStep* steps,
                         uint8_t stepCount,
                         bool roleChange,
                         bool promotedToBase)
{
    result.totalSteps = stepCount;
    if (roleChange) {
        setRoleTransitionActive(true);
    }
    if (gnssTxMutex == nullptr ||
        xSemaphoreTake(gnssTxMutex,
                       pdMS_TO_TICKS(GNSS_COMMAND_UART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_BUSY;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_UART_WRITE;
        if (roleChange) {
            setRoleTransitionActive(false);
        }
        return false;
    }

    bool succeeded = true;
    for (uint8_t index = 0; index < stepCount; ++index) {
        if (!writeCommandAndWaitResponse(steps[index].command, result)) {
            succeeded = false;
            break;
        }
        result.completedStep = index + 1;
        Serial.printf("[ROVER][GNSS_CMD] txn=%lu step=%u/%u command=%s",
                      static_cast<unsigned long>(queued.packet.transactionId),
                      index + 1,
                      stepCount,
                      steps[index].command);
        if (steps[index].delayAfterMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(steps[index].delayAfterMs));
        }
    }
    if (succeeded && roleChange) {
        succeeded = verifyPersistedRole(promotedToBase, result);
    }
    if (succeeded) {
        if (roleChange) {
            setPromotedToBase(promotedToBase);
        }
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_VERIFIED;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_NONE;
        incrementStat(&GnssCommandStats::sequencesCompleted);
    }
    xSemaphoreGive(gnssTxMutex);
    if (roleChange) {
        setRoleTransitionActive(false);
    }
    return succeeded;
}

bool executeBaseFixedEcefSequence(const QueuedGnssCommand& queued,
                                  rtcm_espnow::GnssCommandResultPacket& result)
{
    char modeCommand[128] = {};
    snprintf(modeCommand,
             sizeof(modeCommand),
             "mode base %.4f %.4f %.4f\r\n",
             static_cast<double>(queued.packet.ecefXScaled) /
                 rtcm_espnow::ECEF_SCALE,
             static_cast<double>(queued.packet.ecefYScaled) /
                 rtcm_espnow::ECEF_SCALE,
             static_cast<double>(queued.packet.ecefZScaled) /
                 rtcm_espnow::ECEF_SCALE);
    const CommandStep steps[BASE_COMMAND_STEPS] = {
        {"unlogall\r\n", GNSS_COMMAND_UNLOG_DELAY_MS},
        {modeCommand, GNSS_COMMAND_MODE_DELAY_MS},
        {"gpgga com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1006 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1033 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1074 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1124 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1084 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1094 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1042 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1019 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1020 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"rtcm1045 com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"saveconfig\r\n", 0},
    };
    const bool succeeded =
        executeCommandSteps(queued, result, steps, BASE_COMMAND_STEPS, true, true);
    if (succeeded) {
        setRtkCorrectionHeld(false);
    }
    return succeeded;
}

bool waitForCommandResponse(const ObservationState& before,
                            rtcm_espnow::GnssCommandResultPacket& result)
{
    const uint32_t startedAt = millis();
    while (millis() - startedAt < GNSS_COMMAND_RESPONSE_TIMEOUT_MS) {
        const ObservationState current = getObservations();
        if (current.errorGeneration != before.errorGeneration) {
            result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
            result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_RESPONSE_REJECTED;
            incrementStat(&GnssCommandStats::responseRejected);
            return false;
        }
        if (current.okGeneration != before.okGeneration) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
    result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_RESPONSE_TIMEOUT;
    incrementStat(&GnssCommandStats::responseTimeouts);
    return false;
}

bool writeCommandAndWaitResponse(const char* command,
                                 rtcm_espnow::GnssCommandResultPacket& result)
{
    const ObservationState before = getObservations();
    if (!writeCommand(command)) {
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_UART_WRITE;
        incrementStat(&GnssCommandStats::uartWriteFailures);
        return false;
    }
    return waitForCommandResponse(before, result);
}

bool waitForModeQuery(const ObservationState& before,
                      GnssObservedMode expectedMode)
{
    const uint32_t startedAt = millis();
    while (millis() - startedAt < GNSS_COMMAND_RESPONSE_TIMEOUT_MS) {
        const ObservationState current = getObservations();
        if (current.errorGeneration != before.errorGeneration) {
            return false;
        }
        if (current.modeGeneration != before.modeGeneration &&
            current.mode == expectedMode) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool verifyPersistedRole(bool promotedToBase,
                         rtcm_espnow::GnssCommandResultPacket& result)
{
    const GnssObservedMode expectedMode = promotedToBase
                                              ? GnssObservedMode::Base
                                              : GnssObservedMode::Rover;
    const ObservationState beforeReset = getObservations();
    vTaskDelay(pdMS_TO_TICKS(GNSS_COMMAND_SAVECONFIG_SETTLE_MS));
    if (!writeCommand("reset\r\n")) {
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_UART_WRITE;
        incrementStat(&GnssCommandStats::uartWriteFailures);
        return false;
    }
    Serial.println("[ROVER][GNSS_CMD][VERIFY] RESET sent; checking NVM role");

    bool modeVerified = false;
    const uint32_t resetAt = millis();
    while (millis() - resetAt < GNSS_COMMAND_RESET_BOOT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(GNSS_COMMAND_MODE_QUERY_INTERVAL_MS));
        const ObservationState beforeQuery = getObservations();
        if (writeCommand("mode\r\n") &&
            waitForModeQuery(beforeQuery, expectedMode)) {
            modeVerified = true;
            break;
        }
    }
    if (!modeVerified) {
        const ObservationState current = getObservations();
        const bool receivedMode =
            current.modeGeneration != beforeReset.modeGeneration &&
            current.mode != GnssObservedMode::Unknown;
        if (receivedMode) {
            setPromotedToBase(current.mode == GnssObservedMode::Base);
            Serial.printf("[ROVER][GNSS_CMD][VERIFY][WARN] actual_mode=%s expected=%s\n",
                          current.mode == GnssObservedMode::Base ? "base" : "rover",
                          promotedToBase ? "base" : "rover");
        }
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
        result.detailCode = receivedMode
                                ? rtcm_espnow::GNSS_COMMAND_DETAIL_PERSISTENCE_VERIFY
                                : rtcm_espnow::GNSS_COMMAND_DETAIL_MODE_VERIFY;
        incrementStat(&GnssCommandStats::modeVerifyFailures);
        if (receivedMode) {
            incrementStat(&GnssCommandStats::persistenceVerifyFailures);
        }
        return false;
    }

    // MODE readback is the authoritative role after RESET. Keep software
    // routing aligned with the receiver even if the following GGA check fails.
    setPromotedToBase(promotedToBase);

    const uint32_t ggaWaitAt = millis();
    while (millis() - ggaWaitAt < GNSS_COMMAND_GGA_VERIFY_TIMEOUT_MS) {
        const ObservationState current = getObservations();
        if (current.ggaGeneration != beforeReset.ggaGeneration) {
            const bool qualityMatches = promotedToBase
                                            ? current.ggaQuality == 7
                                            : current.ggaQuality != 7;
            if (qualityMatches) {
                Serial.printf(
                    "[ROVER][GNSS_CMD][VERIFY] persisted_mode=%s gga_quality=%u\n",
                    promotedToBase ? "base" : "rover",
                    current.ggaQuality);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
    result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_GGA_VERIFY;
    incrementStat(&GnssCommandStats::ggaVerifyFailures);
    return false;
}

bool executeRoverSequence(const QueuedGnssCommand& queued,
                          rtcm_espnow::GnssCommandResultPacket& result)
{
    const CommandStep steps[ROVER_COMMAND_STEPS] = {
        {"unlogall\r\n", GNSS_COMMAND_UNLOG_DELAY_MS},
        {"mode rover survey\r\n", GNSS_COMMAND_MODE_DELAY_MS},
        {"gpgga com2 1\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
        {"saveconfig\r\n", 0},
    };
    const bool succeeded =
        executeCommandSteps(queued, result, steps, ROVER_COMMAND_STEPS, true, false);
    if (succeeded) {
        setRtkCorrectionHeld(false);
    }
    return succeeded;
}

bool executeRtkResetSequence(const QueuedGnssCommand& queued,
                             rtcm_espnow::GnssCommandResultPacket& result)
{
    setRtkCorrectionHeld(true);
    clearGgaDebugSnapshot();
    const CommandStep steps[] = {
        {"config rtk disable\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
    };
    const bool localReset =
        executeCommandSteps(queued, result, steps, 1, false, false);
    if (!localReset) {
        setRtkCorrectionHeld(false);
    }
    if constexpr (ROVER_RELAY_MODE) {
        if (localReset &&
            !relayRequestChildrenRtkReset(queued.packet.transactionId)) {
            result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
            result.detailCode =
                rtcm_espnow::GNSS_COMMAND_DETAIL_QUEUE_FULL;
            return false;
        }
    }
    return localReset;
}

bool executeRtkResumeSequence(const QueuedGnssCommand& queued,
                              rtcm_espnow::GnssCommandResultPacket& result)
{
    const CommandStep steps[] = {
        {"config rtk user_defaults\r\n", GNSS_COMMAND_OUTPUT_DELAY_MS},
    };
    const bool localResume =
        executeCommandSteps(queued, result, steps, 1, false, false);
    if (localResume) {
        setRtkCorrectionHeld(false);
    }
    if constexpr (ROVER_RELAY_MODE) {
        if (localResume &&
            !relayRequestChildrenRtkResume(queued.packet.transactionId)) {
            result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
            result.detailCode =
                rtcm_espnow::GNSS_COMMAND_DETAIL_QUEUE_FULL;
            return false;
        }
    }
    return localResume;
}
}

bool gnssCommandSetup()
{
    if (commandQueue != nullptr) {
        return true;
    }
    commandQueue = xQueueCreate(GNSS_COMMAND_QUEUE_LENGTH,
                                sizeof(QueuedGnssCommand));
    if (commandQueue == nullptr) {
        Serial.println("[ROVER][GNSS_CMD][ERROR] Cannot create command queue");
        return false;
    }
    Serial.println(
        "[ROVER][GNSS_CMD] Ready actions=base_fixed_ecef,rover,rtk_reset "
        "port=COM2");
    return true;
}

bool gnssCommandHandleRequest(const uint8_t* sourceMac,
                              const uint8_t* data,
                              int length)
{
    if (data == nullptr ||
        length < static_cast<int>(sizeof(rtcm_espnow::EspNowCommonHeader))) {
        return false;
    }
    rtcm_espnow::EspNowCommonHeader common{};
    std::memcpy(&common, data, sizeof(common));
    if (common.packetType != rtcm_espnow::PACKET_TYPE_GNSS_COMMAND_REQUEST) {
        return false;
    }
    incrementStat(&GnssCommandStats::requestsReceived);

    if (sourceMac == nullptr ||
        length != static_cast<int>(sizeof(rtcm_espnow::GnssCommandRequestPacket))) {
        incrementStat(&GnssCommandStats::requestsInvalid);
        return true;
    }
    QueuedGnssCommand queued{};
    std::memcpy(queued.sourceMac, sourceMac, sizeof(queued.sourceMac));
    std::memcpy(&queued.packet, data, sizeof(queued.packet));
    if (!rtcm_espnow::validateGnssCommandRequest(queued.packet,
                                                 sizeof(queued.packet),
                                                 ESPNOW_NETWORK_ID,
                                                 ESPNOW_PAIRING_KEY,
                                                 sizeof(ESPNOW_PAIRING_KEY))) {
        incrementStat(&GnssCommandStats::requestsInvalid);
        Serial.println("[ROVER][GNSS_CMD][REJECT] Invalid request/auth/COM/duration");
        return true;
    }
    if (commandQueue == nullptr || xQueueSend(commandQueue, &queued, 0) != pdTRUE) {
        incrementStat(&GnssCommandStats::queueOverflow);
        Serial.printf("[ROVER][GNSS_CMD][REJECT] Queue full txn=%lu\n",
                      static_cast<unsigned long>(queued.packet.transactionId));
        return true;
    }
    Serial.printf("[ROVER][GNSS_CMD] Queued txn=%lu command_id=%u\n",
                  static_cast<unsigned long>(queued.packet.transactionId),
                  queued.packet.commandId);
    return true;
}

void gnssCommandTask(void* parameter)
{
    (void)parameter;
    while (true) {
        QueuedGnssCommand queued{};
        if (commandQueue == nullptr ||
            xQueueReceive(commandQueue, &queued, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (haveLastResult &&
            macEquals(queued.sourceMac, lastResultMac) &&
            queued.packet.transactionId == lastResult.transactionId &&
            queued.packet.commandId == lastResult.commandId) {
            incrementStat(&GnssCommandStats::duplicateRequests);
            sendResult(queued.sourceMac, lastResult);
            continue;
        }

        rtcm_espnow::GnssCommandResultPacket result = makeResult(queued.packet);
        if (queued.packet.commandId ==
            rtcm_espnow::GNSS_COMMAND_SWITCH_TO_BASE_FIXED_ECEF) {
            executeBaseFixedEcefSequence(queued, result);
        } else if (queued.packet.commandId ==
                   rtcm_espnow::GNSS_COMMAND_RESET_RTK) {
            executeRtkResetSequence(queued, result);
        } else if (queued.packet.commandId ==
                   rtcm_espnow::GNSS_COMMAND_RESUME_RTK) {
            executeRtkResumeSequence(queued, result);
        } else {
            executeRoverSequence(queued, result);
        }
        std::memcpy(lastResultMac, queued.sourceMac, sizeof(lastResultMac));
        lastResult = result;
        haveLastResult = true;
        sendResult(queued.sourceMac, lastResult);
        Serial.printf("[ROVER][GNSS_CMD] Completed txn=%lu status=%u steps=%u/%u detail=%u\n",
                      static_cast<unsigned long>(result.transactionId),
                      result.status,
                      result.completedStep,
                      result.totalSteps,
                      result.detailCode);
    }
}

void gnssCommandObserveLine(const char* data, std::size_t length)
{
    const GnssLineObservation observation =
        parseGnssCommandObservation(data, length);
    if (!observation.responseOk && !observation.responseError &&
        observation.mode == GnssObservedMode::Unknown && !observation.hasGga) {
        return;
    }
    portENTER_CRITICAL(&statsMux);
    if (observation.responseOk) {
        ++observations.okGeneration;
    }
    if (observation.responseError) {
        ++observations.errorGeneration;
    }
    if (observation.mode != GnssObservedMode::Unknown) {
        ++observations.modeGeneration;
        observations.mode = observation.mode;
    }
    if (observation.hasGga) {
        ++observations.ggaGeneration;
        observations.ggaQuality = observation.ggaQuality;
    }
    portEXIT_CRITICAL(&statsMux);
}

bool gnssCommandAcceptsRtcmCorrection()
{
    portENTER_CRITICAL(&statsMux);
    const bool accepts = !stats.promotedToBase &&
                         !stats.rtkCorrectionHeld &&
                         !stats.roleTransitionActive;
    portEXIT_CRITICAL(&statsMux);
    return accepts;
}

bool gnssCommandPublishesRoverStatus()
{
    portENTER_CRITICAL(&statsMux);
    const bool publishes = !stats.promotedToBase &&
                           !stats.roleTransitionActive;
    portEXIT_CRITICAL(&statsMux);
    return publishes;
}

bool gnssCommandRoutesGnssToTemporaryBase()
{
    portENTER_CRITICAL(&statsMux);
    const bool routes = stats.promotedToBase &&
                        !stats.roleTransitionActive;
    portEXIT_CRITICAL(&statsMux);
    return routes;
}

GnssCommandStats gnssCommandGetStats()
{
    portENTER_CRITICAL(&statsMux);
    const GnssCommandStats copy = stats;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}
