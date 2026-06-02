#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ---- Compile-time config ----
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

static const uint32_t PUBLISH_INTERVAL_MS    = 5UL * 60UL * 1000UL;  // 5 minutes
static const uint32_t WIFI_CONNECT_TIMEOUT_S = 30;                   // first connect attempt
static const uint32_t PORTAL_TIMEOUT_S       = 5 * 60;               // captive portal stays up this long, then reboot
static const uint32_t MQTT_RECONNECT_MS      = 10UL * 1000UL;        // backoff between MQTT reconnect attempts

static const int      VEDIRECT_RX_PIN        = 16;                   // UART2 RX <- Victron TX
static const int      VEDIRECT_TX_PIN        = 17;                   // unused (read-only); reserved
static const uint32_t VEDIRECT_BAUD          = 19200;
static const size_t   VEDIRECT_RX_BUF        = 2048;                 // big enough to survive WiFi connect blocking
static const uint32_t VEDIRECT_IDLE_RESTART_MS = 5000;                // if no bytes for this long, restart UART

// ---- Persisted user config ----
struct UserConfig {
    String device_name;      // friendly name, used in MQTT topic path
    String mqtt_host;
    uint16_t mqtt_port;
    String mqtt_user;
    String mqtt_pass;
    String mqtt_base_topic;  // e.g. "victron"
};

static UserConfig cfg;
static Preferences prefs;

// ---- Runtime ----
static WiFiClient   net;
static PubSubClient mqtt(net);
static String       deviceId;            // "victron-a1b2c3"
static String       portalSsid;          // "Victron-A1B2C3"
static String       topicState;
static String       topicStatus;
static String       topicInfo;
static String       topicDebug;
static uint32_t     lastPublishMs       = 0;
static uint32_t     lastMqttAttemptMs   = 0;
static bool         shouldSaveConfig    = false;

// ---- VE.Direct parser ----
// Text frames look like (each line is <Tab>-separated key/value, terminated by \r\n):
//   PID\t0xA053\r\n
//   V\t13180\r\n
//   I\t1230\r\n
//   ...
//   Checksum\t<byte>\r\n
// A complete frame ends with a "Checksum" line. We accumulate fields and only swap them
// into `latestFrame` when the whole frame checksum-validates (sum of all bytes mod 256 == 0).
struct VEFrame {
    bool   valid = false;
    StaticJsonDocument<1024> doc;
};

static VEFrame latestFrame;
static StaticJsonDocument<1024> workingDoc;

enum VEState { VE_IDLE, VE_RECORD_NAME, VE_RECORD_VALUE, VE_CHECKSUM };
static VEState veState   = VE_IDLE;
static String  veName;
static String  veValue;
static uint16_t veSum    = 0; // running sum of all bytes in current frame

// ---- Diagnostic counters / ring buffer ----
static uint32_t veBytesTotal       = 0;
static uint32_t veFramesValid      = 0;
static uint32_t veFramesAttempted  = 0; // count of "Checksum" line ends (valid or not)
static uint32_t veLastByteMs       = 0;
static String   veLastCompleteKey;       // last key successfully added
static const size_t VE_RING_SIZE   = 64;
static uint8_t  veRing[VE_RING_SIZE] = {0};
static size_t   veRingHead         = 0;  // next write position
static size_t   veRingCount        = 0;  // how many valid bytes in ring (up to VE_RING_SIZE)

static void veRingPush(uint8_t b) {
    veRing[veRingHead] = b;
    veRingHead = (veRingHead + 1) % VE_RING_SIZE;
    if (veRingCount < VE_RING_SIZE) veRingCount++;
}

static String veRingAsHex() {
    String out;
    size_t start = (veRingHead + VE_RING_SIZE - veRingCount) % VE_RING_SIZE;
    char hex[4];
    for (size_t i = 0; i < veRingCount; i++) {
        uint8_t b = veRing[(start + i) % VE_RING_SIZE];
        snprintf(hex, sizeof(hex), "%02x ", b);
        out += hex;
    }
    return out;
}

static String veRingAsAscii() {
    String out;
    size_t start = (veRingHead + VE_RING_SIZE - veRingCount) % VE_RING_SIZE;
    for (size_t i = 0; i < veRingCount; i++) {
        uint8_t b = veRing[(start + i) % VE_RING_SIZE];
        if (b == '\r') out += "\\r";
        else if (b == '\n') out += "\\n";
        else if (b == '\t') out += "\\t";
        else if (b >= 32 && b < 127) out += (char)b;
        else { char tmp[6]; snprintf(tmp, sizeof(tmp), "\\x%02x", b); out += tmp; }
    }
    return out;
}

