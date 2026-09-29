#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#include <TinyGPS++.h>
#include <SoftwareSerial.h>
TinyGPSPlus gps;
SoftwareSerial neo6M(D5, D6);
static const uint32_t GPSBaud = 38400;//9600
String Latitude,Longitude;

void setup() {
  Serial.begin(115200);//Giao tiếp uart với PC
  neo6M.begin(GPSBaud);
  delay(1000);
  Wire.begin();
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    //for(;;); // Don't proceed, loop forever
  }
  display.display();
  delay(1000); // Pause for 2 seconds
  display.setTextColor(SSD1306_WHITE);        // Draw white text
  display.clearDisplay();
  display.setTextSize(2);  
  display.setCursor(0,0);                     // Start at top-left corner
  display.print("Welcome!");
  display.display();
  delay(2000);
}

void loop() {
  smartDelay(1000);
  if (gps.location.isUpdated()) {
    Latitude = String(gps.location.lat(),6);
    Longitude = String(gps.location.lng(), 6);
    Serial.print("Latitude= ");
    Serial.print(Latitude);
    Serial.print(" Longitude= ");
    Serial.println(Longitude);

    display.clearDisplay();
    display.setTextSize(1);  
    display.setCursor(0,0);                     // Start at top-left corner
    display.print("LAT:");
    display.print(Latitude);
    display.setCursor(0,20);
    display.print("LNG:");
    display.print(Longitude);
    display.display();
  }else{
    printDateTime(gps.date);
  }
}
//-------------Chương trình con--------------------
void smartDelay(unsigned long ms){
  unsigned long start = millis();
  do 
  {
    while (neo6M.available())
      gps.encode(neo6M.read());
  } while (millis() - start < ms);
}
static void printDateTime(TinyGPSDate &d){
  if (!d.isValid()){
    Serial.print(F("********** "));
  }else{
    char sz[32];
    sprintf(sz, "%02d/%02d/%02d", d.day(),  d.month(), d.year());
    Serial.print("\nTimeGPS:");
    Serial.println(sz);
    display.clearDisplay();
    display.setTextSize(1);  
    display.setCursor(0,0);                     // Start at top-left corner
    display.print("Date:"+String(sz));
    display.display();
  }
}