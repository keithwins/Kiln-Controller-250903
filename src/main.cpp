
#include <Arduino.h>
#include <SPI.h>
// VERSION: 2025-01-03 16:00 UTC - HSPI Display Test - Clean Version

// CORRECT pins for your display (HSPI)
#define TFT_CS    15
#define TFT_DC    2  
#define TFT_RST   4
#define TFT_MOSI  13  // Was 23, now 13!
#define TFT_SCK   14  // Was 18, now 14!
#define TFT_MISO  12  // Was 19, now 12!
#define BACKLIGHT 27

void sendCommand(uint8_t cmd) {
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);   // Command mode
  SPI.transfer(cmd);
  digitalWrite(TFT_CS, HIGH);
}

void sendData(uint8_t data) {
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, HIGH);  // Data mode
  SPI.transfer(data);
  digitalWrite(TFT_CS, HIGH);
}

void fillScreen(uint16_t color) {
  // Set window to full screen
  sendCommand(0x2A); // Column address
  sendData(0x00); sendData(0x00); // Start 0
  sendData(0x01); sendData(0x3F); // End 319
  
  sendCommand(0x2B); // Row address  
  sendData(0x00); sendData(0x00); // Start 0
  sendData(0x01); sendData(0xDF); // End 479
  
  // Memory write
  sendCommand(0x2C);
  
  // Send color data
  digitalWrite(TFT_DC, HIGH); // Data mode
  digitalWrite(TFT_CS, LOW);
  
  uint8_t colorHigh = (color >> 8) & 0xFF;
  uint8_t colorLow = color & 0xFF;
  
  for (int i = 0; i < 320 * 480; i++) {
    SPI.transfer(colorHigh);
    SPI.transfer(colorLow);
  }
  
  digitalWrite(TFT_CS, HIGH);
}

void drawPattern() {
  // Set window
  sendCommand(0x2A);
  sendData(0x00); sendData(0x00);
  sendData(0x01); sendData(0x3F);
  
  sendCommand(0x2B);
  sendData(0x00); sendData(0x00);
  sendData(0x01); sendData(0xDF);
  
  sendCommand(0x2C);
  
  // Draw alternating stripes
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  
  for (int y = 0; y < 480; y++) {
    uint16_t color = (y / 60) % 2 ? 0xFFFF : 0x0000; // White/black stripes
    uint8_t colorHigh = (color >> 8) & 0xFF;
    uint8_t colorLow = color & 0xFF;
    
    for (int x = 0; x < 320; x++) {
      SPI.transfer(colorHigh);
      SPI.transfer(colorLow);
    }
  }
  
  digitalWrite(TFT_CS, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== HSPI Display Test ===");
  Serial.println("VERSION: 2025-01-03 16:00 UTC");
  Serial.println("Using CORRECT pins: MOSI=13, SCK=14, MISO=12");
  
  // Setup backlight
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);
  Serial.println("Backlight ON");
  
  // Setup control pins
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_RST, HIGH);
  
  // Initialize SPI with HSPI pins
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  SPI.setFrequency(4000000);
  SPI.setDataMode(SPI_MODE0);
  Serial.println("HSPI initialized");
  
  // Hardware reset
  Serial.println("Performing hardware reset...");
  digitalWrite(TFT_RST, LOW);
  delay(10);
  digitalWrite(TFT_RST, HIGH);
  delay(120);
  
  // Read display ID again to confirm
  Serial.println("Reading display ID...");
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(0x09);
  digitalWrite(TFT_DC, HIGH);
  uint8_t id1 = SPI.transfer(0x00);
  uint8_t id2 = SPI.transfer(0x00);
  uint8_t id3 = SPI.transfer(0x00);
  digitalWrite(TFT_CS, HIGH);
  Serial.printf("Display ID: 0x%02X 0x%02X 0x%02X\n", id1, id2, id3);
  
  // Initialize display
  Serial.println("Initializing display...");
  
  // Software reset
  sendCommand(0x01);
  delay(120);
  
  // Sleep out
  sendCommand(0x11);
  delay(120);
  
  // Pixel format - 16 bit
  sendCommand(0x3A);
  sendData(0x55);
  
  // Memory access control
  sendCommand(0x36);
  sendData(0x48); // BGR, row/column exchange
  
  // Display on
  sendCommand(0x29);
  delay(10);
  
  Serial.println("Testing screen fills...");
  
  // Test 1: Red screen
  Serial.println("Filling screen RED...");
  fillScreen(0xF800); // Red
  delay(2000);
  
  // Test 2: Green screen  
  Serial.println("Filling screen GREEN...");
  fillScreen(0x07E0); // Green
  delay(2000);
  
  // Test 3: Blue screen
  Serial.println("Filling screen BLUE...");
  fillScreen(0x001F); // Blue
  delay(2000);
  
  // Test 4: White screen
  Serial.println("Filling screen WHITE...");
  fillScreen(0xFFFF); // White
  delay(2000);
  
  // Test 5: Simple pattern
  Serial.println("Drawing simple pattern...");
  drawPattern();
  
  Serial.println("=== Display tests complete ===");
  Serial.println("If you see colors on the display, SUCCESS!");
  Serial.println("VERSION: 2025-01-03 16:00 UTC - End of setup()");
}

void loop() {
  static unsigned long lastBlink = 0;
  static bool state = true;
  
  if (millis() - lastBlink > 3000) {
    lastBlink = millis();
    state = !state;
    digitalWrite(BACKLIGHT, state);
    Serial.println(state ? "Backlight ON - VERSION: 2025-01-03 16:00 UTC" : "Backlight OFF");
  }
}

// VERSION: 2025-01-03 16:00 UTC - End of file