static const char* veStateName(VEState s) {
    switch (s) {
        case VE_IDLE:          return "IDLE";
        case VE_RECORD_NAME:   return "RECORD_NAME";
        case VE_RECORD_VALUE:  return "RECORD_VALUE";
        case VE_CHECKSUM:      return "CHECKSUM";
    }
    return "?";
}

static void veResetFrame() {
    workingDoc.clear();
    veName = "";
    veValue = "";
    veSum = 0;
    veState = VE_IDLE;
}

static void veFeedByte(uint8_t b) {
    veSum += b;

    switch (veState) {
    case VE_IDLE:
        if (b == '\r' || b == '\n') return;
        veName = "";
        veName += (char)b;
        veState = VE_RECORD_NAME;
        break;

    case VE_RECORD_NAME:
        if (b == '\t') {
            veValue = "";
            // The "Checksum" field's "value" is a single raw byte (not text)
            if (veName == "Checksum") veState = VE_CHECKSUM;
            else                       veState = VE_RECORD_VALUE;
        } else if (b == '\r' || b == '\n' || veName.length() > 40) {
            // Sync recovery: a stray newline or runaway field length means we're out of sync.
            // Abort the current frame (sum is poisoned anyway) and wait for the next field.
            veResetFrame();
        } else {
            veName += (char)b;
        }
        break;

    case VE_RECORD_VALUE:
        if (b == '\r') {
            // ignore \r; \n will follow and finish the line
        } else if (b == '\n') {
            workingDoc[veName] = veValue;
            veLastCompleteKey = veName;
            veName = "";
            veValue = "";
            veState = VE_IDLE;
        } else {
            veValue += (char)b;
        }
        break;

    case VE_CHECKSUM:
        // After the single checksum byte, the running sum (including all bytes from
        // the start of the frame plus this byte) should be 0 mod 256.
        veFramesAttempted++;
        if ((veSum & 0xFF) == 0) {
            latestFrame.doc = workingDoc;
            latestFrame.valid = true;
            veFramesValid++;
        }
        veResetFrame();
        break;
    }
}

static void veDirectStart() {
    Serial2.end();
    Serial2.setRxBufferSize(VEDIRECT_RX_BUF);
    Serial2.begin(VEDIRECT_BAUD, SERIAL_8N1, VEDIRECT_RX_PIN, VEDIRECT_TX_PIN);
    veResetFrame();
    veLastByteMs = millis(); // pretend a byte just arrived so the watchdog doesn't immediately retrigger
}

static void veDirectPoll() {
    while (Serial2.available()) {
        uint8_t b = (uint8_t)Serial2.read();
        veBytesTotal++;
        veLastByteMs = millis();
        veRingPush(b);
        veFeedByte(b);
    }
}

// ---- Config persistence ----
static void loadConfig() {
    prefs.begin("victron", true);
    cfg.device_name     = prefs.getString("dev_name", "");
    cfg.mqtt_host       = prefs.getString("mqtt_host", "");
    cfg.mqtt_port       = prefs.getUShort("mqtt_port", 1883);
    cfg.mqtt_user       = prefs.getString("mqtt_user", "");
    cfg.mqtt_pass       = prefs.getString("mqtt_pass", "");
    cfg.mqtt_base_topic = prefs.getString("mqtt_base",  "victron");
    prefs.end();
}

static void saveConfig() {
    prefs.begin("victron", false);
    prefs.putString("dev_name",  cfg.device_name);
    prefs.putString("mqtt_host", cfg.mqtt_host);
    prefs.putUShort("mqtt_port", cfg.mqtt_port);
    prefs.putString("mqtt_user", cfg.mqtt_user);
    prefs.putString("mqtt_pass", cfg.mqtt_pass);
    prefs.putString("mqtt_base", cfg.mqtt_base_topic);
    prefs.end();
}

// ---- Identity ----
static String macSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    // Last 3 bytes of MAC, hex, lowercase
    char buf[7];
    snprintf(buf, sizeof(buf), "%02x%02x%02x",
             (uint8_t)(mac >> 24) & 0xFF,
             (uint8_t)(mac >> 16) & 0xFF,
             (uint8_t)(mac >> 8)  & 0xFF);
    return String(buf);
}

