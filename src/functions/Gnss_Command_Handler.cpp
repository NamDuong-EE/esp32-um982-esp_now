#include "functions/Gnss_Command_Handler.h"

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

bool executeCommandSteps(const QueuedGnssCommand& queued,
                         rtcm_espnow::GnssCommandResultPacket& result,
                         const CommandStep* steps,
                         uint8_t stepCount,
                         bool promotedToBase)
{
    result.totalSteps = stepCount;
    if (gnssTxMutex == nullptr ||
        xSemaphoreTake(gnssTxMutex,
                       pdMS_TO_TICKS(GNSS_COMMAND_UART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_BUSY;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_UART_WRITE;
        return false;
    }

    bool succeeded = true;
    for (uint8_t index = 0; index < stepCount; ++index) {
        if (!writeCommand(steps[index].command)) {
            result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_ERROR;
            result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_UART_WRITE;
            incrementStat(&GnssCommandStats::uartWriteFailures);
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
    if (succeeded) {
        setPromotedToBase(promotedToBase);
        result.status = rtcm_espnow::GNSS_COMMAND_STATUS_UART_SEQUENCE_WRITTEN;
        result.detailCode = rtcm_espnow::GNSS_COMMAND_DETAIL_NONE;
        incrementStat(&GnssCommandStats::sequencesCompleted);
    }
    xSemaphoreGive(gnssTxMutex);
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
        executeCommandSteps(queued, result, steps, BASE_COMMAND_STEPS, true);
    if (succeeded) {
        setRtkCorrectionHeld(false);
    }
    return succeeded;
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
        executeCommandSteps(queued, result, steps, ROVER_COMMAND_STEPS, false);
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
        executeCommandSteps(queued, result, steps, 1, false);
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
        executeCommandSteps(queued, result, steps, 1, false);
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

bool gnssCommandAcceptsRtcmCorrection()
{
    portENTER_CRITICAL(&statsMux);
    const bool accepts = !stats.promotedToBase &&
                         !stats.rtkCorrectionHeld;
    portEXIT_CRITICAL(&statsMux);
    return accepts;
}

bool gnssCommandPublishesRoverStatus()
{
    portENTER_CRITICAL(&statsMux);
    const bool publishes = !stats.promotedToBase;
    portEXIT_CRITICAL(&statsMux);
    return publishes;
}

GnssCommandStats gnssCommandGetStats()
{
    portENTER_CRITICAL(&statsMux);
    const GnssCommandStats copy = stats;
    portEXIT_CRITICAL(&statsMux);
    return copy;
}
