#include "helper.h"
#include "hardware/DebugWeb_handler.h"
#include "hardware/Relay_handler.h"

extern PubSubClient mqtt;

String gnssLineBuffer;
String latestGGA;
String targetGGA = "$GNGGA,045151.00,2104.44183385,N,10546.62503715,E,1,28,0.7,22.4381,M,-28.2448,M,,*6C";

SemaphoreHandle_t mqttClientMutex = nullptr;
SemaphoreHandle_t nmeaBufferMutex = nullptr;
SemaphoreHandle_t gnssTxMutex = nullptr;

void taskRtcm(void* parameter);
void gnssParseTask(void* parameter);
void gnssPublishTask(void* parameter);
void healthCheckTask(void* parameter);
void relaySendTask(void* parameter);

namespace {

SemaphoreHandle_t createRequiredMutex(const char* name) {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        Serial.printf("[SETUP][FATAL] Khong tao duoc mutex %s\n", name);
    }
    return mutex;
}

void createRequiredTask(TaskFunction_t task,
                        const char* name,
                        uint32_t stackSize,
                        UBaseType_t priority,
                        BaseType_t core) {
    if (xTaskCreatePinnedToCore(task, name, stackSize, nullptr, priority, nullptr, core) != pdPASS) {
        Serial.printf("[SETUP][FATAL] Khong tao duoc task %s\n", name);
    }
}

bool setupNetworkWithRetry() {
    while (!setupWiFi()) {
        Serial.printf("[SETUP] Thu lai cau hinh radio mang sau %lu giay\n",
                      static_cast<unsigned long>(WIFI_RETRY_DELAY_MS / 1000));
        delay(WIFI_RETRY_DELAY_MS);
    }
    return true;
}

} // namespace

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=========================================");
    Serial.println("       ESP32U GNSS ROVER KHOI DONG       ");
    Serial.println("=========================================");
    Serial.printf("[MODE] %s\n", ROVER_RELAY_MODE ? "relay" : "normal");

    if (Serial1.setTxBufferSize(GNSS_TX_BUFFER_SIZE) != GNSS_TX_BUFFER_SIZE) {
        Serial.println("[GNSS][ERROR] Khong dat duoc UART TX buffer");
    }
    Serial1.begin(GNSS_BAUD, SERIAL_8N1, RX_GNSS, TX_GNSS);
    Serial.printf("[GNSS] UART1 baud=%lu RX=%d TX=%d tx_buffer=%u\n",
                  static_cast<unsigned long>(GNSS_BAUD),
                  RX_GNSS,
                  TX_GNSS,
                  static_cast<unsigned>(GNSS_TX_BUFFER_SIZE));

    mqttClientMutex = createRequiredMutex("mqttClientMutex");
    nmeaBufferMutex = createRequiredMutex("nmeaBufferMutex");
    gnssTxMutex = createRequiredMutex("gnssTxMutex");
    if (mqttClientMutex == nullptr || nmeaBufferMutex == nullptr || gnssTxMutex == nullptr) {
        while (true) {
            delay(1000);
        }
    }

    // Keep the ESP-NOW startup path deterministic: configure the radio/core,
    // register all runtime peers, then start the optional SoftAP last. The LR
    // rate must be configured before SoftAP on this ESP32 Arduino stack.
    setupNetworkWithRetry();
    const bool espnowCoreReady = espnowPrepare();
    if (!espnowCoreReady) {
        Serial.println("[SETUP][WARN] ESP-NOW core/LR chua san sang");
    }
    if (espnowCoreReady && !espnowSetup()) {
        Serial.println("[SETUP][WARN] ESP-NOW chua hoat dong; hay pair voi Base");
    }
    if constexpr (ROVER_RELAY_MODE) {
        if (!relaySetup()) {
            Serial.println("[SETUP][WARN] Relay downstream chua san sang");
        }
    }
    if constexpr (DEBUG_WEB_ENABLED) {
        if (espnowIsReady() && !debugWebSetup()) {
            Serial.println("[SETUP][WARN] Debug web chua hoat dong");
        }
    }

    if constexpr (ROVER_MQTT_ENABLED) {
        setupMQTT();
        connectMQTT();
    } else {
        Serial.println("[MQTT] Da tat theo cau hinh ROVER_MQTT_ENABLED=false");
    }
    resetRtcmEspNowReassembly();

    createRequiredTask(taskRtcm, "RTCM ESP-NOW", 4096, 4, 1);
    createRequiredTask(gnssParseTask, "GNSS Parse", 4096, 3, 0);
    createRequiredTask(gnssPublishTask, "GNSS Publish", 4096, 2, 0);
    createRequiredTask(healthCheckTask, "Health", 4096, 1, 1);
    if constexpr (ROVER_RELAY_MODE) {
        createRequiredTask(relaySendTask, "RTCM Relay", 4096, 3, 1);
    }

    digitalWrite(LED_PIN, LOW);
    Serial.println("[SETUP] Khoi dong hoan tat");
}

