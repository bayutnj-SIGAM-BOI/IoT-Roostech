#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ════════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ════════════════════════════════════════════════════════════
#define DHT_PIN         5
#define DHT_TYPE        DHT22
#define SERVO_PIN       35
#define RELAY_LAMP_PIN  33
#define RELAY_PUMP_PIN  32

// ── I2C custom pins ──────────────────────────────────────────
#define I2C_SDA  21
#define I2C_SCL  22

// FIX: Sesuaikan dengan jenis relay board kamu.
//      Kebanyakan modul relay China: Active LOW → RELAY_ON = LOW
//      Jika relay tidak nyala sama sekali, coba balik ke HIGH/LOW
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

#define SERVO_IDLE   90
#define SERVO_OPEN  180

#define LCD_COLS  16
#define LCD_ROWS   2

#define TEMP_LAMP_ON   28.0f   // nyala kalau suhu < 28°C
#define TEMP_LAMP_OFF  32.0f   // mati  kalau suhu > 32°C

// ════════════════════════════════════════════════════════════
//  KONFIGURASI
// ════════════════════════════════════════════════════════════
const char* WIFI_SSID       = "PRAXIS-TNJ";
const char* WIFI_PASSWORD   = "@techno@213";

// MQTT (HiveMQ Cloud)
const char* MQTT_SERVER     = "601011583e8349a79985f364d6cbae47.s1.eu.hivemq.cloud";
const int   MQTT_PORT       = 8883;
const char* MQTT_USERNAME   = "IoTeam";
const char* MQTT_PASSWORD_M = "Roostech123";
const char* CLIENT_ID       = "esp32-controller";

// Topics
const char* TOPIC_COMMAND   = "esp32/command";
const char* TOPIC_TELEMETRY = "esp32/telemetry";
const char* TOPIC_STATUS    = "esp32/status";

// Interval
const unsigned long TELEMETRY_INTERVAL  = 5000UL;
const unsigned long RECONNECT_INTERVAL  = 5000UL;
const unsigned long LCD_UPDATE_INTERVAL = 2000UL;

// ════════════════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════════════════
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);
DHT              dht(DHT_PIN, DHT_TYPE);
Servo            feederServo;
LiquidCrystal_I2C lcd(0x27, LCD_COLS, LCD_ROWS);

unsigned long lastTelemetryMs = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastLcdMs       = 0;

bool  lampState = false;
bool  pumpState = false;
float lastTemp  = 0.0f;
float lastHumi  = 0.0f;

int lcdPage = 0;

// ════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════
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
void readAndPublishSensor();
void updateLCD();
void lcdShowLine(int row, const String& text);

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Boot] ESP32 IoT Controller starting...");

    // ── GPIO ────────────────────────────────────────────────
    pinMode(RELAY_LAMP_PIN, OUTPUT);
    pinMode(RELAY_PUMP_PIN, OUTPUT);
    digitalWrite(RELAY_LAMP_PIN, RELAY_OFF);
    digitalWrite(RELAY_PUMP_PIN, RELAY_OFF);

    // ── FIX: I2C custom SDA/SCL sebelum lcd.init() ──────────
    Wire.begin(I2C_SDA, I2C_SCL);

    // ── LCD I2C ─────────────────────────────────────────────
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("  IoT Controller");
    lcd.setCursor(0, 1); lcd.print("  Booting...    ");
    Serial.println("[LCD]  Initialized");

    // ── FIX: Relay boot-test ─────────────────────────────────
    // Jika relay berbunyi klik 2x → hardware OK, masalah di logika/MQTT
    // Jika tidak berbunyi sama sekali → cek wiring / ganti RELAY_ON ke HIGH
    Serial.println("[TEST] Relay lamp test...");
    digitalWrite(RELAY_LAMP_PIN, RELAY_ON);
    delay(300);
    digitalWrite(RELAY_LAMP_PIN, RELAY_OFF);
    delay(300);
    Serial.println("[TEST] Relay pump test...");
    digitalWrite(RELAY_PUMP_PIN, RELAY_ON);
    delay(300);
    digitalWrite(RELAY_PUMP_PIN, RELAY_OFF);
    Serial.println("[TEST] Relay test selesai");

    // ── Servo ────────────────────────────────────────────────
    feederServo.attach(SERVO_PIN);
    feederServo.write(SERVO_IDLE);
    delay(500);
    feederServo.detach(); // FIX: detach agar tidak jitter/panas saat idle

    // ── DHT ─────────────────────────────────────────────────
    dht.begin();

    // ── WiFi ─────────────────────────────────────────────────
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Connecting WiFi ");
    connectWiFi();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK!        ");
    lcd.setCursor(0, 1);
    String ipStr = WiFi.localIP().toString();
    while (ipStr.length() < LCD_COLS) ipStr += ' ';
    lcd.print(ipStr.substring(0, LCD_COLS));
    delay(1500);

    // ── MQTT ─────────────────────────────────────────────────
    secureClient.setInsecure();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(512);
    connectMQTT();

    Serial.println("[Boot] Setup selesai!");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
    // ── MQTT keepalive ───────────────────────────────────────
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectMs >= RECONNECT_INTERVAL) {
            lastReconnectMs = now;
            connectMQTT();
        }
        return;
    }
    mqttClient.loop();

    unsigned long now = millis();

    // ── Telemetri + auto lamp ────────────────────────────────
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL) {
        lastTelemetryMs = now;
        readAndPublishSensor();
    }

    // ── Update LCD ───────────────────────────────────────────
    if (now - lastLcdMs >= LCD_UPDATE_INTERVAL) {
        lastLcdMs = now;
        updateLCD();
    }
}

