// Test co ban cho ESP32 DevKit 38 chan: nhap nha 1 LED de kiem tra
// board/driver COM/Arduino IDE hoat dong dung truoc khi lam gi phuc tap hon.
//
// Dung thang GPIO2 thay vi LED_BUILTIN: nhieu board ESP32 DevKit 38 chan
// (kieu "DOIT DevKit V1") co san 1 LED do tren mach ngay tai GPIO2, nhung
// khong phai ban clone nao cung han san LED nay, va macro LED_BUILTIN co
// the khong duoc dinh nghia cho board "ESP32 Dev Module" trong Arduino IDE
// tuy phien ban core (=> loi bien dich "'LED_BUILTIN' was not declared").
// Neu board ban dung khong co LED tai GPIO2, gan them 1 LED + dien tro
// ~220-330 ohm tu GPIO2 xuong GND de quan sat.

#define LED_PIN 2

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  delay(1000);
}
