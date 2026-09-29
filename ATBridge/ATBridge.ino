// Cau noi AT command tho: chuyen tiep byte qua lai giua Serial Monitor (may
// tinh) va module A7680C (Serial2), khong qua logic parse cua SmsSender.ino.
// Dung de go tay lenh AT, loai tru kha nang loi nam o code thay vi day/nguon.
//
// Cach dung:
//   1. Nap sketch nay (giu nguyen day noi: TXD module -> GPIO26, RXD module -> GPIO27).
//   2. Mo Serial Monitor, baud 115200, o o goc duoi chon "Both NL & CR" (hoac "Newline").
//   3. Go "AT" roi Enter. Module song thi tra ve "OK" ngay lap tuc.
//   4. Thu tiep AT+CPIN?  ->  phai co "READY"
//            AT+CSQ      ->  cuong do song, "99,99" nghia la chua bat duoc antena/sim
//            AT+CREG?    ->  0,1 hoac 0,5 la da dang ky mang; 0,0 / 0,2 la chua co song

#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
#define MODEM_BAUD 115200

HardwareSerial modemSerial(2);

void setup() {
  Serial.begin(115200);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  Serial.println("AT bridge san sang. Go lenh AT roi Enter (chon 'Both NL & CR' o Serial Monitor).");
}

void loop() {
  while (Serial.available()) {
    modemSerial.write(Serial.read());
  }
  while (modemSerial.available()) {
    Serial.write(modemSerial.read());
  }
}
