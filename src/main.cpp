// Uncomment to enable serial debug output (only when Serial Monitor is used)
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

// Display configuration for LovyanGFX - NO TOUCH
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 18; // SCK
      cfg.pin_mosi = 23; // MOSI
      cfg.pin_miso = 19; // MISO (optional)
      cfg.pin_dc = 2;    // DC
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;   // CS
      cfg.pin_rst = 4;   // RST
      cfg.pin_busy = -1; // Not used
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
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
const int MAX1_CS_PIN = 27; // First thermocouple
const int MAX2_CS_PIN = 26; // Second thermocouple
const int MAX_SCK_PIN = 18;
const int MAX_SO_PIN = 19;
const int MAX_SI_PIN = 23;

// SSR pins
const int SSR1_PIN = 32;
const int SSR2_PIN = 33;

// I2C pins
const int SDA_PIN = 32;
const int SCL_PIN = 25;

// --- Firing Schedule Structure ---
struct FiringSegment
{
  double targetTemp; // Target temperature for this segment
  int rampRate;      // Rate in °C/hour (0 = as fast as possible)
  int soakTime;      // Soak time in minutes
  bool completed;    // Has this segment been completed?
};

struct FiringSchedule
{
  char name[20];              // Fixed-size to avoid String
  FiringSegment segments[10]; // Max 10 segments
  int segmentCount;
  bool active;
  int currentSegment;
  unsigned long segmentStartTime;
};

// Predefined firing schedules
FiringSchedule presetSchedules[] = {
    {{"Bisque Fire"},
     {
         {200, 50, 30, false},  // Slow warm-up
         {500, 100, 60, false}, // Dehydration hold
         {950, 150, 20, false}, // Final bisque temp
     },
     3,
     false,
     0,
     0},
    {{"Glaze Fire"},
     {
         {300, 100, 0, false},  // Quick warm-up
         {600, 80, 0, false},   // Steady climb
         {1000, 60, 0, false},  // Approach glaze temp
         {1240, 30, 15, false}, // Glaze maturation
     },
     4,
     false,
     0,
     0},
    {{"Test Fire"},
     {
         {100, 60, 5, false},   // Gentle test
         {200, 120, 10, false}, // Hold
     },
     2,
     false,
     0,
     0}};

// --- Object Instantiation ---
Adafruit_MAX31856 maxsensor1(MAX1_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
Adafruit_MAX31856 maxsensor2(MAX2_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
AsyncWebServer server(80);

// --- PID Variables ---
double Setpoint, Input1, Input2, Output1, Output2;
double avgTemp;
PID myPID(&avgTemp, &Output1, &Setpoint, 50.0, 10.0, 5.0, DIRECT);

// --- System State Variables ---
bool systemEnabled = false;
bool emergencyStop = false;
bool wifiConnected = false;
unsigned long heatingStartTime = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTempRead = 0;
unsigned long lastWiFiCheck = 0;

FiringSchedule currentSchedule;
bool usingSchedule = false;

// --- UI State ---
enum UIScreen
{
  SCREEN_MAIN,
  SCREEN_SCHEDULES,
  SCREEN_MANUAL,
  SCREEN_GRAPH,
  SCREEN_SETTINGS
};
UIScreen currentScreen = SCREEN_MAIN;

// --- Data logging ---
struct DataPoint
{
  unsigned long timestamp;
  double temp1, temp2, setpoint, output;
};

const int MAX_DATA_POINTS = 100;
DataPoint dataLog[MAX_DATA_POINTS];
int dataIndex = 0;
int validDataCount = 0;

// --- Colors ---
const uint32_t COLOR_BG = 0x1820;       // Dark blue
const uint32_t COLOR_CARD = 0x2945;     // Card background
const uint32_t COLOR_PRIMARY = 0x07E0;  // Green
const uint32_t COLOR_DANGER = 0xF800;   // Red
const uint32_t COLOR_WARNING = 0xFC00;  // Orange
const uint32_t COLOR_INFO = 0x07FF;     // Cyan
const uint32_t COLOR_TEXT = 0xFFFF;     // White
const uint32_t COLOR_TEXT_DIM = 0x8410; // Gray

#ifdef DRY_RUN
double fakedTemp1 = 22.0;
double fakedTemp2 = 23.0;
double ambientTemp = 22.0;
double thermalMass = 0.02;
double heatLoss = 0.98;
unsigned long lastTempUpdate = 0;
const int TEMP_UPDATE_INTERVAL = 200;
#endif

// Forward declarations
void drawBootScreen();
void displayError(const char *error);
void setupWiFi();
void setupWebServer();
void checkWiFi();
void drawMainScreen();
void readTemperatures();
void logData();
void performSafetyChecks();
void updateCurrentScreen();
void drawTemperatureCard(int x, int y, const char *label, double temp, uint32_t color);
void drawPowerBar(int x, int y);
void drawStatusArea(int x, int y);

void setup()
{
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  Serial.println("Kiln Controller Starting...");
#endif

  // Initialize display first
  display.init();
  display.setRotation(0);
  display.setBrightness(128);

  // Set backlight pin
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Display initialized");
#endif

  // Draw boot screen
  drawBootScreen();
  delay(2000);

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Boot screen drawn");
#endif

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("SPIFFS Mount Failed");
#endif
    displayError("SPIFFS Failed");
    while(1) delay(1000);
  }

#ifndef DRY_RUN
  // Initialize thermocouples
  if (!maxsensor1.begin()) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("MAX31856 1 not found");
#endif
    displayError("Thermocouple 1 Error");
    while(1) delay(1000);
  }
  
  if (!maxsensor2.begin()) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("MAX31856 2 not found");
#endif
    displayError("Thermocouple 2 Error");
    while(1) delay(1000);
  }

  // Configure thermocouples
  maxsensor1.setThermocoupleType(MAX31856_TCTYPE_K);
  maxsensor2.setThermocoupleType(MAX31856_TCTYPE_K);
  maxsensor1.setConversionMode(MAX31856_CONTINUOUS);
  maxsensor2.setConversionMode(MAX31856_CONTINUOUS);
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

  // Initialize variables
  Setpoint = 25.0;
  Input1 = 25.0;
  Input2 = 25.0;
  Output1 = 0;

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Setup complete");
#endif

  // Draw main screen
  drawMainScreen();
}

