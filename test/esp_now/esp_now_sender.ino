/*
  ESP32 ESP-NOW Sender - Long Range Mode
  Ho tro test bang thong va packet loss

  Cach dung:
    - Serial: go "test" hoac "test 500"
    - Hoac nhan nut tren web cua Receiver
*/
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// MAC Address cua ESP32 nhan
uint8_t broadcastAddress[] = {0x68, 0x09, 0x47, 0xf8, 0x48, 0x90};

// ========================
// Packet types
// ========================
#define PKT_DATA 0x01
#define PKT_TEST 0x02
#define PKT_CMD  0x03

typedef struct data_message {
  uint8_t type;
  char a[32];
  int b;
  float c;
  bool d;
} data_message;

typedef struct test_message {
  uint8_t type;
  uint32_t seq;
  uint32_t total;
  uint32_t send_time;
  uint8_t payload[200];
} test_message;

typedef struct cmd_message {
  uint8_t type;
  uint8_t command;   // 1 = start test
  uint32_t param;    // so goi can gui
} cmd_message;

data_message myData;
test_message testPkt;
esp_now_peer_info_t peerInfo;

// Thong ke gui
volatile uint32_t ackOK = 0;
volatile uint32_t ackFail = 0;
bool testing = false;

// Lenh test tu web (nhan qua ESP-NOW)
volatile bool startTestFlag = false;
volatile uint32_t testNumPackets = 100;

// ========================
// Callback gui
// ========================
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  if (testing) {
    if (status == ESP_NOW_SEND_SUCCESS) ackOK++;
    else ackFail++;
  } else {
    Serial.print("Send: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
  }
}

// ========================
// Callback nhan (lenh tu receiver)
// ========================
void OnDataRecv(const esp_now_recv_info_t *recv_info,
                const uint8_t *incomingData, int len) {
  if (len >= (int)sizeof(cmd_message) && incomingData[0] == PKT_CMD) {
    cmd_message cmd;
    memcpy(&cmd, incomingData, sizeof(cmd));
    if (cmd.command == 1) {
      testNumPackets = cmd.param;
      if (testNumPackets == 0 || testNumPackets > 10000) testNumPackets = 100;
      startTestFlag = true;
      Serial.print("\nNhan lenh test tu web: ");
      Serial.print(testNumPackets);
      Serial.println(" goi");
    }
  }
}

// ========================
// Chay test
// ========================
void runTest(uint32_t numPackets) {
  testing = true;
  ackOK = 0;
  ackFail = 0;

  testPkt.type = PKT_TEST;
  testPkt.total = numPackets;
  memset(testPkt.payload, 0xAA, sizeof(testPkt.payload));

  Serial.println("\n=============================");
  Serial.println("  BANDWIDTH TEST - SENDER");
  Serial.println("=============================");
  Serial.print("Packets   : "); Serial.println(numPackets);
  Serial.print("Pkt size  : "); Serial.print(sizeof(testPkt)); Serial.println(" bytes");
  Serial.println("Sending...");

  uint32_t sendErrors = 0;
  unsigned long startTime = millis();

  for (uint32_t i = 0; i < numPackets; i++) {
    testPkt.seq = i;
    testPkt.send_time = millis();

    esp_err_t result = esp_now_send(broadcastAddress,
                                     (uint8_t *)&testPkt, sizeof(testPkt));
    if (result != ESP_OK) sendErrors++;
    delay(15);
  }

  unsigned long duration = millis() - startTime;
  delay(500);

  Serial.println("\n===== KET QUA GUI =====");
  Serial.print("Tong gui     : "); Serial.println(numPackets);
  Serial.print("Send errors  : "); Serial.println(sendErrors);
  Serial.print("ACK OK       : "); Serial.println(ackOK);
  Serial.print("ACK Fail     : "); Serial.println(ackFail);
  Serial.print("Thoi gian    : "); Serial.print(duration); Serial.println(" ms");

  if (duration > 0) {
    float throughput = (float)(numPackets * sizeof(testPkt)) * 1000.0 / duration;
    Serial.print("Throughput   : "); Serial.print(throughput / 1024.0, 2); Serial.println(" KB/s");
    float pps = (float)numPackets * 1000.0 / duration;
    Serial.print("Packets/sec  : "); Serial.println(pps, 1);
  }

  float lossRate = (float)(numPackets - ackOK) / numPackets * 100.0;
  Serial.print("Loss (ACK)   : "); Serial.print(lossRate, 1); Serial.println(" %");
  Serial.println("========================");

  testing = false;
}

// ========================
// Setup
// ========================
void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Bat che do Long Range (LR)
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("  ESP-NOW Sender (LR Mode)");
  Serial.println("================================");
  Serial.println("Serial: test / test N");
  Serial.println("Hoac nhan nut tren web receiver");
  Serial.println("================================");
}

// ========================
// Loop
// ========================
void loop() {
  // Lenh test tu web (qua ESP-NOW)
  if (startTestFlag) {
    startTestFlag = false;
    runTest(testNumPackets);
    return;
  }

  // Lenh test tu Serial
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("test")) {
      uint32_t n = 100;
      if (cmd.length() > 4) {
        n = cmd.substring(5).toInt();
        if (n == 0 || n > 10000) n = 100;
      }
      runTest(n);
      return;
    }
  }

  // Gui du lieu thuong
  myData.type = PKT_DATA;
  strcpy(myData.a, "THANG GUI");
  myData.b = random(1, 20);
  myData.c = 1.2;
  myData.d = false;

  esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
  delay(2000);
}