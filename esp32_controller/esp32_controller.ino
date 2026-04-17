#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <DHT.h>

#define DHTPIN 5
#define DHTTYPE DHT22
#define SERVO_PIN       18
#define RELAY_LAMP_PIN  32
#define RELAY_PUMP_PIN  33

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ── KONFIGURASI SERVO ────────────────────────────────────────
#define SERVO_IDLE   90    // Posisi tutup  (idle)
#define SERVO_OPEN  180    // Posisi buka   (ngasih makan)

// ── THRESHOLD SUHU LAMPU (°C) ────────────────────────────────
// Lampu NYALA jika: TEMP_ON ≤ suhu < TEMP_OFF
// Lampu MATI  jika: suhu < TEMP_ON  ATAU  suhu ≥ TEMP_OFF
#define TEMP_LAMP_ON   30.0f
#define TEMP_LAMP_OFF  35.0f

// ── WIFI ─────────────────────────────────────────────────────
const char* WIFI_SSID     = "PRAXIS-TNJ";
const char* WIFI_PASSWORD = "@techno@231";

// ── MQTT (HiveMQ Cloud) ──────────────────────────────────────
const char* MQTT_SERVER   = "601011583e8349a79985f364d6cbae47.s1.eu.hivemq.cloud";
const int   MQTT_PORT     = 8883;
const char* MQTT_USERNAME = "IoTeam";
const char* MQTT_PASSWORD = "Roostech123";
const char* CLIENT_ID     = "esp32-controller";

// ── MQTT TOPICS ──────────────────────────────────────────────
const char* TOPIC_COMMAND   = "esp32/command";
const char* TOPIC_TELEMETRY = "esp32/telemetry";
const char* TOPIC_STATUS    = "esp32/status";

// ── INTERVAL ─────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL = 5000UL;   // 5 detik
const unsigned long RECONNECT_INTERVAL = 5000UL;

// ================================================================
//  GLOBALS
// ================================================================
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);
DHT dht (DHTPIN, DHTTYPE);
Servo            feederServo;

unsigned long lastTelemetryMs  = 0;
unsigned long lastReconnectMs  = 0;

// State subsystem
bool  lampState  = false;
bool  pumpState  = false;
float lastTemp   = 0.0f;

// ================================================================
//  FORWARD DECLARATIONS
// ================================================================
void connectWiFi();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishStatus(const char* state);
void publishTelemetry(float temp, float humi, int rssi);
void handleCommand(const char* payload, unsigned int length);
void setLamp(bool on, const char* by = "manual");
void setPump(bool on);
void doFeeding(int durationSec);
void checkAutoLamp(float temp);

// ================================================================
//  SETUP
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n========================================");
    Serial.println("  ESP32 IoT Controller — Starting up");
    Serial.println("========================================");

    // ── GPIO init ────────────────────────────────────────────
    pinMode(RELAY_LAMP_PIN, OUTPUT);
    pinMode(RELAY_PUMP_PIN, OUTPUT);
    digitalWrite(RELAY_LAMP_PIN, RELAY_OFF);   // Pastikan relay mati saat boot
    digitalWrite(RELAY_PUMP_PIN, RELAY_OFF);

    // ── Servo init ───────────────────────────────────────────
    feederServo.attach(SERVO_PIN);
    feederServo.write(SERVO_IDLE);
    Serial.println("[Servo]  Posisi idle 90°");

    // ── DHT init ─────────────────────────────────────────────
    dht.begin();
    Serial.println("[DHT]    Sensor diinisialisasi");

    // ── WiFi ─────────────────────────────────────────────────
    connectWiFi();

    // ── MQTT TLS setup ───────────────────────────────────────
    // setInsecure() → skip verifikasi sertifikat CA.
    // Aman untuk development. Untuk produksi: pakai setCACert(hivemq_root_ca)
    secureClient.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);   // Naikkan jika payload besar

    connectMQTT();
    Serial.println("\n[Boot]   Setup selesai!");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
    // ── MQTT keepalive & reconnect ───────────────────────────
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectMs >= RECONNECT_INTERVAL) {
            lastReconnectMs = now;
            Serial.println("[MQTT]   Koneksi terputus, reconnect...");
            connectMQTT();
        }
        return;   // Jangan proses apapun jika belum connect
    }
    mqttClient.loop();

    // ── Telemetri berkala ────────────────────────────────────
    unsigned long now = millis();
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL) {
        lastTelemetryMs = now;
        readAndPublishSensor();
    }
}

