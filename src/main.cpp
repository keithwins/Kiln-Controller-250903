// VERSION: 2025-01-03 16:05 UTC - Working Kiln Controller
#define ENABLE_SERIAL_DEBUG

#include <Arduino.h>
#include <Adafruit_MAX31856.h>
#include <PID_v1.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>

// Display configuration for LovyanGFX - CORRECTED FOR HSPI PINS
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = HSPI_HOST;  // Changed from VSPI_HOST to HSPI_HOST
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 8000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14; // Changed from 18 to 14 (HSPI SCK)
      cfg.pin_mosi = 13; // Changed from 23 to 13 (HSPI MOSI)
      cfg.pin_miso = 12; // Changed from 19 to 12 (HSPI MISO)
      cfg.pin_dc = 2;    // DC - same
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;   // CS - same
      cfg.pin_rst = 4;   // RST - same
      cfg.pin_busy = -1; // Not used
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false; // Don't share HSPI bus
      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};

LGFX display;

// Uncomment this line to enable a dry run without the thermocouple
#define DRY_RUN

// WiFi credentials - change these!
const char *ssid PROGMEM = "wellbeing24_Guest";
const char *password PROGMEM = "wellbeing?25";

// --- Safety Limits ---
const double MAX_TEMPERATURE = 1200.0;
const double MIN_TEMPERATURE = 0.0;
const unsigned long MAX_HEATING_TIME = 14400000; // 4 hours max heating time (ms)

// --- Hardware Pin Definitions ---
// Updated for HSPI - thermocouples use different pins to avoid conflict
const int MAX1_CS_PIN = 26; // First thermocouple (was 27, changed to avoid conflict)
const int MAX2_CS_PIN = 25; // Second thermocouple (was 26, changed)
const int MAX_SCK_PIN = 18; // Use VSPI pins for thermocouples
const int MAX_SO_PIN = 19;  // MISO for thermocouples
const int MAX_SI_PIN = 23;  // MOSI for thermocouples

// SSR pins
const int SSR1_PIN = 32;
const int SSR2_PIN = 33;

// I2C pins for your expander
const int SDA_PIN = 32;
const int SCL_PIN = 25;

// System State Variables
bool systemEnabled = false;
bool emergencyStop = false;
bool wifiConnected = false;
double Input1 = 25.0, Input2 = 25.0, Setpoint = 25.0, Output1 = 0.0;
double avgTemp;

#ifdef DRY_RUN
double fakedTemp1 = 22.0;
double fakedTemp2 = 23.0;
double ambientTemp = 22.0;
unsigned long lastTempUpdate = 0;
const int TEMP_UPDATE_INTERVAL = 1000;
#endif

// --- Colors ---
const uint32_t COLOR_BG = 0x1820;       // Dark blue
const uint32_t COLOR_CARD = 0x2945;     // Card background
const uint32_t COLOR_PRIMARY = 0x07E0;  // Green
const uint32_t COLOR_DANGER = 0xF800;   // Red
const uint32_t COLOR_WARNING = 0xFC00;  // Orange
const uint32_t COLOR_INFO = 0x07FF;     // Cyan
const uint32_t COLOR_TEXT = 0xFFFF;     // White
const uint32_t COLOR_TEXT_DIM = 0x8410; // Gray