static void computeIdentity() {
    String suffix = macSuffix();
    deviceId   = "victron-" + suffix;
    String upper = suffix; upper.toUpperCase();
    portalSsid = "Victron-" + upper;
}

static void computeTopics() {
    String name = cfg.device_name.length() ? cfg.device_name : deviceId;
    String base = cfg.mqtt_base_topic.length() ? cfg.mqtt_base_topic : "victron";
    topicState  = base + "/" + name + "/state";
    topicStatus = base + "/" + name + "/status";
    topicInfo   = base + "/" + name + "/info";
    topicDebug  = base + "/" + name + "/debug";
}

// ---- Wi-Fi / captive portal ----
static void runCaptivePortal(bool firstTime) {
    WiFiManager wm;
    wm.setDebugOutput(true);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);

    // Extra fields persisted in our Preferences (not in WiFiManager's own store)
    char portStr[6]; snprintf(portStr, sizeof(portStr), "%u", cfg.mqtt_port);
    WiFiManagerParameter p_devname   ("dev_name", "Device name (e.g. cabin-north)", cfg.device_name.c_str(),     32);
    WiFiManagerParameter p_mqtt_host ("mqtt_host","MQTT broker host",                cfg.mqtt_host.c_str(),       64);
    WiFiManagerParameter p_mqtt_port ("mqtt_port","MQTT broker port",                portStr,                      6);
    WiFiManagerParameter p_mqtt_user ("mqtt_user","MQTT username (optional)",        cfg.mqtt_user.c_str(),       32);
    WiFiManagerParameter p_mqtt_pass ("mqtt_pass","MQTT password (optional)",        cfg.mqtt_pass.c_str(),       64);
    WiFiManagerParameter p_mqtt_base ("mqtt_base","MQTT base topic",                 cfg.mqtt_base_topic.c_str(), 32);
    wm.addParameter(&p_devname);
    wm.addParameter(&p_mqtt_host);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);
    wm.addParameter(&p_mqtt_base);

    wm.setSaveConfigCallback([]() { shouldSaveConfig = true; });

    bool ok;
    if (firstTime) {
        // No saved Wi-Fi yet, or we want the portal: blocking start
        ok = wm.startConfigPortal(portalSsid.c_str());
    } else {
        // Try saved creds first, fall back to portal on failure
        wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
        ok = wm.autoConnect(portalSsid.c_str());
    }

    if (shouldSaveConfig) {
        cfg.device_name     = p_devname.getValue();
        cfg.mqtt_host       = p_mqtt_host.getValue();
        cfg.mqtt_port       = (uint16_t) atoi(p_mqtt_port.getValue());
        if (cfg.mqtt_port == 0) cfg.mqtt_port = 1883;
        cfg.mqtt_user       = p_mqtt_user.getValue();
        cfg.mqtt_pass       = p_mqtt_pass.getValue();
        cfg.mqtt_base_topic = p_mqtt_base.getValue();
        if (cfg.mqtt_base_topic.length() == 0) cfg.mqtt_base_topic = "victron";
        saveConfig();
        shouldSaveConfig = false;
        Serial.println(F("[cfg] saved"));
    }

    if (!ok) {
        Serial.println(F("[wifi] portal timed out, rebooting to retry"));
        delay(500);
        ESP.restart();
    }
    Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

// ---- MQTT ----
static bool mqttEnsureConnected() {
    if (mqtt.connected()) return true;
    if (cfg.mqtt_host.length() == 0) return false;

    uint32_t now = millis();
    if (now - lastMqttAttemptMs < MQTT_RECONNECT_MS && lastMqttAttemptMs != 0) return false;
    lastMqttAttemptMs = now;

    mqtt.setServer(cfg.mqtt_host.c_str(), cfg.mqtt_port);
    Serial.printf("[mqtt] connecting to %s:%u ...\n", cfg.mqtt_host.c_str(), cfg.mqtt_port);

    bool ok;
    const char* user = cfg.mqtt_user.length() ? cfg.mqtt_user.c_str() : nullptr;
    const char* pass = cfg.mqtt_pass.length() ? cfg.mqtt_pass.c_str() : nullptr;
    // Last-will: retained "offline" so the broker auto-marks us down on disconnect
    ok = mqtt.connect(deviceId.c_str(), user, pass,
                      topicStatus.c_str(), 0, true, "offline");
    if (!ok) {
        Serial.printf("[mqtt] failed, state=%d\n", mqtt.state());
        return false;
    }

    Serial.println(F("[mqtt] connected"));
    mqtt.publish(topicStatus.c_str(), "online", true);

    StaticJsonDocument<256> info;
    info["device_id"] = deviceId;
    info["name"]      = cfg.device_name.length() ? cfg.device_name : deviceId;
    info["version"]   = FIRMWARE_VERSION;
    info["ip"]        = WiFi.localIP().toString();
    info["mac"]       = WiFi.macAddress();
    info["event"]     = "boot";
    char buf[256];
    size_t n = serializeJson(info, buf, sizeof(buf));
    mqtt.publish(topicInfo.c_str(), (uint8_t*)buf, n, true);

    return true;
}