// ================================================================
//  SENSOR: Baca DHT & kirim telemetri
// ================================================================
void readAndPublishSensor() {
    float temp = dht.readTemperature();
    float humi = dht.readHumidity();

    if (isnan(temp) || isnan(humi)) {
        Serial.println("[DHT]  !! Gagal baca sensor — cek kabel!");
        return;
    }

    lastTemp = temp;
    Serial.printf("[DHT]    Suhu: %.1f°C | Humidity: %.1f%%\n", temp, humi);

    // Cek apakah lampu perlu auto-toggle berdasarkan suhu
    checkAutoLamp(temp);

    // Kirim telemetri ke MQTT (dan diteruskan ke Discord oleh bot)
    publishTelemetry(temp, humi, WiFi.RSSI());
}

// ================================================================
//  WIFI
// ================================================================
void connectWiFi() {
    Serial.printf("[WiFi]   Menghubungkan ke: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempt > 40) {
            Serial.println("\n[WiFi] !! Timeout, restart...");
            ESP.restart();
        }
    }
    Serial.printf("\n[WiFi]   Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ================================================================
//  MQTT — Connect
// ================================================================
void connectMQTT() {
    // Last Will Testament → bot Discord tahu ESP32 offline
    StaticJsonDocument<64> willDoc;
    willDoc["state"] = "OFFLINE";
    char willPayload[64];
    serializeJson(willDoc, willPayload);

    bool ok = mqttClient.connect(
        CLIENT_ID,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        TOPIC_STATUS,    // LWT topic
        1,               // LWT QoS
        true,            // LWT retain
        willPayload      // LWT payload
    );

    if (ok) {
        Serial.println("[MQTT]   Terhubung ke broker!");
        mqttClient.subscribe(TOPIC_COMMAND, 1);
        Serial.printf("[MQTT]   Subscribed: %s\n", TOPIC_COMMAND);
        publishStatus("ONLINE");
    } else {
        Serial.printf("[MQTT] !! Gagal, state=%d. Coba lagi...\n", mqttClient.state());
    }
}

// ================================================================
//  MQTT — Callback (pesan masuk dari Discord bot)
// ================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Cetak payload ke Serial untuk debug
    Serial.printf("[MQTT IN] [%s]: ", topic);
    for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
    Serial.println();

    if (strcmp(topic, TOPIC_COMMAND) == 0) {
        handleCommand((const char*)payload, length);
    }
}

// ================================================================
//  PUBLISH HELPERS
// ================================================================
void publishStatus(const char* state) {
    StaticJsonDocument<64> doc;
    doc["state"] = state;
    char buf[64];
    serializeJson(doc, buf);
    // retain=true → broker simpan pesan, subscriber baru langsung tahu statusnya
    mqttClient.publish(TOPIC_STATUS, buf, true);
    Serial.printf("[PUB]    Status → %s\n", state);
}

void publishTelemetry(float temp, float humi, int rssi) {
    StaticJsonDocument<192> doc;
    doc["temperature"] = serialized(String(temp, 1));
    doc["humadity"]    = serialized(String(humi, 1));   // sengaja typo — cocokkan dengan config.py
    doc["rssi"]        = rssi;
    doc["lamp"]        = lampState ? "ON" : "OFF";
    doc["pump"]        = pumpState ? "ON" : "OFF";

    char buf[192];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
    Serial.printf("[PUB]    Telemetry → %s\n", buf);
}