// Object Instantiation
Adafruit_MAX31856 maxsensor1(MAX1_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
Adafruit_MAX31856 maxsensor2(MAX2_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
AsyncWebServer server(80);
PID myPID(&avgTemp, &Output1, &Setpoint, 50.0, 10.0, 5.0, DIRECT);

void readTemperatures() {
#ifdef DRY_RUN
  if (millis() - lastTempUpdate >= TEMP_UPDATE_INTERVAL) {
    lastTempUpdate = millis();
    if (systemEnabled && !emergencyStop) {
      double heatInput = (Output1 / 255.0) * 5.0;
      fakedTemp1 += heatInput + random(-10, 10) / 100.0;
      fakedTemp2 += heatInput + random(-12, 12) / 100.0;
      fakedTemp1 = max(ambientTemp, min(1300.0, fakedTemp1));
      fakedTemp2 = max(ambientTemp, min(1300.0, fakedTemp2));
    } else {
      fakedTemp1 = fakedTemp1 * 0.999 + ambientTemp * 0.001;
      fakedTemp2 = fakedTemp2 * 0.999 + ambientTemp * 0.001;
    }
    Input1 = fakedTemp1;
    Input2 = fakedTemp2;
  }
#else
  Input1 = maxsensor1.readThermocoupleTemperature();
  Input2 = maxsensor2.readThermocoupleTemperature();
  
  uint8_t fault1 = maxsensor1.readFault();
  uint8_t fault2 = maxsensor2.readFault();
  if (fault1 || fault2) {
    emergencyStop = true;
    systemEnabled = false;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.printf("Thermocouple fault - MAX1: 0x%02X, MAX2: 0x%02X\n", fault1, fault2);
#endif
  }
#endif
}

void drawBootScreen() {
  display.fillScreen(COLOR_BG);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("KILN CONTROLLER", 20, 100);
  display.setFont(&fonts::Font2);
  display.drawString("Advanced Firing System", 50, 150);
  display.drawString("VERSION: 2025-01-03 16:05", 50, 180);
  display.drawString("Initializing...", 100, 300);

#ifdef DRY_RUN
  display.setTextColor(COLOR_WARNING);
  display.drawString("** DRY RUN MODE **", 80, 250);
#endif

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Boot screen displayed - VERSION: 2025-01-03 16:05 UTC");
#endif
}

void drawMainScreen() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Drawing main screen - VERSION: 2025-01-03 16:05 UTC");
#endif

  display.fillScreen(COLOR_BG);

  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("KILN STATUS", 60, 10);

  // Temperature cards
  display.fillRoundRect(10, 60, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(10, 60, 140, 60, 8, COLOR_INFO);
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString("TEMP 1", 20, 70);

  display.fillRoundRect(170, 60, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(170, 60, 140, 60, 8, COLOR_INFO);
  display.drawString("TEMP 2", 180, 70);

  // Average temperature
  display.fillRoundRect(10, 130, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(10, 130, 140, 60, 8, COLOR_PRIMARY);
  display.drawString("AVERAGE", 20, 140);

  // Target temperature
  display.fillRoundRect(170, 130, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(170, 130, 140, 60, 8, COLOR_WARNING);
  display.drawString("TARGET", 180, 140);

  // Display temperature values
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_INFO);
  char temp1Str[10];
  snprintf(temp1Str, sizeof(temp1Str), "%.1f C", Input1);
  display.drawString(temp1Str, 15, 90);

  display.setTextColor(COLOR_INFO);
  char temp2Str[10];
  snprintf(temp2Str, sizeof(temp2Str), "%.1f C", Input2);
  display.drawString(temp2Str, 175, 90);

  display.setTextColor(COLOR_PRIMARY);
  char avgStr[10];
  snprintf(avgStr, sizeof(avgStr), "%.1f C", (Input1 + Input2) / 2.0);
  display.drawString(avgStr, 15, 160);

  display.setTextColor(COLOR_WARNING);
  char setStr[10];
  snprintf(setStr, sizeof(setStr), "%.1f C", Setpoint);
  display.drawString(setStr, 175, 160);

  // Status area
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("STATUS:", 10, 220);
  
  const char *statusText;
  uint32_t statusColor;
  
  if (emergencyStop) {
    statusText = "EMERGENCY STOP";
    statusColor = COLOR_DANGER;
  } else if (systemEnabled) {
    statusText = "HEATING";
    statusColor = COLOR_PRIMARY;
  } else {
    statusText = "READY";
    statusColor = COLOR_INFO;
  }
  
  display.setTextColor(statusColor);
  display.drawString(statusText, 80, 220);

  display.setTextColor(COLOR_TEXT);
  display.drawString("WiFi:", 10, 240);
  display.setTextColor(wifiConnected ? COLOR_PRIMARY : COLOR_DANGER);
  display.drawString(wifiConnected ? "Connected" : "Disconnected", 50, 240);

  // Power output
  display.setTextColor(COLOR_TEXT);
  display.drawString("POWER OUTPUT:", 10, 270);
  char powerStr[10];
  snprintf(powerStr, sizeof(powerStr), "%.0f%%", (Output1 / 255.0) * 100);
  display.drawString(powerStr, 150, 270);
}

void setupWiFi() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Connecting to WiFi...");
#endif
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.print(".");
#endif
  }
  
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  
#ifdef ENABLE_SERIAL_DEBUG
  if (wifiConnected) {
    Serial.println("\nWiFi connected!");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed");
  }
#endif
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<h1>Kiln Controller</h1><p>VERSION: 2025-01-03 16:05 UTC</p>");
  });
  server.begin();
}

void setup() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  Serial.println("=== Kiln Controller Starting ===");
  Serial.println("VERSION: 2025-01-03 16:05 UTC");
#endif

  // Initialize display with HSPI pins
  display.init();
  display.setRotation(0);
  display.setBrightness(128);

  // Set backlight pin
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Display initialized with HSPI pins");
#endif

  // Draw boot screen
  drawBootScreen();
  delay(3000);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("SPIFFS Mount Failed");
#endif
  }

#ifndef DRY_RUN
  // Initialize thermocouples on VSPI pins (separate from display)
  if (!maxsensor1.begin() || !maxsensor2.begin()) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("Thermocouple initialization failed");
#endif
  } else {
    maxsensor1.setThermocoupleType(MAX31856_TCTYPE_K);
    maxsensor2.setThermocoupleType(MAX31856_TCTYPE_K);
  }
#endif

  // Initialize PID
  myPID.SetMode(AUTOMATIC);
  myPID.SetOutputLimits(0, 255);
  myPID.SetSampleTime(1000);

  // Initialize SSR pins
  pinMode(SSR1_PIN, OUTPUT);
  pinMode(SSR2_PIN, OUTPUT);
  digitalWrite(SSR1_PIN, LOW);
  digitalWrite(SSR2_PIN, LOW);

  // Initialize WiFi
  setupWiFi();
  setupWebServer();

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Setup complete - VERSION: 2025-01-03 16:05 UTC");
#endif

  // Draw main screen
  drawMainScreen();
}

void loop() {
  static unsigned long lastTempRead = 0;
  static unsigned long lastDisplayUpdate = 0;
  
  unsigned long currentTime = millis();

  // Read temperatures
  if (currentTime - lastTempRead >= 1000) {
    lastTempRead = currentTime;
    readTemperatures();
    
    // PID control
    if (systemEnabled && !emergencyStop) {
      avgTemp = (Input1 + Input2) / 2.0;
      myPID.Compute();
      analogWrite(SSR1_PIN, (int)Output1);
      analogWrite(SSR2_PIN, (int)Output1);
    } else {
      digitalWrite(SSR1_PIN, LOW);
      digitalWrite(SSR2_PIN, LOW);
      Output1 = 0;
    }
  }

  // Update display
  if (currentTime - lastDisplayUpdate >= 2000) {
    lastDisplayUpdate = currentTime;
    drawMainScreen();
  }

  delay(10);
}

// VERSION: 2025-01-03 16:05 UTC - End of file