static void publishDebug() {
    StaticJsonDocument<768> d;
    d["uptime_s"]          = millis() / 1000;
    d["bytes_seen"]        = veBytesTotal;
    d["frames_attempted"]  = veFramesAttempted;
    d["frames_valid"]      = veFramesValid;
    d["parser_state"]      = veStateName(veState);
    d["last_complete_key"] = veLastCompleteKey;
    d["ms_since_last_byte"]= (veLastByteMs == 0) ? -1 : (long)(millis() - veLastByteMs);
    d["ring_ascii"]        = veRingAsAscii();
    d["ring_hex"]          = veRingAsHex();
    char buf[896];
    size_t n = serializeJson(d, buf, sizeof(buf));
    bool ok = mqtt.publish(topicDebug.c_str(), (uint8_t*)buf, n, false);
    Serial.printf("[dbg] bytes=%u attempted=%u valid=%u state=%s last_key=%s ms_since_byte=%ld pub_ok=%d\n",
                  (unsigned)veBytesTotal, (unsigned)veFramesAttempted, (unsigned)veFramesValid,
                  veStateName(veState), veLastCompleteKey.c_str(),
                  (veLastByteMs == 0) ? -1L : (long)(millis() - veLastByteMs),
                  ok ? 1 : 0);
}

// ---- VE.Direct enum decoders ----
// Keep these short and human-readable; they go straight into Node-RED status widgets.
static const char* csText(int cs) {
    switch (cs) {
        case 0:   return "Off";
        case 2:   return "Fault";
        case 3:   return "Bulk";
        case 4:   return "Absorption";
        case 5:   return "Float";
        case 6:   return "Storage";
        case 7:   return "Equalize";
        case 245: return "Starting up";
        case 247: return "Auto equalize";
        case 252: return "External control";
        default:  return "Unknown";
    }
}

static const char* mpptText(int m) {
    switch (m) {
        case 0: return "Off";
        case 1: return "Voltage/current limited";
        case 2: return "MPPT active";
        default: return "Unknown";
    }
}

static const char* errText(int e) {
    switch (e) {
        case 0:   return "OK";
        case 2:   return "Battery voltage too high";
        case 17:  return "Charger temp too high";
        case 18:  return "Charger over current";
        case 19:  return "Charger reverse current";
        case 20:  return "Bulk time limit exceeded";
        case 21:  return "Current sensor issue";
        case 26:  return "Terminals overheated";
        case 33:  return "Input voltage too high";
        case 34:  return "Input current too high";
        case 38:  return "Input shutdown (battery)";
        case 39:  return "Input shutdown (current)";
        case 65:  return "Comms warning";
        case 67:  return "BMS connection lost";
        case 116: return "Factory cal lost";
        case 117: return "Invalid firmware";
        case 119: return "User settings invalid";
        default:  return "Unknown error";
    }
}