void taskRtcm(void* parameter) {
    (void)parameter;
    while (true) {
        if (!espnowIsReady()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        processNextRtcmEspNowPacket(pdMS_TO_TICKS(50));
    }
}

void gnssParseTask(void* parameter) {
    (void)parameter;
    String currentLine;
    currentLine.reserve(512);

    while (true) {
        while (Serial1.available()) {
            const char value = static_cast<char>(Serial1.read());
            if (value == '\n') {
                currentLine += value;
                if (xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    gnssLineBuffer = currentLine;
                    xSemaphoreGive(nmeaBufferMutex);
                }
                currentLine = "";
            } else if (value != '\0') {
                currentLine += value;
                if (currentLine.length() > 1024) {
                    Serial.println("[GNSS][WARN] Dong GNSS qua dai, da bo");
                    currentLine = "";
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void gnssPublishTask(void* parameter) {
    (void)parameter;
    while (true) {
        String localLine;
        if (xSemaphoreTake(nmeaBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            localLine = gnssLineBuffer;
            gnssLineBuffer = "";
            xSemaphoreGive(nmeaBufferMutex);
        }

        if (!localLine.isEmpty()) {
            if constexpr (ROVER_MQTT_ENABLED) {
                if (xSemaphoreTake(mqttClientMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    if (mqtt.connected()) {
                        publishGGA(localLine);
                    }
                    xSemaphoreGive(mqttClientMutex);
                }
            } else {
                publishGGA(localLine);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void healthCheckTask(void* parameter) {
    (void)parameter;
    while (true) {
        const String payload = formDeviceHealthString();
        Serial.print("[HEALTH] ");
        Serial.println(payload);
        if constexpr (MQTT_PUBLISH_HEALTH_ENABLED) {
            if (xSemaphoreTake(mqttClientMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                if (mqtt.connected()) {
                    publishHealth(payload);
                }
                xSemaphoreGive(mqttClientMutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(HEALTH_INTERVAL_MS));
    }
}

void relaySendTask(void* parameter) {
    (void)parameter;
    while (true) {
        if (!relayIsReady()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        relayProcessNextFrame(pdMS_TO_TICKS(100));
    }
}

void loop() {
    static bool wasConnected = true;
    static uint32_t lastEspNowRetry = 0;

    if constexpr (WIFI_CONNECT_TO_ROUTER_ENABLED) {
        const bool wifiConnected = WiFi.status() == WL_CONNECTED;

        if (!wifiConnected) {
            if (wasConnected) {
                Serial.println("[WIFI] Mat ket noi router/AP");
            }
            wasConnected = false;
            setupNetworkWithRetry();
            espnowRefreshPeerChannel();
        } else if (!wasConnected) {
            wasConnected = true;
            espnowRefreshPeerChannel();
        }
    }

    if (!espnowIsReady() && ESPNOW_PAIRING_ENABLED &&
        millis() - lastEspNowRetry >= 5000) {
        lastEspNowRetry = millis();
        if (espnowPrepare() && espnowSetup()) {
            if constexpr (ROVER_RELAY_MODE) {
                relaySetup();
            }
            if constexpr (DEBUG_WEB_ENABLED) {
                debugWebSetup();
            }
        }
    }
    if constexpr (ROVER_RELAY_MODE) {
        if (espnowIsReady() && !relayIsReady()) {
            relaySetup();
        }
    }

    if constexpr (ROVER_MQTT_ENABLED) {
        if (xSemaphoreTake(mqttClientMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            if (!mqtt.connected()) {
                connectMQTT();
            }
            mqtt.loop();
            xSemaphoreGive(mqttClientMutex);
        }
    }
    if constexpr (DEBUG_WEB_ENABLED) {
        debugWebLoop();
    }
    espnowLoop();
    if constexpr (ROVER_RELAY_MODE) {
        relayLoop();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
