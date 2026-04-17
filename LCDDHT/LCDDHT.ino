#define RELAY 33

void setup() {
  // put your setup code here, to run once:
  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, HIGH);

}

void loop() {
  // put your main code here, to run repeatedly:
  digitalWrite(RELAY, LOW);
  delay(1000);
  digitalWrite(RELAY, HIGH);
  delay(1000);
}
