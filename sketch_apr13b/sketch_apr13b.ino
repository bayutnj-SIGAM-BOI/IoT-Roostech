#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

LiquidCrystal_I2C lcd(0x27, 16, 2); 


#define DHTPIN 5
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

const int relayPin = 33;

void setup() {
  Serial.begin(9600);
  pinMode(relayPin, OUTPUT);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Memulai Sensor...");
  


  dht.begin();
  delay(2000);
  lcd.clear();
}

void loop() {

  delay(2000);
  float h = dht.readHumidity();
  float t = dht.readTemperature(); 

  if (isnan(h) || isnan(t)) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Sensor Error!");
    return;
  }

  if(t > 30) {
    digitalWrite(relayPin, HIGH);
    lcd.setCursor(0, 1);
    lcd.print("Status : ON");
  } else {
    digitalWrite(relayPin, LOW);
     lcd.setCursor(0, 1);
     lcd.print("Status : OFF");
  
  }


  // Tampilkan ke Serial Monitor
  Serial.print("Kelembapan: "); Serial.print(h);
  Serial.print(" % | Suhu: "); Serial.print(t); Serial.println(" *C");

  // Tampilkan ke LCD
  // Baris 1
  lcd.setCursor(0, 0);
  lcd.print("Suhu: ");
  lcd.print(t, 1);
  lcd.print((char)223);
  lcd.print("C  ");

  // Baris 2
  // lcd.setCursor(0, 1);
  // lcd.print("Kelembap: ");
  // lcd.print(h, 1);
  // lcd.print("%  ");
}
