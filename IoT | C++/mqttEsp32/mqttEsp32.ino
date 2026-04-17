#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>

#define WIFI_SSID "TNJ_PRAXIS"
#define WIFI_PASS "juaraSatu1"

#define MQTT_SERVER "601011583e8349a79985f364d6cbae47.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "IoTeam"
#define MQTT_PASS "Roostech123"
#define MQTT_CLIENT "esp32-client"

#define TOPIC_TELEMETRY "esp32/telemetry"
#define TOPIC_STATUS "esp32/status"
#define TOPIC_COMMAND "esp32/command"

#define DHT_PIN 4
#define DHT_TYPE DHT11

DHT dht(DHTPIN, DHTTYPE);
Servo servo;
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastTelemetry = 0;
const long telemetryInterval = 10000;  // Jeda setiap ngirim Telemetry 10s nanti dikirim melaui esp32/telemetry topic

bool feedActive = false;
unsigned long feedStart = 0;
unsigned long feeddur = 0;

// ======== WIFI ======
void connectWIFI() {
  Serial.print("Connecting WIFI");
  WIFI.begin(WIFI_SSID, WIFI_PASS);
  while (WIFI.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWIFI Connected! IP:" + WIFI.LocalIP().toString());
}

// MQTT ||| Terima COMMAND DARI BOT DISCORD
void onMessage(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("📨 MQTT [" + String(topic) + "]: " + msg);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.println("JSON Error: " + String(err.c_str()));
    return;
  }

  String command = doc["command"].as<String>();

  if (command == "servo") {
    String val = doc["value"].as<String>();
    int dur = doc["duration"].as<int>() * 1000;

    if (val == "start" && !feedActive) {
      servo.write(180);
      feedActive = true;
      feedStart = millis();
      feeddur = dur * 1000UL;
      Serial.println("🍽️ Feeding started for " + String(feeddur) + "s");
    } else if (feedActive) {
      Serial.println("⚠️ Feeding sedang berjalan, tunggu selesai!");
    }
  } else if (command == "ping") {
    StaticJsonDocument<64> resp;
    resp["state"] = "pong";
    String out;
    serializeJson(resp, out);
    mqtt.publish(TOPIC_STATUS, out.c_str());
    Serial.println("🏓 Pong!");
  } else if (command == "relay") {
    int pin = doc["pin"].as<int>();
    String val = doc["value"].as<String>();
    digitalWrite(pin, val == "on" ? LOW : HIGH);
    Serial.println("⚡ Relay → " + pin + val);
  }
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Trying to Connect MQTT!!");
  }
  if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)) {
    Serial.println("Connected!");
    mqtt.subscribe(TOPIC_COMMAND);  // subs ke topic command

    // Kirim status online
    StaticJsonDocument<64> doc;
    doc["state"] = "online";
    String payload;
    serializeJson(doc, payload);
    mqtt.publish(TOPIC_STATUS, payload.c_str(), true);  // retained
  } else {
    Serial.println("Failed rc=" + String(mqtt.state()) + " retry 5s...");
    delay(5000);
  }
}

// kirim telemetry
void sendTelemetry() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.print("DHT read failed");
    return;
  }

  StaticJsonDocument<128> doc;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["rssi"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_TELEMETRY, payload.c_str());
  Serial.println("📤 Telemetry: " + payload);
}

void handleFeeding() {
  if (!feedActive) return;

  unsigned long elapsed = millis() - feedStart;
  if (elapsed >= feeddur) {
    servo.write(90);
    feedActive = false;

    Serial.println("✅ Feeding done!");

    // Kirim notif selesai ke Discord
    StaticJsonDocument<64> doc;
    doc["state"] = "feed_done";
    String payload;
    serializeJson(doc, payload);
    mqtt.publish(TOPIC_STATUS, payload.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  connectWifi();

  wifiClient.setInsecure();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);

  connectMQTT();

  dht.begin();

  servo.write(90);  // Posisi Awal 90º
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  handleFeeding();
  //Telemetry setiap 10s

  unsigned long now = millis();
  if (now - lastTelemetry >= telemetryInterval) {
    lastTelemetry = now;
    sendTelemetry();
  }
}