void loop()
{
  unsigned long currentTime = millis();

  // Read temperatures
  if (currentTime - lastTempRead >= 250)
  {
    lastTempRead = currentTime;
    readTemperatures();
    logData();
  }

  // Safety checks
  performSafetyChecks();

  // PID control
  if (systemEnabled && !emergencyStop)
  {
    avgTemp = (Input1 + Input2) / 2.0;
    myPID.Compute();
    int outputInt = (int)Output1;
    analogWrite(SSR1_PIN, outputInt);
    analogWrite(SSR2_PIN, outputInt);
  }
  else
  {
    digitalWrite(SSR1_PIN, LOW);
    digitalWrite(SSR2_PIN, LOW);
    Output1 = 0;
  }

  // Update display
  if (currentTime - lastDisplayUpdate >= 1000)
  {
    lastDisplayUpdate = currentTime;
    updateCurrentScreen();
  }

  // WiFi status check
  if (currentTime - lastWiFiCheck >= 5000)
  {
    lastWiFiCheck = currentTime;
    checkWiFi();
  }

  delay(10);
}

void setupWiFi()
{
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Connecting to WiFi...");
#endif
  
  WiFi.begin(ssid, password);

  int attempts = 0;
  const int maxAttempts = 20;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts)
  {
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

void checkWiFi()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    wifiConnected = false;
  } else {
    wifiConnected = true;
  }
}

void setupWebServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              request->send(200, "text/html", "<h1>Kiln Controller</h1><p>Server OK</p>");
            });

  server.begin();
}

void drawBootScreen()
{
  display.fillScreen(COLOR_BG);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("KILN CONTROLLER", 20, 100);
  display.setFont(&fonts::Font2);
  display.drawString("Advanced Firing System", 50, 150);
  display.drawString("Initializing...", 100, 300);

#ifdef DRY_RUN
  display.setTextColor(COLOR_WARNING);
  display.drawString("** DRY RUN MODE **", 80, 250);
#endif

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Boot screen should be visible");
#endif
}

