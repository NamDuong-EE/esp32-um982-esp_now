#include "hardware/DebugWeb_handler.h"

#include "Prog_Config.h"

#if DEBUG_WEB_ENABLED

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "functions/Gnss_Command_Handler.h"
#include "hardware/Relay_handler.h"
#include "helper.h"

namespace {

WebServer debugServer(80);
bool debugWebReady = false;
const IPAddress debugWebIp(192, 168, 4, 1);
const IPAddress debugWebGateway(192, 168, 4, 1);
const IPAddress debugWebSubnet(255, 255, 255, 0);

const char NORMAL_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Rover Debug</title><style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#1c2526;background:#f5f7f8}body{margin:0;padding:20px}main{max-width:780px;margin:auto}h1{margin:0}.sub{color:#607074}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}.card,table{background:#fff;border:1px solid #d9e1e3;border-radius:8px}.card{padding:14px}.label{font-size:12px;color:#68787c;text-transform:uppercase}.value{font-size:24px;font-weight:700;margin-top:6px}.health{margin-top:16px}table{width:100%;border-collapse:collapse;overflow:hidden}td{padding:10px 12px;border-bottom:1px solid #edf1f2}td:last-child{text-align:right;font-weight:600}.ok{color:#087f5b}.warn{color:#b35c00}.bad{color:#c92a2a}
</style></head><body><main><h1>ESP32 Rover Debug</h1><p class="sub">Normal mode · 192.168.4.1</p>
<section class="grid"><div class="card"><div class="label">Latitude</div><div id="lat" class="value">--</div></div><div class="card"><div class="label">Longitude</div><div id="lon" class="value">--</div></div><div class="card"><div class="label">Height</div><div id="height" class="value">--</div></div><div class="card"><div class="label">Fix quality</div><div id="fix" class="value">--</div></div><div class="card"><div class="label">Satellites</div><div id="sats" class="value">--</div></div></section>
<section class="health"><table><tbody id="health"></tbody></table></section></main><script>
const fields=["espnow_ready","base_provisioned","pairing_active","pair_confirm_ok","rtk_correction_held","rtcm_frames","last_rtcm_age_ms","rtcm_crc_errors","rtcm_queue_overflow","rtcm_sequence_gaps","ack_queued","ack_send_fail","free_heap_bytes"];
const text=v=>v===null||v===undefined?"--":v;
async function refresh(){try{const d=await(await fetch('/api/status',{cache:'no-store'})).json();lat.textContent=d.gga.valid?d.gga.lat.toFixed(7):'--';lon.textContent=d.gga.valid?d.gga.lon.toFixed(7):'--';height.textContent=d.gga.valid?d.gga.height_m.toFixed(3)+' m':'--';fix.textContent=d.gga.valid?d.gga.fix_quality:'--';fix.className='value '+(d.gga.fix_quality===4?'ok':d.gga.fix_quality===5?'warn':'bad');sats.textContent=d.gga.valid?d.gga.satellites:'--';health.innerHTML=fields.map(k=>`<tr><td>${k}</td><td>${text(d.health[k])}</td></tr>`).join('')}catch(e){health.innerHTML='<tr><td>status</td><td>offline</td></tr>'}}refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

const char RELAY_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Rover Relay</title><style>
:root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#172126;background:#eef3f4}body{margin:0;padding:20px}main{max-width:960px;margin:auto}h1{margin:0}.sub{color:#607074}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-top:18px}.card,table{background:#fff;border:1px solid #d4dfe1;border-radius:8px}.card{padding:14px}.label{font-size:12px;color:#68787c;text-transform:uppercase}.value{font-size:22px;font-weight:700;margin-top:6px;word-break:break-word}.columns{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-top:16px}h2{font-size:17px;margin:0 0 8px}table{width:100%;border-collapse:collapse;overflow:hidden}td{padding:9px 11px;border-bottom:1px solid #edf1f2}td:last-child{text-align:right;font-weight:600}.ok{color:#087f5b}.warn{color:#b35c00}.bad{color:#c92a2a}@media(max-width:700px){.columns{grid-template-columns:1fr}}
</style></head><body><main><h1>ESP32 Rover Relay</h1><p class="sub">Relay mode · 192.168.4.1</p>
<section class="grid"><div class="card"><div class="label">Latitude</div><div id="lat" class="value">--</div></div><div class="card"><div class="label">Longitude</div><div id="lon" class="value">--</div></div><div class="card"><div class="label">Height</div><div id="height" class="value">--</div></div><div class="card"><div class="label">Fix quality</div><div id="fix" class="value">--</div></div><div class="card"><div class="label">Satellites</div><div id="sats" class="value">--</div></div></section>
<div class="columns"><section><h2>Upstream · Base → Relay</h2><table><tbody id="upstream"></tbody></table></section><section><h2>Downstream · Relay → Child</h2><table><tbody id="downstream"></tbody></table></section></div></main><script>
const up=["paired","mac","pairing_active","rtk_correction_held","frames_received","crc_errors","queue_overflow","acks_sent","last_rtcm_age_ms"];
const down=["paired","mac","pairing_active","frames_queued","frames_sent","frames_acked","fragments_sent","frame_retries","ack_timeouts","send_failures","send_immediate_errors","send_callback_timeouts","send_delivery_failures","backoff_events","frames_without_child","frames_suppressed_pairing","last_ack_age_ms"];
const text=v=>v===null||v===undefined||v===''?'--':v;const rows=(o,keys)=>keys.map(k=>`<tr><td>${k}</td><td>${text(o[k])}</td></tr>`).join('');
async function refresh(){try{const d=await(await fetch('/api/relay/status',{cache:'no-store'})).json();lat.textContent=d.device.gga_valid?d.device.lat.toFixed(7):'--';lon.textContent=d.device.gga_valid?d.device.lon.toFixed(7):'--';height.textContent=d.device.gga_valid?d.device.height_m.toFixed(3)+' m':'--';fix.textContent=d.device.gga_valid?d.device.fix_quality:'--';fix.className='value '+(d.device.fix_quality===4?'ok':d.device.fix_quality===5?'warn':'bad');sats.textContent=d.device.gga_valid?d.device.satellites:'--';upstream.innerHTML=rows(d.upstream,up);downstream.innerHTML=rows(d.downstream,down)}catch(e){upstream.innerHTML='<tr><td>status</td><td>offline</td></tr>';downstream.innerHTML=''}}refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

String macText(const uint8_t mac[6], bool valid) {
    if (!valid) {
        return String();
    }
    char value[18];
    snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(value);
}

void appendGga(String& payload, const GgaDebugSnapshot& gga, uint32_t now) {
    payload += "\"valid\":";
    payload += gga.valid ? "true" : "false";
    payload += ",\"lat\":" + String(gga.lat, 7);
    payload += ",\"lon\":" + String(gga.lon, 7);
    payload += ",\"height_m\":" + String(gga.heightM, 3);
    payload += ",\"ellipsoid_height_m\":" + String(gga.ellipsoidHeightM, 3);
    payload += ",\"fix_quality\":" + String(gga.fixQuality);
    payload += ",\"satellites\":" + String(gga.satellites);
    payload += ",\"last_gga_age_ms\":";
    payload += gga.valid ? String(now - gga.lastUpdateMs) : "null";
}

void handleNormalIndex() {
    debugServer.send_P(200, "text/html", NORMAL_INDEX_HTML);
}

void handleRelayIndex() {
    debugServer.send_P(200, "text/html", RELAY_INDEX_HTML);
}

void handleNormalStatus() {
    const GgaDebugSnapshot gga = getGgaDebugSnapshot();
    const EspNowRtcmStats s = espnowGetStats();
    const GnssCommandStats command = gnssCommandGetStats();
    const uint32_t now = millis();
    uint8_t baseMac[6] = {};
    const bool hasBase = espnowGetBaseMac(baseMac);
    String payload;
    payload.reserve(1200);
    payload = "{\"mode\":\"normal\",\"gga\":{";
    appendGga(payload, gga, now);
    payload += "},\"health\":{";
    payload += "\"uptime_s\":" + String(now / 1000U);
    payload += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
    payload += ",\"espnow_ready\":" + String(espnowIsReady() ? "true" : "false");
    payload += ",\"base_provisioned\":" + String(hasBase ? "true" : "false");
    payload += ",\"base_mac\":\"" + macText(baseMac, hasBase) + "\"";
    payload += ",\"pairing_active\":" + String(s.pairingActive ? "true" : "false");
    payload += ",\"pair_confirm_ok\":" + String(s.pairConfirmsAccepted);
    payload += ",\"rtk_correction_held\":" +
               String(command.rtkCorrectionHeld ? "true" : "false");
    payload += ",\"rtcm_frames\":" + String(s.framesWritten);
    payload += ",\"rtcm_crc_errors\":" + String(s.crcErrors);
    payload += ",\"rtcm_queue_overflow\":" + String(s.queueOverflow);
    payload += ",\"rtcm_sequence_gaps\":" + String(s.sequenceGaps);
    payload += ",\"ack_queued\":" + String(s.ackPacketsQueued);
    payload += ",\"ack_send_fail\":" + String(s.ackSendFailures);
    payload += ",\"last_rtcm_age_ms\":";
    payload += s.lastValidFrameMillis == 0 ? "null" : String(now - s.lastValidFrameMillis);
    payload += "}}";
    debugServer.send(200, "application/json", payload);
}

void handleRelayStatus() {
    const GgaDebugSnapshot gga = getGgaDebugSnapshot();
    const EspNowRtcmStats upstream = espnowGetStats();
    const GnssCommandStats command = gnssCommandGetStats();
    const RelayStats downstream = relayGetStats();
    RelayChildStatus children[RELAY_MAX_CHILDREN] = {};
    const size_t childCount = relayCopyChildren(children, RELAY_MAX_CHILDREN);
    const uint32_t now = millis();
    uint8_t baseMac[6] = {};
    uint8_t childMac[6] = {};
    const bool hasBase = espnowGetBaseMac(baseMac);
    const bool hasChild = relayGetChildMac(childMac);
    String payload;
    payload.reserve(3200);
    payload = "{\"mode\":\"relay\",\"device\":{";
    payload += "\"gga_valid\":" + String(gga.valid ? "true" : "false");
    payload += ",\"lat\":" + String(gga.lat, 7);
    payload += ",\"lon\":" + String(gga.lon, 7);
    payload += ",\"height_m\":" + String(gga.heightM, 3);
    payload += ",\"ellipsoid_height_m\":" + String(gga.ellipsoidHeightM, 3);
    payload += ",\"fix_quality\":" + String(gga.fixQuality);
    payload += ",\"satellites\":" + String(gga.satellites);
    payload += ",\"uptime_s\":" + String(now / 1000U);
    payload += "},\"upstream\":{";
    payload += "\"paired\":" + String(hasBase ? "true" : "false");
    payload += ",\"mac\":\"" + macText(baseMac, hasBase) + "\"";
    payload += ",\"pairing_active\":" + String(upstream.pairingActive ? "true" : "false");
    payload += ",\"rtk_correction_held\":" +
               String(command.rtkCorrectionHeld ? "true" : "false");
    payload += ",\"frames_received\":" + String(upstream.framesWritten);
    payload += ",\"crc_errors\":" + String(upstream.crcErrors);
    payload += ",\"queue_overflow\":" + String(upstream.queueOverflow);
    payload += ",\"acks_sent\":" + String(upstream.ackPacketsQueued);
    payload += ",\"last_rtcm_age_ms\":";
    payload += upstream.lastValidFrameMillis == 0 ? "null" : String(now - upstream.lastValidFrameMillis);
    payload += "},\"downstream\":{";
    payload += "\"paired\":" + String(hasChild ? "true" : "false");
    payload += ",\"mac\":\"" + macText(childMac, hasChild) + "\"";
    payload += ",\"pairing_active\":" + String(downstream.childPairingActive ? "true" : "false");
    payload += ",\"frames_queued\":" + String(downstream.framesQueued);
    payload += ",\"frames_sent\":" + String(downstream.framesSent);
    payload += ",\"frames_acked\":" + String(downstream.framesAcked);
    payload += ",\"fragments_sent\":" + String(downstream.fragmentsSent);
    payload += ",\"frame_retries\":" + String(downstream.frameRetries);
    payload += ",\"ack_timeouts\":" + String(downstream.ackTimeouts);
    payload += ",\"send_failures\":" + String(downstream.sendFailures);
    payload += ",\"send_immediate_errors\":" + String(downstream.sendImmediateErrors);
    payload += ",\"send_callback_timeouts\":" + String(downstream.sendCallbackTimeouts);
    payload += ",\"send_delivery_failures\":" + String(downstream.sendDeliveryFailures);
    payload += ",\"backoff_events\":" + String(downstream.backoffEvents);
    payload += ",\"frames_without_child\":" + String(downstream.framesWithoutChild);
    payload += ",\"frames_suppressed_pairing\":" + String(downstream.framesSuppressedDuringPairing);
    payload += ",\"child_count\":" + String(childCount);
    payload += ",\"child_clear_events\":" + String(downstream.childClearEvents);
    payload += ",\"frames_skipped_cooldown\":" + String(downstream.framesSkippedCooldown);
    payload += ",\"children\":[";
    for (size_t index = 0; index < childCount; ++index) {
        if (index > 0) {
            payload += ',';
        }
        payload += "{\"mac\":\"" + macText(children[index].mac, true) + "\"";
        payload += ",\"frames_sent\":" + String(children[index].framesSent);
        payload += ",\"frames_acked\":" + String(children[index].framesAcked);
        payload += ",\"frame_retries\":" + String(children[index].frameRetries);
        payload += ",\"ack_timeouts\":" + String(children[index].ackTimeouts);
        payload += ",\"send_failures\":" + String(children[index].sendFailures);
        payload += ",\"frames_skipped_cooldown\":" + String(children[index].framesSkippedCooldown);
        payload += ",\"consecutive_failures\":" + String(children[index].consecutiveFailures);
        payload += ",\"cooldown_remaining_ms\":";
        payload += children[index].cooldownUntilMs != 0 &&
                           static_cast<int32_t>(children[index].cooldownUntilMs - now) > 0
                       ? String(children[index].cooldownUntilMs - now)
                       : "0";
        payload += ",\"llh_received\":" + String(children[index].llhReceived);
        payload += ",\"llh_forwarded\":" + String(children[index].llhForwarded);
        payload += ",\"last_ack_age_ms\":";
        payload += children[index].lastAckMillis == 0
                       ? "null"
                       : String(now - children[index].lastAckMillis);
        payload += '}';
    }
    payload += ']';
    payload += ",\"last_ack_age_ms\":";
    payload += downstream.lastAckMillis == 0 ? "null" : String(now - downstream.lastAckMillis);
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
    const char* ssid = ROVER_RELAY_MODE ? DEBUG_WEB_RELAY_AP_SSID : DEBUG_WEB_AP_SSID;
    if (!WiFi.softAP(ssid, DEBUG_WEB_AP_PASSWORD, ESPNOW_WIFI_CHANNEL,
                     false, DEBUG_WEB_AP_MAX_CLIENTS)) {
        Serial.println("[DEBUG_WEB][ERROR] Khong khoi dong duoc SoftAP");
        return false;
    }
    delay(100);
    const uint8_t apProtocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    const esp_err_t protocolResult = esp_wifi_set_protocol(WIFI_IF_AP, apProtocols);
    if (protocolResult != ESP_OK) {
        Serial.printf("[DEBUG_WEB][WARN] Khong dat duoc AP protocol B/G/N: %d\n", protocolResult);
    }
    if constexpr (ROVER_RELAY_MODE) {
        debugServer.on("/", HTTP_GET, handleRelayIndex);
        debugServer.on("/api/relay/status", HTTP_GET, handleRelayStatus);
    } else {
        debugServer.on("/", HTTP_GET, handleNormalIndex);
        debugServer.on("/api/status", HTTP_GET, handleNormalStatus);
    }
    debugServer.onNotFound(handleNotFound);
    debugServer.begin();
    debugWebReady = true;
    Serial.printf("[DEBUG_WEB] mode=%s SSID=%s IP=%s channel=%u MAC=%s\n",
                  ROVER_RELAY_MODE ? "relay" : "normal", ssid,
                  WiFi.softAPIP().toString().c_str(), ESPNOW_WIFI_CHANNEL,
                  WiFi.softAPmacAddress().c_str());
    return true;
}

void debugWebLoop() {
    if (debugWebReady) {
        debugServer.handleClient();
    }
}

#else

bool debugWebSetup() { return false; }
void debugWebLoop() {}

#endif
