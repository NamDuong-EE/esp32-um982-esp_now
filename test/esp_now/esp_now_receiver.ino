#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>

#define LED_PIN 2

WebServer server(80);

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
  uint8_t command;
  uint32_t param;
} cmd_message;

data_message myData;

// ========================
// Hien thi
// ========================
String senderMAC = "-";
unsigned long packetCount = 0;
int lastRSSI = 0;

// Peer sender (tu dong them khi nhan goi dau tien)
bool senderPeerAdded = false;
uint8_t senderAddr[6];

// ========================
// Thong ke test
// ========================
volatile uint32_t testRecvCount = 0;
volatile uint32_t testExpected = 0;
volatile unsigned long testStartTime = 0;
volatile unsigned long testEndTime = 0;
volatile int minRSSI = 0;
volatile int maxRSSI = -120;
volatile long rssiSum = 0;
volatile bool testRunning = false;
volatile uint32_t testPktSize = 0;

// Ket qua test gan nhat
uint32_t lastTestRecv = 0;
uint32_t lastTestExpected = 0;
unsigned long lastTestDuration = 0;
float lastThroughput = 0;
float lastPacketLoss = 0;
float lastPPS = 0;
int lastAvgRSSI = 0;
int lastMinRSSI = 0;
int lastMaxRSSI = -120;
bool hasTestResult = false;

// ========================
// Ket thuc test (luu ket qua)
// ========================
void finalizeTest() {
  testRunning = false;
  unsigned long duration = testEndTime - testStartTime;

  lastTestRecv = testRecvCount;
  lastTestExpected = testExpected;
  lastTestDuration = duration;
  lastPacketLoss = testExpected > 0
    ? (float)(testExpected - testRecvCount) / testExpected * 100.0
    : 0;

  if (duration > 0) {
    lastThroughput = (float)(testRecvCount * testPktSize) * 1000.0 / duration / 1024.0;
    lastPPS = (float)testRecvCount * 1000.0 / duration;
  }

  lastAvgRSSI = testRecvCount > 0 ? rssiSum / (int)testRecvCount : 0;
  lastMinRSSI = minRSSI;
  lastMaxRSSI = maxRSSI;
  hasTestResult = true;

  Serial.println("\n===== KET QUA NHAN =====");
  Serial.print("Goi nhan    : "); Serial.print(lastTestRecv);
  Serial.print(" / "); Serial.println(lastTestExpected);
  Serial.print("Packet Loss : "); Serial.print(lastPacketLoss, 1); Serial.println(" %");
  Serial.print("Thoi gian   : "); Serial.print(lastTestDuration); Serial.println(" ms");
  Serial.print("Throughput  : "); Serial.print(lastThroughput, 2); Serial.println(" KB/s");
  Serial.print("Packets/sec : "); Serial.println(lastPPS, 1);
  Serial.print("RSSI Avg    : "); Serial.print(lastAvgRSSI); Serial.println(" dBm");
  Serial.print("RSSI Range  : "); Serial.print(lastMinRSSI);
  Serial.print(" ~ "); Serial.print(lastMaxRSSI); Serial.println(" dBm");
  Serial.println("========================");
}

