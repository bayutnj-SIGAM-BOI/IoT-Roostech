#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  
  // Set ke mode station (bukan access point)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // pastikan tidak connect ke jaringan apapun
  delay(100);

  Serial.println("Mulai scan WiFi...");
}

void loop() {
  int n = WiFi.scanNetworks(); // scan, return jumlah jaringan ditemukan
  
  if (n == 0) {
    Serial.println("Tidak ada jaringan ditemukan.");
  } else {
    Serial.print(n);
    Serial.println(" jaringan ditemukan:\n");
    
    for (int i = 0; i < n; i++) {
      Serial.print(i + 1);
      Serial.print(". SSID: ");
      Serial.print(WiFi.SSID(i));        // nama jaringan
      Serial.print("  |  RSSI: ");
      Serial.print(WiFi.RSSI(i));        // kekuatan sinyal (dBm)
      Serial.print(" dBm  |  ");
      Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Encrypted");
    }
  }

  Serial.println("\nScan ulang dalam 5 detik...");
  delay(5000);
}