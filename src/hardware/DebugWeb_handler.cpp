#include "hardware/DebugWeb_handler.h"

#include "Prog_Config.h"

#if DEBUG_WEB_ENABLED

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "helper.h"

namespace {

WebServer debugServer(80);
bool debugWebReady = false;
const IPAddress debugWebIp(192, 168, 4, 1);
const IPAddress debugWebGateway(192, 168, 4, 1);
const IPAddress debugWebSubnet(255, 255, 255, 0);

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 Rover Debug</title>
  <style>
    :root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#1c2526;background:#f5f7f8}
    body{margin:0;padding:20px}
    main{max-width:760px;margin:0 auto}
    h1{font-size:26px;margin:0 0 4px}
    .sub{color:#607074;margin:0 0 20px}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}
    .card{background:#fff;border:1px solid #d9e1e3;border-radius:8px;padding:14px}
    .label{font-size:12px;color:#68787c;text-transform:uppercase;letter-spacing:.04em}
    .value{font-size:24px;font-weight:700;margin-top:6px;word-break:break-word}
    .health{margin-top:16px}
    table{width:100%;border-collapse:collapse;background:#fff;border:1px solid #d9e1e3;border-radius:8px;overflow:hidden}
    td{padding:10px 12px;border-bottom:1px solid #edf1f2}
    td:last-child{text-align:right;font-weight:600}
    tr:last-child td{border-bottom:0}
    .ok{color:#087f5b}.warn{color:#b35c00}.bad{color:#c92a2a}
  </style>
</head>
<body>
  <main>
    <h1>ESP32 Rover Debug</h1>
    <p class="sub">SoftAP 192.168.4.1</p>
    <section class="grid">
      <div class="card"><div class="label">Latitude</div><div id="lat" class="value">--</div></div>
      <div class="card"><div class="label">Longitude</div><div id="lon" class="value">--</div></div>
      <div class="card"><div class="label">RTK status</div><div id="rtk" class="value">--</div></div>
      <div class="card"><div class="label">Satellites</div><div id="sats" class="value">--</div></div>
      <div class="card"><div class="label">Base RSSI</div><div id="rssi" class="value">--</div></div>
    </section>
    <section class="health">
      <table>
        <tbody id="health"></tbody>
      </table>
    </section>
  </main>
  <script>
    const fields = ["espnow_ready","espnow_rssi_dbm","rtcm_frames","last_rtcm_age_ms","rtcm_crc_errors","rtcm_queue_overflow","rtcm_sequence_gaps","ack_queued","ack_send_fail","free_heap_bytes"];
    function text(value){return value === null || value === undefined ? "--" : value;}
    function rssiClass(value){return value === null || value === undefined ? "bad" : value >= -65 ? "ok" : value >= -80 ? "warn" : "bad";}
    async function refresh(){
      try{
        const response = await fetch("/api/status",{cache:"no-store"});
        const data = await response.json();
        document.getElementById("lat").textContent = data.gga.valid ? data.gga.lat.toFixed(7) : "--";
        document.getElementById("lon").textContent = data.gga.valid ? data.gga.lon.toFixed(7) : "--";
        document.getElementById("rtk").textContent = data.gga.valid ? data.gga.fix_quality : "--";
        document.getElementById("rtk").className = "value " + (data.gga.fix_quality === 4 ? "ok" : data.gga.fix_quality === 5 ? "warn" : "bad");
        document.getElementById("sats").textContent = data.gga.valid ? data.gga.satellites : "--";
        document.getElementById("rssi").textContent = data.health.espnow_rssi_dbm === null ? "--" : `${data.health.espnow_rssi_dbm} dBm`;
        document.getElementById("rssi").className = "value " + rssiClass(data.health.espnow_rssi_dbm);
        document.getElementById("health").innerHTML = fields.map((key)=>`<tr><td>${key}</td><td>${text(data.health[key])}</td></tr>`).join("");
      }catch(error){
        document.getElementById("health").innerHTML = "<tr><td>status</td><td>offline</td></tr>";
      }
    }
    refresh();
    setInterval(refresh,1000);
  </script>
</body>
</html>
)HTML";

const char* fixQualityToText(uint8_t fixQuality) {
    switch (fixQuality) {
        case 1:
            return "GPS Fix";
        case 2:
            return "DGPS";
        case 4:
            return "RTK Fixed";
        case 5:
            return "RTK Float";
        default:
            return "Invalid";
    }
}

void handleIndex() {
    debugServer.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
    const GgaDebugSnapshot gga = getGgaDebugSnapshot();
    const EspNowRtcmStats espnowStats = espnowGetStats();
    const uint32_t now = millis();
    const bool hasRtcmFrame = espnowStats.lastValidFrameMillis != 0;
    const bool hasRssi = espnowStats.hasRssi;

    String payload;
    payload.reserve(1024);
    payload += "{\"gga\":{";
    payload += "\"valid\":";
    payload += gga.valid ? "true" : "false";
    payload += ",\"lat\":";
    payload += String(gga.lat, 7);
    payload += ",\"lon\":";
    payload += String(gga.lon, 7);
    payload += ",\"fix_quality\":";
    payload += String(gga.fixQuality);
    payload += ",\"rtk_status\":\"";
    payload += fixQualityToText(gga.fixQuality);
    payload += "\",\"satellites\":";
    payload += String(gga.satellites);
    payload += ",\"last_gga_age_ms\":";
    payload += gga.valid ? String(now - gga.lastUpdateMs) : "null";
    payload += "},\"health\":{";
    payload += "\"uptime_s\":";
    payload += String(now / 1000U);
    payload += ",\"free_heap_bytes\":";
    payload += String(ESP.getFreeHeap());
    payload += ",\"espnow_ready\":";
    payload += espnowIsReady() ? "true" : "false";
    payload += ",\"base_provisioned\":";
    payload += espnowBaseMacIsConfigured() ? "true" : "false";
    payload += ",\"espnow_rssi_dbm\":";
    payload += hasRssi ? String(espnowStats.lastRssiDbm) : "null";
    payload += ",\"packets_received\":";
    payload += String(espnowStats.packetsReceived);
    payload += ",\"packets_wrong_source\":";
    payload += String(espnowStats.packetsWrongSource);
    payload += ",\"packets_invalid\":";
    payload += String(espnowStats.packetsInvalidHeader);
    payload += ",\"rtcm_frames\":";
    payload += String(espnowStats.framesWritten);
    payload += ",\"rtcm_crc_errors\":";
    payload += String(espnowStats.crcErrors);
    payload += ",\"rtcm_queue_overflow\":";
    payload += String(espnowStats.queueOverflow);
    payload += ",\"rtcm_queue_hwm\":";
    payload += String(espnowStats.queueHighWater);
    payload += ",\"rtcm_duplicates\":";
    payload += String(espnowStats.duplicateFragments);
    payload += ",\"rtcm_timeouts\":";
    payload += String(espnowStats.frameTimeouts);
    payload += ",\"rtcm_sequence_gaps\":";
    payload += String(espnowStats.sequenceGaps);
    payload += ",\"uart_write_errors\":";
    payload += String(espnowStats.uartWriteErrors);
    payload += ",\"ack_queued\":";
    payload += String(espnowStats.ackPacketsQueued);
    payload += ",\"ack_send_fail\":";
    payload += String(espnowStats.ackSendFailures);
    payload += ",\"last_rtcm_age_ms\":";
    payload += hasRtcmFrame ? String(now - espnowStats.lastValidFrameMillis) : "null";
    payload += "}}";

    debugServer.send(200, "application/json", payload);
}

void handleNotFound() {
    debugServer.send(404, "text/plain", "Not found");
}

} // namespace

bool debugWebSetup() {
    if (debugWebReady) {
        return true;
    }

    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (!WiFi.softAPConfig(debugWebIp, debugWebGateway, debugWebSubnet)) {
        Serial.println("[DEBUG_WEB][ERROR] Khong cau hinh duoc SoftAP IP");
        return false;
    }

    const bool apStarted = WiFi.softAP(
        DEBUG_WEB_AP_SSID,
        DEBUG_WEB_AP_PASSWORD,
        ESPNOW_WIFI_CHANNEL,
        false,
        DEBUG_WEB_AP_MAX_CLIENTS);
    if (!apStarted) {
        Serial.println("[DEBUG_WEB][ERROR] Khong khoi dong duoc SoftAP");
        return false;
    }
    delay(100);

    const uint8_t apProtocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    const esp_err_t protocolResult = esp_wifi_set_protocol(WIFI_IF_AP, apProtocols);
    if (protocolResult != ESP_OK) {
        Serial.printf("[DEBUG_WEB][WARN] Khong dat duoc AP protocol B/G/N: %d\n", protocolResult);
    }

    debugServer.on("/", HTTP_GET, handleIndex);
    debugServer.on("/api/status", HTTP_GET, handleStatus);
    debugServer.onNotFound(handleNotFound);
    debugServer.begin();

    debugWebReady = true;
    Serial.printf("[DEBUG_WEB] SoftAP SSID=%s IP=%s channel=%u MAC=%s\n",
                  DEBUG_WEB_AP_SSID,
                  WiFi.softAPIP().toString().c_str(),
                  ESPNOW_WIFI_CHANNEL,
                  WiFi.softAPmacAddress().c_str());
    return true;
}

void debugWebLoop() {
    if (debugWebReady) {
        debugServer.handleClient();
    }
}

#else

bool debugWebSetup() {
    return false;
}

void debugWebLoop() {}

#endif