// ================================================================
//  COMMAND HANDLER
//  Format JSON dari Discord bot:
//    ping        : {"command":"ping"}
//    relay lampu : {"command":"relay","pin":"1","value":"on"}
//    relay pump  : {"command":"relay","pin":"2","value":"on"}
//    feeding     : {"command":"servo","value":"start","duration":5}
// ================================================================
void handleCommand(const char* payload, unsigned int length) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);

    if (err) {
        Serial.printf("[CMD] !! JSON error: %s\n", err.c_str());
        return;
    }

    const char* cmd = doc["command"] | "";

    // ── PING ─────────────────────────────────────────────────
    if (strcmp(cmd, "ping") == 0) {
        StaticJsonDocument<128> resp;
        resp["response"]    = "pong";
        resp["temperature"] = serialized(String(lastTemp, 1));
        resp["rssi"]        = WiFi.RSSI();
        resp["lamp"]        = lampState ? "ON" : "OFF";
        resp["pump"]        = pumpState ? "ON" : "OFF";
        char buf[128];
        serializeJson(resp, buf);
        mqttClient.publish(TOPIC_TELEMETRY, buf);
        Serial.println("[CMD]    Pong dikirim!");
    }

    // ── RELAY ─────────────────────────────────────────────────
    else if (strcmp(cmd, "relay") == 0) {
        const char* pin   = doc["pin"]   | "";
        const char* value = doc["value"] | "";
        bool turnOn = (strcmp(value, "on") == 0);

        if (strcmp(pin, "1") == 0) {
            // Pin 1 = Lampu (bisa override otomatis)
            setLamp(turnOn, "discord");

        } else if (strcmp(pin, "2") == 0) {
            // Pin 2 = Water Pump
            setPump(turnOn);

        } else {
            Serial.printf("[CMD] !! Pin tidak dikenal: %s\n", pin);
        }
    }

    // ── SERVO / FEEDING ───────────────────────────────────────
    else if (strcmp(cmd, "servo") == 0) {
        const char* value = doc["value"] | "";
        float dur = doc["duration"] | 3.0f;

        if (strcmp(value, "start") == 0 && dur > 0) {
            doFeeding((int)dur);
        } else {
            Serial.println("[CMD] !! Servo command tidak valid");
        }
    }

    else {
        Serial.printf("[CMD] !! Command tidak dikenal: %s\n", cmd);
    }
}

// ================================================================
//  SUBSYSTEM 1 — LAMPU (Relay Pin 1)
//
//  Auto logic (dipanggil setiap baca sensor):
//    20°C ≤ suhu < 30°C  → NYALA  (range butuh kehangatan)
//    suhu < 20°C          → MATI   (belum perlu)
//    suhu ≥ 30°C          → MATI   (terlalu panas)
//
//  Manual via Discord: !relay 1 on/off  (langsung override)
// ================================================================
void setLamp(bool on, const char* by) {
    lampState = on;
    digitalWrite(RELAY_LAMP_PIN, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("[LAMP]   %s  (by: %s)\n", on ? "NYALA" : "MATI", by);
}

void checkAutoLamp(float temp) {
    bool shouldOn = (temp >= TEMP_LAMP_ON && temp < TEMP_LAMP_OFF);

    if (shouldOn != lampState) {
        // Hanya toggle jika state berubah, biar tidak spam relay
        Serial.printf("[AUTO]   Suhu %.1f°C → Lampu %s\n", temp, shouldOn ? "NYALA" : "MATI");
        setLamp(shouldOn, "auto");
    }
}

// ================================================================
//  SUBSYSTEM 2 — WATER PUMP (Relay Pin 2)
//  Sepenuhnya manual dari Discord: !relay 2 on/off
// ================================================================
void setPump(bool on) {
    pumpState = on;
    digitalWrite(RELAY_PUMP_PIN, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("[PUMP]   %s\n", on ? "NYALA" : "MATI");
}

// ================================================================
//  SUBSYSTEM 3 — FEEDER SERVO
//  Non-blocking wait: MQTT tetap diproses selama servo terbuka
//  sehingga ESP32 masih bisa terima command lain saat feeding.
// ================================================================
void doFeeding(int durationSec) {
    Serial.printf("[FEED]   Mulai! Buka servo → 180° selama %d detik\n", durationSec);
    feederServo.write(SERVO_OPEN);

    unsigned long startMs = millis();
    unsigned long totalMs = (unsigned long)durationSec * 1000UL;

    // Non-blocking loop — MQTT tetap berjalan
    while (millis() - startMs < totalMs) {
        mqttClient.loop();   // Proses incoming MQTT
        delay(50);
    }

    feederServo.write(SERVO_IDLE);
    Serial.println("[FEED]   Selesai! Servo kembali 90°");

    // Kirim notifikasi ke Discord via telemetri
    StaticJsonDocument<96> doc;
    doc["event"]    = "feeding_done";
    doc["duration"] = durationSec;
    char buf[96];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
}