// ========================
// Trang web
// ========================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 TESTING </title></head>";
  html += "<body style='font-family:Arial;text-align:center'>";

  html += "<h2>ESP32 ESP-NOW TESTING</h2>";
  html += "<hr>";

  // --- Du lieu nhan ---
  html += "<h3>Du Lieu Nhan</h3>";
  html += "<p><b>Packets:</b> " + String(packetCount) + "</p>";
  html += "<p><b>Sender:</b> " + senderMAC + "</p>";
  html += "<p><b>Char:</b> " + String(myData.a) + "</p>";
  html += "<p><b>Int:</b> " + String(myData.b) + "</p>";
  html += "<p><b>Float:</b> " + String(myData.c) + "</p>";
  html += "<p><b>Bool:</b> " + String(myData.d) + "</p>";
  html += "<p><b>RSSI:</b> " + String(lastRSSI) + " dBm</p>";

  html += "<hr>";

  // --- Test bang thong ---
  html += "<h3>Test Bang Thong / Packet Loss</h3>";

  if (testRunning) {
    html += "<p><b>Trang thai:</b> DANG CHAY</p>";
    html += "<p><b>Da nhan:</b> " + String(testRecvCount) + " / " + String(testExpected) + "</p>";
  } else if (hasTestResult) {
    html += "<p><b>Trang thai:</b> HOAN THANH</p>";
    html += "<p><b>Goi nhan:</b> " + String(lastTestRecv) + " / " + String(lastTestExpected) + "</p>";
    html += "<p><b>Packet Loss:</b> " + String(lastPacketLoss, 1) + " %</p>";
    html += "<p><b>Thoi gian:</b> " + String(lastTestDuration) + " ms</p>";
    html += "<p><b>Throughput:</b> " + String(lastThroughput, 2) + " KB/s</p>";
    html += "<p><b>Packets/sec:</b> " + String(lastPPS, 1) + "</p>";
    html += "<p><b>RSSI Avg:</b> " + String(lastAvgRSSI) + " dBm</p>";
    html += "<p><b>RSSI Min/Max:</b> " + String(lastMinRSSI) + " / " + String(lastMaxRSSI) + " dBm</p>";
  } else {
    html += "<p><b>Trang thai:</b> CHO TEST</p>";
  }

  // Nut bat dau test
  if (!testRunning) {
    if (!senderPeerAdded) {
      html += "<p><i>Chua co sender ket noi. Cho goi dau tien...</i></p>";
    } else {
      html += "<br><form action='/starttest' method='GET'>";
      html += "So goi: <input type='number' name='n' value='100' min='10' max='10000' style='width:80px'>";
      html += " <button type='submit'>BAT DAU TEST</button>";
      html += "</form>";
    }
    html += "<br><a href='/reset'><button>RESET</button></a>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ========================
// Xu ly bat dau test tu web
// ========================
void handleStartTest() {
  if (!senderPeerAdded) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  uint32_t n = 100;
  if (server.hasArg("n")) {
    n = server.arg("n").toInt();
    if (n == 0 || n > 10000) n = 100;
  }

  // Reset thong ke
  testRecvCount = 0;
  testExpected = n;
  testRunning = false;
  hasTestResult = false;
  rssiSum = 0;
  minRSSI = 0;
  maxRSSI = -120;

  // Gui lenh toi sender qua ESP-NOW
  cmd_message cmd;
  cmd.type = PKT_CMD;
  cmd.command = 1;
  cmd.param = n;

  esp_err_t result = esp_now_send(senderAddr, (uint8_t *)&cmd, sizeof(cmd));

  Serial.print("Gui lenh test "); Serial.print(n);
  Serial.print(" goi -> ");
  Serial.println(result == ESP_OK ? "OK" : "FAIL");

  server.sendHeader("Location", "/");
  server.send(303);
}

// ========================
// Xu ly reset
// ========================
void handleReset() {
  testRecvCount = 0;
  testExpected = 0;
  testStartTime = 0;
  testEndTime = 0;
  testRunning = false;
  hasTestResult = false;
  rssiSum = 0;
  minRSSI = 0;
  maxRSSI = -120;
  server.sendHeader("Location", "/");
  server.send(303);
}

// ========================
// Callback nhan ESP-NOW
// ========================
void OnDataRecv(const esp_now_recv_info_t *recv_info,
                const uint8_t *incomingData,
                int len)
{
  // Lay RSSI
  if (recv_info->rx_ctrl) {
    lastRSSI = recv_info->rx_ctrl->rssi;
  }

  // Lay MAC sender
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          recv_info->src_addr[0], recv_info->src_addr[1],
          recv_info->src_addr[2], recv_info->src_addr[3],
          recv_info->src_addr[4], recv_info->src_addr[5]);
  senderMAC = String(macStr);

  // Tu dong them sender lam peer (de gui lenh nguoc lai)
  if (!senderPeerAdded) {
    memcpy(senderAddr, recv_info->src_addr, 6);
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, senderAddr, 6);
    pi.channel = 1;
    pi.encrypt = false;
    if (esp_now_add_peer(&pi) == ESP_OK) {
      senderPeerAdded = true;
      Serial.println("Da them sender lam peer");
    }
  }

  // Phan loai goi
  uint8_t pktType = incomingData[0];

  if (pktType == PKT_DATA && len >= (int)sizeof(data_message)) {
    // ===== GOI DU LIEU THUONG =====
    memcpy(&myData, incomingData, sizeof(myData));
    packetCount++;

    Serial.println("\n===== DATA =====");
    Serial.print("Packets : "); Serial.println(packetCount);
    Serial.print("MAC     : "); Serial.println(senderMAC);
    Serial.print("Char    : "); Serial.println(myData.a);
    Serial.print("Int     : "); Serial.println(myData.b);
    Serial.print("Float   : "); Serial.println(myData.c);
    Serial.print("Bool    : "); Serial.println(myData.d);
    Serial.print("RSSI    : "); Serial.print(lastRSSI); Serial.println(" dBm");

    // Nhay LED
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);

  } else if (pktType == PKT_TEST && len >= (int)sizeof(test_message)) {
    // ===== GOI TEST =====
    test_message testData;
    memcpy(&testData, incomingData, sizeof(testData));

    // Bat dau test moi khi nhan goi seq=0
    if (testData.seq == 0) {
      testRecvCount = 0;
      testExpected = testData.total;
      testStartTime = millis();
      testRunning = true;
      rssiSum = 0;
      minRSSI = 0;
      maxRSSI = -120;
      testPktSize = len;
      Serial.println("\n[TEST] Bat dau nhan goi test...");
    }

    testRecvCount++;
    rssiSum += lastRSSI;
    if (lastRSSI < minRSSI) minRSSI = lastRSSI;
    if (lastRSSI > maxRSSI) maxRSSI = lastRSSI;
    testEndTime = millis();

    // Kiem tra hoan thanh
    if (testRecvCount >= testExpected) {
      finalizeTest();
    }
  }
}

// ========================
// Setup
// ========================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // AP o channel 1 de dung voi ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_DEBUG", "12345678", 1);

  // Bat che do Long Range (LR) cho ESP-NOW tren STA interface
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
  // AP chi dung protocol thuong (11b/g/n) de dien thoai ket noi
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  Serial.println();
  Serial.println("================================");
  Serial.println("  ESP-NOW Receiver (LR Mode)");
  Serial.println("================================");
  Serial.print("SSID : "); Serial.println("ESP32_DEBUG");
  Serial.print("PASS : "); Serial.println("12345678");
  Serial.print("IP   : "); Serial.println(WiFi.softAPIP());
  Serial.println("================================");

  server.on("/", handleRoot);
  server.on("/starttest", handleStartTest);
  server.on("/reset", handleReset);
  server.begin();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("ESP-NOW Ready - Cho du lieu...");
}

// ========================
// Loop
// ========================
void loop() {
  server.handleClient();

  // Timeout: neu test dang chay ma >5s khong nhan goi -> ket thuc
  if (testRunning && testEndTime > 0 && (millis() - testEndTime > 5000)) {
    finalizeTest();
    Serial.println("[TIMEOUT] Test ket thuc do het goi.");
  }
}