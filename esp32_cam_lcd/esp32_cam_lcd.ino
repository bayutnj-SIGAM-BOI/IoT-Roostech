/*
 * ================================================================
 *  ESP32 IoT Controller — Tambahan Modul:
 *    - OV2640 Camera  : snapshot auto tiap N detik + manual via MQTT
 *    - LCD I2C 16x2   : tampilkan suhu, humidity, status
 *
 *  Alur foto ke Discord:
 *    ESP32 capture JPEG → HTTP POST ke Python bot (port 5005)
 *    → bot forward ke Discord channel sebagai attachment
 *
 *  Library tambahan yang perlu diinstall:
 *    - LiquidCrystal_I2C  (Frank de Brabander)
 *    - esp32-camera        sudah built-in di ESP32 Arduino core
 *
 *  Pin OV2640 (pakai board ESP32-CAM AI-Thinker):
 *    Sudah terdefinisi di camera_pins.h bawaan core
 *    PSRAM wajib aktif: Tools → PSRAM → "OPI PSRAM" atau "Enabled"
 *
 *  Pin LCD I2C:
 *    SDA → GPIO 14   (sesuaikan jika beda board)
 *    SCL → GPIO 15
 *    Alamat I2C default: 0x27  (scan jika tidak jalan)
 *
 *  CATATAN: File ini melengkapi esp32_controller.ino.
 *  Gabungkan semua kode dalam SATU file .ino atau pakai tab tambahan
 *  di Arduino IDE.
 * ================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>/Users/bayu/Documents/IoT/IoT | C++/mqttEsp32/mqttEsp32.ino
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Camera includes ──────────────────────────────────────────
#include "esp_camera.h"
#include "camera_pins.h"   // Berisi definisi pin PWDN, RESET, XCLK, dll.
                            // File ini ada di contoh "CameraWebServer" bawaan core

// ════════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ════════════════════════════════════════════════════════════
#define DHT_PIN         5      // GPIO13 — pindah dari 4 karena cam pakai banyak GPIO
#define DHT_TYPE        DHT22
#define SERVO_PIN       12
#define RELAY_LAMP_PIN  32
#define RELAY_PUMP_PIN  33

// LCD I2C
#define LCD_SDA         14
#define LCD_SCL         15      // Jika pakai GPIO15 untuk relay pump, ganti ke pin lain
#define LCD_ADDR        0x27    // Scan I2C jika tidak jalan: 0x27 atau 0x3F
#define LCD_COLS        16
#define LCD_ROWS        2

#define RELAY_ON   LOW
#define RELAY_OFF  HIGH
#define SERVO_IDLE   90
#define SERVO_OPEN  180

// ── Threshold suhu ───────────────────────────────────────────
#define TEMP_LAMP_ON   30.0f
#define TEMP_LAMP_OFF  35.0f

// ════════════════════════════════════════════════════════════
//  KONFIGURASI
// ════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "PRAXIS-TNJ";
const char* WIFI_PASSWORD = "@techno@213";

// MQTT (HiveMQ Cloud)
const char* MQTT_SERVER   = "601011583e8349a79985f364d6cbae47.s1.eu.hivemq.cloud";
const int   MQTT_PORT     = 8883;
const char* MQTT_USERNAME = "IoTeam";
const char* MQTT_PASSWORD_M = "Roostech123";
const char* CLIENT_ID     = "esp32-cam-controller";

// Topics
const char* TOPIC_COMMAND   = "esp32/command";
const char* TOPIC_TELEMETRY = "esp32/telemetry";
const char* TOPIC_STATUS    = "esp32/status";

// HTTP endpoint Python bot untuk upload foto
// Ganti dengan IP komputer yang menjalankan bot.py
// Contoh: "http://192.168.1.100:5005/snapshot"
const char* SNAPSHOT_ENDPOINT = "http://192.168.1.XXX:5005/snapshot";

// Interval
const unsigned long TELEMETRY_INTERVAL = 5000UL;    // 5 detik
const unsigned long SNAPSHOT_INTERVAL  = 30000UL;   // Auto snapshot tiap 30 detik
const unsigned long RECONNECT_INTERVAL = 5000UL;
const unsigned long LCD_UPDATE_INTERVAL = 2000UL;   // Update LCD tiap 2 detik

// ════════════════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════════════════
WiFiClientSecure secureClient;
PubSubClient     mqttClient(secureClient);
DHT              dht(DHT_PIN, DHT_TYPE);
Servo            feederServo;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

unsigned long lastTelemetryMs  = 0;
unsigned long lastSnapshotMs   = 0;
unsigned long lastReconnectMs  = 0;
unsigned long lastLcdMs        = 0;

bool  lampState    = false;
bool  pumpState    = false;
bool  autoSnapshot = true;    // Bisa di-toggle dari Discord
float lastTemp     = 0.0f;
float lastHumi     = 0.0f;

// LCD scrolling state
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
bool initCamera();
bool captureAndSend(const char* trigger);
void updateLCD();
void lcdShowLine(int row, const String& text, bool clear = false);

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n[Boot] ESP32 CAM + LCD Controller starting...");

    // ── GPIO ────────────────────────────────────────────────
    pinMode(RELAY_LAMP_PIN, OUTPUT);
    pinMode(RELAY_PUMP_PIN, OUTPUT);
    digitalWrite(RELAY_LAMP_PIN, RELAY_OFF);
    digitalWrite(RELAY_PUMP_PIN, RELAY_OFF);

    // ── LCD I2C ─────────────────────────────────────────────
    Wire.begin(LCD_SDA, LCD_SCL);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("  IoT Controller");
    lcd.setCursor(0, 1); lcd.print("  Booting...");
    Serial.println("[LCD]  Initialized");

    // ── Servo ────────────────────────────────────────────────
    feederServo.attach(SERVO_PIN);
    feederServo.write(SERVO_IDLE);

    // ── DHT ─────────────────────────────────────────────────
    dht.begin();

    // ── Camera ──────────────────────────────────────────────
    if (!initCamera()) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Camera FAILED!");
        Serial.println("[CAM] !! Init gagal. Cek pin & PSRAM.");
    } else {
        Serial.println("[CAM]  Ready");
    }

    // ── WiFi ─────────────────────────────────────────────────
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Connecting WiFi");
    connectWiFi();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK!");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString());
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

    // ── Auto snapshot ────────────────────────────────────────
    if (autoSnapshot && (now - lastSnapshotMs >= SNAPSHOT_INTERVAL)) {
        lastSnapshotMs = now;
        captureAndSend("auto");
    }

    // ── Update LCD ───────────────────────────────────────────
    if (now - lastLcdMs >= LCD_UPDATE_INTERVAL) {
        lastLcdMs = now;
        updateLCD();
    }
}

// ════════════════════════════════════════════════════════════
//  CAMERA INIT
// ════════════════════════════════════════════════════════════
bool initCamera() {
    camera_config_t config;

    // Pin mapping AI-Thinker ESP32-CAM
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        // PSRAM tersedia → resolusi lebih tinggi
        config.frame_size   = FRAMESIZE_VGA;   // 640x480
        config.jpeg_quality = 12;              // 0-63, makin kecil makin bagus
        config.fb_count     = 2;
    } else {
        // Tanpa PSRAM → turunkan resolusi
        config.frame_size   = FRAMESIZE_QVGA;  // 320x240
        config.jpeg_quality = 15;
        config.fb_count     = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] esp_camera_init gagal: 0x%x\n", err);
        return false;
    }

    // Optional: tuning sensor
    sensor_t* s = esp_camera_sensor_get();
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);

    return true;
}

// ════════════════════════════════════════════════════════════
//  CAPTURE & SEND SNAPSHOT
//  Capture JPEG dari kamera, kirim via HTTP POST ke Python bot.
//  Bot akan forward foto ke channel Discord sebagai attachment.
// ════════════════════════════════════════════════════════════
bool captureAndSend(const char* trigger) {
    Serial.printf("[CAM]  Capturing... (trigger: %s)\n", trigger);

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[CAM] !! Capture gagal");
        lcdShowLine(1, "Cam capture err!");
        return false;
    }

    Serial.printf("[CAM]  Size: %u bytes\n", fb->len);

    // Kirim via HTTP POST multipart ke Python bot
    HTTPClient http;
    http.begin(SNAPSHOT_ENDPOINT);
    http.setTimeout(8000);

    // Header agar bot tahu konteksnya
    http.addHeader("X-Trigger",     trigger);
    http.addHeader("X-Temperature", String(lastTemp, 1));
    http.addHeader("X-Humidity",    String(lastHumi, 1));
    http.addHeader("Content-Type",  "image/jpeg");

    int httpCode = http.POST(fb->buf, fb->len);
    esp_camera_fb_return(fb);   // WAJIB: kembalikan buffer ke pool

    if (httpCode == 200) {
        Serial.printf("[CAM]  Snapshot terkirim! HTTP %d\n", httpCode);
        lcdShowLine(1, "Snapshot sent!  ");
        return true;
    } else {
        Serial.printf("[CAM] !! HTTP error: %d\n", httpCode);
        lcdShowLine(1, "Snap err:" + String(httpCode));
        return false;
    }

    http.end();
}

// ════════════════════════════════════════════════════════════
//  SENSOR
// ════════════════════════════════════════════════════════════
void readAndPublishSensor() {
    float temp = dht.readTemperature();
    float humi = dht.readHumidity();

    if (isnan(temp) || isnan(humi)) {
        Serial.println("[DHT] !! Gagal baca sensor");
        return;
    }

    lastTemp = temp;
    lastHumi = humi;
    Serial.printf("[DHT]  Suhu: %.1f°C | Humi: %.1f%%\n", temp, humi);

    checkAutoLamp(temp);
    publishTelemetry(temp, humi, WiFi.RSSI());
}

// ════════════════════════════════════════════════════════════
//  LCD UPDATE — Bolak-balik 2 halaman tiap 2 detik
//  Halaman 0: Suhu & Humidity
//  Halaman 1: Status Lamp & Pump
// ════════════════════════════════════════════════════════════
void updateLCD() {
    lcd.clear();
    if (lcdPage == 0) {
        // Baris 0: Suhu
        lcd.setCursor(0, 0);
        lcd.print("Suhu:");
        lcd.print(lastTemp, 1);
        lcd.print((char)223);   // Karakter derajat °
        lcd.print("C");

        // Baris 1: Humidity
        lcd.setCursor(0, 1);
        lcd.print("Humi:");
        lcd.print(lastHumi, 1);
        lcd.print("%");
    } else {
        // Baris 0: Status lampu
        lcd.setCursor(0, 0);
        lcd.print("Lamp:");
        lcd.print(lampState ? "ON " : "OFF");
        lcd.print(" Pump:");
        lcd.print(pumpState ? "ON" : "OFF");

        // Baris 1: WiFi & RSSI
        lcd.setCursor(0, 1);
        String rssiStr = "RSSI:" + String(WiFi.RSSI()) + "dBm";
        lcd.print(rssiStr.substring(0, LCD_COLS));
    }
    lcdPage = 1 - lcdPage;   // Toggle halaman
}

// Helper: tulis 1 baris di LCD tanpa clear seluruh layar
void lcdShowLine(int row, const String& text) {
    lcd.setCursor(0, row);
    // Pad spasi agar karakter lama tertimpa bersih
    String padded = text;
    while (padded.length() < LCD_COLS) padded += ' ';
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
        if (++attempt > 40) ESP.restart();
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

void publishTelemetry(float temp, float humi, int rssi) {
    StaticJsonDocument<192> doc;
    doc["temperature"] = serialized(String(temp, 1));
    doc["humadity"]    = serialized(String(humi, 1));
    doc["rssi"]        = rssi;
    doc["lamp"]        = lampState ? "ON" : "OFF";
    doc["pump"]        = pumpState ? "ON" : "OFF";
    char buf[192];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
}

// ════════════════════════════════════════════════════════════
//  COMMAND HANDLER
//  Tambahan command baru:
//    {"command":"snapshot"}               → ambil foto sekarang
//    {"command":"autosnap","value":"on"}  → aktifkan auto snapshot
//    {"command":"autosnap","value":"off"} → matikan auto snapshot
// ════════════════════════════════════════════════════════════
void handleCommand(const char* payload, unsigned int length) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, length)) return;

    const char* cmd = doc["command"] | "";

    if (strcmp(cmd, "ping") == 0) {
        StaticJsonDocument<128> resp;
        resp["response"]    = "pong";
        resp["temperature"] = serialized(String(lastTemp, 1));
        resp["rssi"]        = WiFi.RSSI();
        resp["lamp"]        = lampState ? "ON" : "OFF";
        resp["pump"]        = pumpState ? "ON" : "OFF";
        resp["autosnap"]    = autoSnapshot ? "ON" : "OFF";
        char buf[128];
        serializeJson(resp, buf);
        mqttClient.publish(TOPIC_TELEMETRY, buf);
    }

    else if (strcmp(cmd, "relay") == 0) {
        const char* pin   = doc["pin"]   | "";
        bool turnOn = strcmp(doc["value"] | "", "on") == 0;
        if      (strcmp(pin, "1") == 0) setLamp(turnOn, "discord");
        else if (strcmp(pin, "2") == 0) setPump(turnOn);
    }

    else if (strcmp(cmd, "servo") == 0) {
        float dur = doc["duration"] | 3.0f;
        if (strcmp(doc["value"] | "", "start") == 0 && dur > 0)
            doFeeding((int)dur);
    }

    // ── Snapshot manual dari Discord (!snapshot) ─────────────
    else if (strcmp(cmd, "snapshot") == 0) {
        lcdShowLine(1, "Snap: manual... ");
        captureAndSend("manual");
    }

    // ── Toggle auto snapshot (!autosnap on/off) ───────────────
    else if (strcmp(cmd, "autosnap") == 0) {
        const char* val = doc["value"] | "";
        autoSnapshot = (strcmp(val, "on") == 0);
        Serial.printf("[CAM]  Auto snapshot: %s\n", autoSnapshot ? "ON" : "OFF");
        // Kirim konfirmasi balik ke Discord lewat telemetri
        StaticJsonDocument<64> resp;
        resp["event"]     = "autosnap_changed";
        resp["autosnap"]  = autoSnapshot ? "ON" : "OFF";
        char buf[64];
        serializeJson(resp, buf);
        mqttClient.publish(TOPIC_TELEMETRY, buf);
        lcdShowLine(1, autoSnapshot ? "AutoSnap: ON    " : "AutoSnap: OFF   ");
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

void checkAutoLamp(float temp) {
    bool shouldOn = (temp >= TEMP_LAMP_ON && temp < TEMP_LAMP_OFF);
    if (shouldOn != lampState) {
        Serial.printf("[AUTO]  Suhu %.1f°C → Lampu %s\n", temp, shouldOn ? "NYALA" : "MATI");
        setLamp(shouldOn, "auto");
    }
}

void setPump(bool on) {
    pumpState = on;
    digitalWrite(RELAY_PUMP_PIN, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("[PUMP]  %s\n", on ? "NYALA" : "MATI");
}

void doFeeding(int durationSec) {
    Serial.printf("[FEED]  Mulai %d detik\n", durationSec);
    lcdShowLine(1, "Feeding " + String(durationSec) + "s...   ");
    feederServo.write(SERVO_OPEN);

    unsigned long startMs = millis();
    while (millis() - startMs < (unsigned long)durationSec * 1000UL) {
        mqttClient.loop();
        delay(50);
    }

    feederServo.write(SERVO_IDLE);
    lcdShowLine(1, "Feeding done!   ");
    Serial.println("[FEED]  Selesai");

    StaticJsonDocument<64> doc;
    doc["event"]    = "feeding_done";
    doc["duration"] = durationSec;
    char buf[64];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TELEMETRY, buf);
}