// ════════════════════════════════════════════════════════════
//  SENSOR
// ════════════════════════════════════════════════════════════
void readAndPublishSensor() {
    float temp = dht.readTemperature();
    float humi = dht.readHumidity();

    if (isnan(temp) || isnan(humi)) {
        Serial.println("[DHT] !! Gagal baca sensor");
        lcd.clear();
        lcdShowLine(0, "DHT Error!      ");
        lcdShowLine(1, "Cek kabel DHT22 ");
        return;
    }

    lastTemp = temp;
    lastHumi = humi;
    Serial.printf("[DHT]  Suhu: %.1f C | Humi: %.1f%%\n", temp, humi);

    checkAutoLamp(temp);
    publishTelemetry(temp, humi, WiFi.RSSI());
}

// ════════════════════════════════════════════════════════════
//  LCD UPDATE — Bolak-balik 2 halaman tiap LCD_UPDATE_INTERVAL
//  Halaman 0: Suhu & Humidity
//  Halaman 1: Status Lamp & Pump + RSSI
// ════════════════════════════════════════════════════════════
void updateLCD() {
    lcd.clear();
    if (lcdPage == 0) {
        // Baris 0: "Suhu:28.5C"
        char line0[LCD_COLS + 1];
        snprintf(line0, sizeof(line0), "Suhu:%-5.1fC     ", lastTemp);
        // tambahkan karakter derajat manual
        lcd.setCursor(0, 0);
        lcd.print("Suhu:");
        lcd.print(lastTemp, 1);
        lcd.write((uint8_t)223); // °
        lcd.print("C");

        // Baris 1: "Humi:65.2%"
        lcd.setCursor(0, 1);
        lcd.print("Humi:");
        lcd.print(lastHumi, 1);
        lcd.print("%");

    } else {
        // Baris 0: "Lp:ON  Pm:OFF"
        lcd.setCursor(0, 0);
        lcd.print("Lp:");
        lcd.print(lampState ? "ON " : "OFF");
        lcd.print("  Pm:");
        lcd.print(pumpState ? "ON " : "OFF");

        // FIX: rssiStr sebelumnya dideklarasi tapi tidak di-print ke LCD
        lcd.setCursor(0, 1);
        String rssiStr = "RSSI:" + String(WiFi.RSSI()) + "dBm";
        while (rssiStr.length() < LCD_COLS) rssiStr += ' ';
        lcd.print(rssiStr.substring(0, LCD_COLS));
    }
    lcdPage = 1 - lcdPage;
}

void lcdShowLine(int row, const String& text) {
    lcd.setCursor(0, row);
    String padded = text;
    while ((int)padded.length() < LCD_COLS) padded += ' ';
    lcd.print(padded.substring(0, LCD_COLS));
}

// ════════════════════════════════════════════════════════════
//  WIFI
// ════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempt > 40) {
            Serial.println("\n[WiFi] Gagal connect! Restart...");
            ESP.restart();
        }
    }
    Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
}

// ════════════════════════════════════════════════════════════
//  MQTT
// ════════════════════════════════════════════════════════════
void connectMQTT() {
    StaticJsonDocument<64> willDoc;
    willDoc["state"] = "OFFLINE";
    char willPayload[64];
    serializeJson(willDoc, willPayload);

    bool ok = mqttClient.connect(
        CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD_M,
        TOPIC_STATUS, 1, true, willPayload
    );

    if (ok) {
        mqttClient.subscribe(TOPIC_COMMAND, 1);
        publishStatus("ONLINE");
        Serial.println("[MQTT] Connected & subscribed");
        lcdShowLine(0, "MQTT: Connected ");
    } else {
        Serial.printf("[MQTT] Gagal, rc=%d\n", mqttClient.state());
        lcdShowLine(0, "MQTT: FAILED!   ");
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT IN] [%s]: ", topic);
    for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
    Serial.println();
    if (strcmp(topic, TOPIC_COMMAND) == 0)
        handleCommand((const char*)payload, length);
}

void publishStatus(const char* state) {
    StaticJsonDocument<64> doc;
    doc["state"] = state;
    char buf[64];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_STATUS, buf, true);
}

// FIX: temperature & humidity dikirim sebagai float, bukan String serialized
void publishTelemetry(float temp, float humi, int rssi) {
    StaticJsonDocument<192> doc;
    doc["temperature"] = temp;
    doc["humidity"]    = humi;
    doc["rssi"]        = rssi;
    doc["lamp"]        = lampState ? "ON" : "OFF";
    doc["pump"]        = pumpState ? "ON" : "OFF";
    char buf[192];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
}