static void publishState() {
    publishDebug();
    if (!latestFrame.valid) {
        Serial.println(F("[pub] no valid frame yet, skipping state"));
        return;
    }

    JsonObject src = latestFrame.doc.as<JsonObject>();
    auto hasK   = [&](const char* k) { return src.containsKey(k); };
    auto getS   = [&](const char* k) -> const char* { return hasK(k) ? src[k].as<const char*>() : ""; };
    auto getL   = [&](const char* k) -> long        { return hasK(k) ? atol(src[k].as<const char*>()) : 0; };
    // Round to N decimals to keep JSON output tidy (avoids 13.229999... artifacts)
    auto round2 = [](float v) { return roundf(v * 100.0f)  / 100.0f;  };
    auto round3 = [](float v) { return roundf(v * 1000.0f) / 1000.0f; };

    StaticJsonDocument<1024> out;

    // Identification (helpful but doesn't change between publishes; harmless to repeat)
    if (hasK("PID"))  out["pid"]     = getS("PID");
    if (hasK("FW"))   out["fw"]      = getS("FW");
    if (hasK("SER#")) out["sn"]      = getS("SER#");
    if (hasK("HSDS")) out["day_seq"] = (int)getL("HSDS");

    // Battery: V is mV, I is mA. Compute power V*I in watts.
    if (hasK("V")) {
        float bv = getL("V") / 1000.0f;
        out["battery_v"] = round2(bv);
        if (hasK("I")) {
            float bi = getL("I") / 1000.0f;
            out["battery_i_a"] = round3(bi);
            out["battery_p_w"] = round2(bv * bi);
        }
    }

    // Panel: VPV is mV, PPV is already W
    if (hasK("VPV")) out["panel_v"]   = round2(getL("VPV") / 1000.0f);
    if (hasK("PPV")) out["panel_p_w"] = (int)getL("PPV");

    // Yield counters: H19/H20/H22 are in 0.01 kWh, H21/H23 are W
    if (hasK("H19")) out["yield_total_kwh"]        = round2(getL("H19") / 100.0f);
    if (hasK("H20")) out["yield_today_kwh"]        = round2(getL("H20") / 100.0f);
    if (hasK("H21")) out["yield_today_peak_w"]     = (int)getL("H21");
    if (hasK("H22")) out["yield_yesterday_kwh"]    = round2(getL("H22") / 100.0f);
    if (hasK("H23")) out["yield_yesterday_peak_w"] = (int)getL("H23");

    // Load output (some models only): LOAD is ON/OFF, IL is mA
    if (hasK("LOAD")) out["load"]      = getS("LOAD");
    if (hasK("IL"))   out["load_i_a"]  = round3(getL("IL") / 1000.0f);

    // State / status fields with human-readable labels alongside the raw int
    if (hasK("CS")) {
        int v = (int)getL("CS");
        out["cs"]      = v;
        out["cs_text"] = csText(v);
    }
    if (hasK("MPPT")) {
        int v = (int)getL("MPPT");
        out["mppt"]      = v;
        out["mppt_text"] = mpptText(v);
    }
    if (hasK("ERR")) {
        int v = (int)getL("ERR");
        out["err"]      = v;
        out["err_text"] = errText(v);
    }
    if (hasK("OR")) out["or_bitmask"] = getS("OR");

    out["ts_ms"]   = millis();
    out["version"] = FIRMWARE_VERSION;

    char buf[1024];
    size_t n = serializeJson(out, buf, sizeof(buf));
    if (mqtt.publish(topicState.c_str(), (uint8_t*)buf, n, false)) {
        Serial.printf("[pub] %s (%u bytes)\n", topicState.c_str(), (unsigned)n);
    } else {
        Serial.println(F("[pub] publish failed"));
    }
}

// ---- Arduino entry points ----
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.printf("== Victron VE.Direct -> MQTT bridge v%s ==\n", FIRMWARE_VERSION);

    veDirectStart();

    loadConfig();
    computeIdentity();
    Serial.printf("[id] deviceId=%s portalSsid=%s\n", deviceId.c_str(), portalSsid.c_str());

    // Always go through WiFiManager.autoConnect: if creds saved, it connects; otherwise it opens the portal.
    runCaptivePortal(false);

    computeTopics();
    Serial.printf("[mqtt] topics: state=%s status=%s info=%s debug=%s\n",
                  topicState.c_str(), topicStatus.c_str(), topicInfo.c_str(), topicDebug.c_str());

    mqtt.setBufferSize(1024);
    // Publish on next loop tick once MQTT is up
    lastPublishMs = 0;
}

void loop() {
    // If Wi-Fi drops, re-open the portal (blocks for up to PORTAL_TIMEOUT_S then reboots)
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[wifi] lost, opening portal"));
        runCaptivePortal(false);
    }

    veDirectPoll();

    // UART self-heal: if no bytes for VEDIRECT_IDLE_RESTART_MS, the RX path may have wedged
    // (e.g. FIFO overrun during the WiFi connect blocking phase). Tear down and re-init.
    if (millis() - veLastByteMs > VEDIRECT_IDLE_RESTART_MS) {
        Serial.println(F("[ve] no bytes for too long, restarting Serial2"));
        veDirectStart();
    }

    if (mqttEnsureConnected()) {
        mqtt.loop();
        uint32_t now = millis();
        if (now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
            lastPublishMs = now;
            publishState();
        }
    }
}