void drawMainScreen()
{
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Drawing main screen");
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
  display.drawString("TEMP 1", 20, 65);

  display.fillRoundRect(170, 60, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(170, 60, 140, 60, 8, COLOR_INFO);
  display.drawString("TEMP 2", 180, 65);

  // Draw temperature values
  drawTemperatureCard(10, 60, "TEMP 1", Input1, COLOR_INFO);
  drawTemperatureCard(170, 60, "TEMP 2", Input2, COLOR_INFO);

  // Status area
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("STATUS:", 10, 240);
  drawStatusArea(10, 240);

  display.drawString("WiFi:", 10, 260);
  display.setTextColor(wifiConnected ? COLOR_PRIMARY : COLOR_DANGER);
  display.drawString(wifiConnected ? "Connected" : "Disconnected", 50, 260);
}

void drawTemperatureCard(int x, int y, const char *label, double temp, uint32_t color)
{
  display.setFont(&fonts::Font4);
  display.setTextColor(color);
  char tempStr[10];
  snprintf(tempStr, sizeof(tempStr), "%.1f C", temp);
  display.fillRect(x + 10, y + 25, 120, 30, COLOR_CARD);
  display.drawString(tempStr, x + 10, y + 25);
}

void drawPowerBar(int x, int y)
{
  int barWidth = 300;
  int barHeight = 25;

  int fillWidth = (int)((Output1 / 255.0) * barWidth);
  uint32_t fillColor = COLOR_PRIMARY;
  if (Output1 > 200)
    fillColor = COLOR_DANGER;
  else if (Output1 > 128)
    fillColor = COLOR_WARNING;

  display.fillRect(x, y, barWidth, barHeight, COLOR_CARD);
  if (fillWidth > 0)
  {
    display.fillRoundRect(x, y, fillWidth, barHeight, 4, fillColor);
  }

  char percentStr[6];
  snprintf(percentStr, sizeof(percentStr), "%.0f%%", (Output1 / 255.0) * 100);
  display.setTextColor(COLOR_TEXT);
  display.drawString(percentStr, x + barWidth + 10, y + 5);
}

void drawStatusArea(int x, int y)
{
  const char *statusText;
  uint32_t statusColor;

  if (emergencyStop)
  {
    statusText = "EMERGENCY STOP";
    statusColor = COLOR_DANGER;
  }
  else if (systemEnabled)
  {
    statusText = "HEATING";
    statusColor = COLOR_PRIMARY;
  }
  else
  {
    statusText = "READY";
    statusColor = COLOR_INFO;
  }

  display.setFont(&fonts::Font2);
  display.setTextColor(statusColor);
  display.drawString(statusText, x + 80, y);
}

void readTemperatures()
{
#ifdef DRY_RUN
  if (millis() - lastTempUpdate >= TEMP_UPDATE_INTERVAL)
  {
    lastTempUpdate = millis();
    if (systemEnabled && !emergencyStop)
    {
      double heatInput = (Output1 / 255.0) * 15.0;
      fakedTemp1 = fakedTemp1 * heatLoss + (heatInput + ambientTemp) * thermalMass;
      fakedTemp2 = fakedTemp2 * heatLoss + (heatInput + ambientTemp) * thermalMass;
      fakedTemp1 += random(-20, 20) / 100.0;
      fakedTemp2 += random(-25, 25) / 100.0;
      fakedTemp1 = max(ambientTemp, min(1300.0, fakedTemp1));
      fakedTemp2 = max(ambientTemp, min(1300.0, fakedTemp2));
    }
    else
    {
      double coolingRate = 0.995;
      fakedTemp1 = fakedTemp1 * coolingRate + ambientTemp * (1 - coolingRate);
      fakedTemp2 = fakedTemp2 * coolingRate + ambientTemp * (1 - coolingRate);
      fakedTemp1 = max(ambientTemp, fakedTemp1);
      fakedTemp2 = max(ambientTemp, fakedTemp2);
    }
    Input1 = fakedTemp1;
    Input2 = fakedTemp2;
  }
#else
  Input1 = maxsensor1.readThermocoupleTemperature();
  Input2 = maxsensor2.readThermocoupleTemperature();

  uint8_t fault1 = maxsensor1.readFault();
  uint8_t fault2 = maxsensor2.readFault();
  if (fault1 || fault2)
  {
    emergencyStop = true;
    systemEnabled = false;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.printf("Thermocouple fault - MAX1: 0x%02X, MAX2: 0x%02X\n", fault1, fault2);
#endif
  }
#endif
}

void logData()
{
  if (validDataCount < MAX_DATA_POINTS)
    validDataCount++;

  dataLog[dataIndex].timestamp = millis();
  dataLog[dataIndex].temp1 = Input1;
  dataLog[dataIndex].temp2 = Input2;
  dataLog[dataIndex].setpoint = Setpoint;
  dataLog[dataIndex].output = (Output1 / 255.0) * 100;

  dataIndex = (dataIndex + 1) % MAX_DATA_POINTS;
}

void performSafetyChecks()
{
  if (Input1 > MAX_TEMPERATURE || Input2 > MAX_TEMPERATURE)
  {
    emergencyStop = true;
    systemEnabled = false;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("Over-temperature detected");
#endif
  }

  if (systemEnabled && (millis() - heatingStartTime > MAX_HEATING_TIME))
  {
    emergencyStop = true;
    systemEnabled = false;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("Max heating time exceeded");
#endif
  }
}

void updateCurrentScreen()
{
  drawMainScreen();
}

void displayError(const char *error)
{
  display.fillScreen(COLOR_DANGER);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("ERROR", 100, 100);
  display.setFont(&fonts::Font2);
  display.drawString(error, 50, 150);
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.printf("ERROR: %s\n", error);
#endif
}