// ════════════════════════════════════════════════════════════
//  COMMAND HANDLER
//  Commands:
//    {"command":"ping"}
//    {"command":"relay","pin":"1","value":"on"}   → lampu
//    {"command":"relay","pin":"2","value":"on"}   → pompa
//    {"command":"servo","value":"start","duration":3}
// ════════════════════════════════════════════════════════════
void handleCommand(const char* payload, unsigned int length) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.printf("[CMD] JSON parse error: %s\n", err.c_str());
        return;
    }

    const char* cmd = doc["command"] | "";
    Serial.printf("[CMD] Command: %s\n", cmd);

    if (strcmp(cmd, "ping") == 0) {
        StaticJsonDocument<128> resp;
        resp["response"]    = "pong";
        resp["temperature"] = lastTemp;
        resp["rssi"]        = WiFi.RSSI();
        resp["lamp"]        = lampState ? "ON" : "OFF";
        resp["pump"]        = pumpState ? "ON" : "OFF";
        char buf[128];
        serializeJson(resp, buf);
        mqttClient.publish(TOPIC_TELEMETRY, buf);
    }

    else if (strcmp(cmd, "relay") == 0) {
        const char* pin   = doc["pin"]   | "";
        const char* value = doc["value"] | "";
        bool turnOn = (strcmp(value, "on") == 0);

        if      (strcmp(pin, "1") == 0) setLamp(turnOn, "mqtt");
        else if (strcmp(pin, "2") == 0) setPump(turnOn);
        else Serial.printf("[CMD] Unknown pin: %s\n", pin);
    }

    else if (strcmp(cmd, "servo") == 0) {
        float dur = doc["duration"] | 3.0f;
        if (strcmp(doc["value"] | "", "start") == 0 && dur > 0)
            doFeeding((int)dur);
    }

    else {
        Serial.printf("[CMD] Unknown command: %s\n", cmd);
    }
}

// ════════════════════════════════════════════════════════════
//  SUBSYSTEMS
// ════════════════════════════════════════════════════════════
void setLamp(bool on, const char* by) {
    lampState = on;
    digitalWrite(RELAY_LAMP_PIN, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("[LAMP]  %s (by: %s)\n", on ? "NYALA" : "MATI", by);
}

// ════════════════════════════════════════════════════════════
//  FIX: checkAutoLamp
//  Sebelumnya:  shouldOn = (temp >= 30 && temp < 35)
//    → Lampu nyala saat suhu TINGGI (30–35°C), tidak masuk akal
//      untuk heater/penghangat kandang.
//
//  Sesudah (hysteresis benar):
//    → Lampu ON  jika suhu < TEMP_LAMP_ON  (terlalu dingin)
//    → Lampu OFF jika suhu >= TEMP_LAMP_OFF (sudah cukup hangat)
//    → Di antara kedua threshold: state tidak berubah (hysteresis)
//
//  Jika kamu butuh logika PENDINGIN (fan/AC), balikkan kondisinya:
//    lampState ON  jika suhu >= TEMP_LAMP_OFF
//    lampState OFF jika suhu <  TEMP_LAMP_ON
// ════════════════════════════════════════════════════════════
void checkAutoLamp(float temp) {
    bool newState = lampState; // pertahankan state saat ini (hysteresis)

    if (temp < TEMP_LAMP_ON) {
        newState = true;   // terlalu dingin → nyalakan pemanas
    } else if (temp >= TEMP_LAMP_OFF) {
        newState = false;  // sudah cukup hangat → matikan pemanas
    }
    // zona hysteresis (TEMP_LAMP_ON <= temp < TEMP_LAMP_OFF): tidak berubah

    if (newState != lampState) {
        Serial.printf("[AUTO]  Suhu %.1fC → Lampu %s\n", temp, newState ? "NYALA" : "MATI");
        setLamp(newState, "auto");
    }
}

void setPump(bool on) {
    pumpState = on;
    digitalWrite(RELAY_PUMP_PIN, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("[PUMP]  %s\n", on ? "NYALA" : "MATI");
}

// FIX: attach servo hanya saat digunakan, detach setelah selesai
void doFeeding(int durationSec) {
    Serial.printf("[FEED]  Mulai %d detik\n", durationSec);
    lcdShowLine(0, "  AUTO FEEDING  ");
    lcdShowLine(1, "Dur: " + String(durationSec) + "s...      ");

    feederServo.attach(SERVO_PIN);
    feederServo.write(SERVO_OPEN);

    unsigned long startMs = millis();
    while (millis() - startMs < (unsigned long)durationSec * 1000UL) {
        mqttClient.loop();
        delay(50);
    }

    feederServo.write(SERVO_IDLE);
    delay(500);
    feederServo.detach(); // FIX: detach agar tidak jitter & hemat daya

    lcdShowLine(0, "Feeding done!   ");
    lcdShowLine(1, "                ");
    Serial.println("[FEED]  Selesai");

    StaticJsonDocument<64> doc;
    doc["event"]    = "feeding_done";
    doc["duration"] = durationSec;
    char buf[64];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
}
