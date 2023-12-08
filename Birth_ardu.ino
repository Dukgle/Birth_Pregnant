#include <Adafruit_NeoPixel.h>

#define PIN 7
#define NUMPIXELS 60
#define BRIGHTNESS 10

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

uint16_t ntohs(uint16_t netshort) {
  return (netshort >> 8) | ((netshort & 0xFF) << 8);
}

int FSRsensor = A0;          // 센서값을 아나로그 A0핀 설정
int value = 0;               // loop에서 사용할 변수 설정
char notEmpty = '0';         // 좌석 상태를 나타내는 변수 초기화

void setup(){
  Serial.begin(9600);
  strip.setBrightness(BRIGHTNESS);
  strip.begin();
  strip.show();
}

void loop(){
  value = analogRead(FSRsensor);
  //fill(strip.Color(0, 255, 0)); // Green color

  if (value > 700) {
    notEmpty = '1';
    Serial.println(notEmpty);


  } else if (value == 0) {
      fill(strip.Color(0, 0, 0)); // Turn off the LED if the seat is empty
      notEmpty = '0';
      Serial.println(notEmpty);
  }

  int command;
  if (Serial.available() >= sizeof(command)) {
    Serial.readBytes((char*)&command, sizeof(command));
    //int command = Serial.parseInt(); // 앞뒤 공백 제거
    command = ntohs(command);
    Serial.print(command);

    if (command == 1) {
      fill(strip.Color(0, 255, 0));  // LED를 켬
    } else if (command == 0) {
      fill(strip.Color(255, 0, 0)); // Red color
    } else {
      fill(strip.Color(0, 0, 0));
    }
  }

  delay(1000);
}

void fill(uint32_t color) {
  for (uint16_t i = 0; i < strip.numPixels(); i++) {
    strip.setPixelColor(i, color);  
  }  
  strip.show();
}