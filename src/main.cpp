// Uncomment the line below if you have an MCP23017 for physical buttons
#define USE_MCP23017

#include <Arduino.h>
#include <Adafruit_MAX31856.h>
#ifdef USE_MCP23017
#include "Adafruit_MCP23X17.h"
#endif
#include <PID_v1.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

// Display configuration for LovyanGFX
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 23;
      cfg.pin_miso = 19;
      cfg.pin_dc = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = 4;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = -1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = VSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 23;
      cfg.pin_miso = 19;
      cfg.pin_cs = 5;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

// Uncomment this line to enable a dry run without the thermocouple
#define DRY_RUN

// WiFi credentials - change these!
const char *ssid = "wellbeing24_Guest";
const char *password = "wellbeing?25";

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
const int SDA_PIN = 32; // 16, 13, 18, 19, 21
const int SCL_PIN = 25; // 17, 14, 19, 18, 22
const int MCP_I2C_ADDR = 0x27;

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
  String name;
  FiringSegment segments[10]; // Max 10 segments
  int segmentCount;
  bool active;
  int currentSegment;
  unsigned long segmentStartTime;
};

// Predefined firing schedules
FiringSchedule presetSchedules[] = {
    {"Bisque Fire",
     {
         {200, 50, 30, false},  // Slow warm-up
         {500, 100, 60, false}, // Dehydration hold
         {950, 150, 20, false}, // Final bisque temp
     },
     3,
     false,
     0,
     0},
    {"Glaze Fire",
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
    {"Test Fire",
     {
         {100, 60, 5, false},   // Gentle test
         {200, 120, 10, false}, // Hold
     },
     2,
     false,
     0,
     0}};

// --- Object Instantiation ---
LGFX display;
Adafruit_MAX31856 maxsensor1(MAX1_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
Adafruit_MAX31856 maxsensor2(MAX2_CS_PIN, MAX_SI_PIN, MAX_SO_PIN, MAX_SCK_PIN);
#ifdef USE_MCP23017
Adafruit_MCP23X17 mcp;
#endif
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

const int MAX_DATA_POINTS = 200;
DataPoint dataLog[MAX_DATA_POINTS];
int dataIndex = 0;
int validDataCount = 0;

// --- Touch handling ---
struct TouchArea
{
  int x, y, w, h;
  int id;
};

TouchArea touchAreas[20]; // Max 20 touch areas
int touchAreaCount = 0;

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
void drawMainScreen();
void readTemperatures();
void logData();
void handleTouch();
void handleFiringSchedule();
void performSafetyChecks();
void updateCurrentScreen();
void drawTemperatureCard(int x, int y, const char *label, double temp, uint32_t color);
void drawPowerBar(int x, int y);
void drawStatusArea(int x, int y);
void drawMainButtons();
void drawScheduleProgress(int x, int y);
void addTouchArea(int x, int y, int w, int h, int id);
void handleTouchAction(int actionId);
void drawManualScreen();
void drawSchedulesScreen();
void drawGraphScreen();
void drawSettingsScreen();

void setup()
{
  Serial.begin(115200);
  Serial.println("Advanced Kiln Controller Starting...");

  // Initialize display
  display.init();
  display.setRotation(0); // Portrait
  display.setBrightness(128);

  drawBootScreen();

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("Failed to initialize SPIFFS");
    displayError("SPIFFS Failed");
    while (1)
      delay(1000);
  }

  // Initialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  // More detailed I2C scanner
  Serial.println("Starting detailed I2C scan...");
  Serial.printf("SDA pin: %d, SCL pin: %d\n", SDA_PIN, SCL_PIN);

  for (byte address = 1; address < 127; address++)
  {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0)
    {
      Serial.printf("Device found at 0x%02X\n", address);
    }
    else if (error == 4)
    {
      Serial.printf("Unknown error at 0x%02X\n", address);
    }
  }
  Serial.println("I2C scan complete");

  // Initialize MCP23017 (optional - only if you have physical buttons)
#ifdef USE_MCP23017
  if (!mcp.begin_I2C(MCP_I2C_ADDR))
  {
    Serial.println("Warning: Cannot connect to MCP23017 - continuing without it");
  }
  else
  {
    Serial.println("MCP23017 connected for physical buttons");
    // Configure MCP23017 pins as inputs with pullups
    for (int i = 2; i < 6; i++)
    {
      mcp.pinMode(i, INPUT_PULLUP);
    }
  }
#else
  Serial.println("MCP23017 support disabled - using touch only");
#endif

#ifndef DRY_RUN
  // Initialize thermocouple amplifiers
  if (!maxsensor1.begin())
  {
    Serial.println("Error: Cannot connect to MAX31856 #1");
    displayError("Thermocouple 1 Error");
    while (1)
      delay(1000);
  }

  if (!maxsensor2.begin())
  {
    Serial.println("Error: Cannot connect to MAX31856 #2");
    displayError("Thermocouple 2 Error");
    while (1)
      delay(1000);
  }

  maxsensor1.setThermocoupleType(MAX31856_TCTYPE_S);
  maxsensor2.setThermocoupleType(MAX31856_TCTYPE_S);

  Serial.println("Thermocouples initialized");
#endif

  // Initialize SSR pins
  // Configure pins A0 and A1 as outputs for SSRs
  mcp.pinMode(0, OUTPUT);
  mcp.pinMode(1, OUTPUT);
  mcp.digitalWrite(0, LOW); // SSR1 on pin A0
  mcp.digitalWrite(1, LOW); // SSR2 on pin A1

  // Initialize PID
  Setpoint = 100.0;
  myPID.SetMode(AUTOMATIC);
  myPID.SetOutputLimits(0, 255);
  myPID.SetSampleTime(250);

  // Setup WiFi and web server
  setupWiFi();
  setupWebServer();

  // Initialize data log
  for (int i = 0; i < MAX_DATA_POINTS; i++)
  {
    dataLog[i].timestamp = 0;
  }

  drawMainScreen();
  Serial.println("Kiln Controller Ready");
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

  // Handle touch input
  handleTouch();

  // Handle firing schedule
  if (usingSchedule && currentSchedule.active)
  {
    handleFiringSchedule();
  }

  // Safety checks
  performSafetyChecks();

  // PID control
  if (systemEnabled && !emergencyStop)
  {
    avgTemp = (Input1 + Input2) / 2.0;
    myPID.Compute();

#ifdef USE_MCP23017
    // Use MCP23017 pins A0 and A1 for SSRs
    mcp.digitalWrite(0, Output1 > 128 ? HIGH : LOW); // SSR1 on pin A0
    mcp.digitalWrite(1, Output1 > 128 ? HIGH : LOW); // SSR2 on pin A1
#else
    analogWrite(SSR1_PIN, (int)Output1);
    analogWrite(SSR2_PIN, (int)Output1);
#endif
  }
  else
  {
#ifdef USE_MCP23017
    mcp.digitalWrite(0, LOW);
    mcp.digitalWrite(1, LOW);
#else
    digitalWrite(SSR1_PIN, LOW);
    digitalWrite(SSR2_PIN, LOW);
#endif
    Output1 = 0;
  }
  // Update display
  if (currentTime - lastDisplayUpdate >= 1000)
  {
    lastDisplayUpdate = currentTime;
    updateCurrentScreen();
  }

  delay(10);
}

void drawBootScreen()
{
  display.fillScreen(COLOR_BG);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("🔥 KILN CONTROLLER", 20, 100);
  display.setFont(&fonts::Font2);
  display.drawString("Advanced Firing System", 50, 150);
  display.drawString("Initializing...", 100, 300);

#ifdef DRY_RUN
  display.setTextColor(COLOR_WARNING);
  display.drawString("** DRY RUN MODE **", 80, 250);
#endif
}

void drawMainScreen()
{
  display.fillScreen(COLOR_BG);
  touchAreaCount = 0;

  // Header
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("KILN STATUS", 60, 10);

  // Temperature cards
  drawTemperatureCard(10, 60, "TEMP 1", Input1, COLOR_INFO);
  drawTemperatureCard(170, 60, "TEMP 2", Input2, COLOR_INFO);

  drawTemperatureCard(10, 130, "AVERAGE", (Input1 + Input2) / 2.0, COLOR_PRIMARY);
  drawTemperatureCard(170, 130, "TARGET", Setpoint, COLOR_WARNING);

  // Power output bar
  drawPowerBar(10, 200);

  // Status
  drawStatusArea(10, 240);

  // Control buttons
  drawMainButtons();

  // Schedule info if active
  if (usingSchedule && currentSchedule.active)
  {
    drawScheduleProgress(10, 300);
  }
}

void drawTemperatureCard(int x, int y, const char *label, double temp, uint32_t color)
{
  display.fillRoundRect(x, y, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(x, y, 140, 60, 8, color);

  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString(label, x + 10, y + 5);

  display.setFont(&fonts::Font4);
  display.setTextColor(color);
  char tempStr[20];
  snprintf(tempStr, sizeof(tempStr), "%.1f°C", temp);
  display.drawString(tempStr, x + 10, y + 25);
}

void drawPowerBar(int x, int y)
{
  int barWidth = 300;
  int barHeight = 25;

  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("POWER OUTPUT", x, y - 20);

  // Background
  display.fillRoundRect(x, y, barWidth, barHeight, 4, COLOR_CARD);

  // Fill based on output
  int fillWidth = (int)((Output1 / 255.0) * barWidth);
  uint32_t fillColor = COLOR_PRIMARY;
  if (Output1 > 200)
    fillColor = COLOR_DANGER;
  else if (Output1 > 128)
    fillColor = COLOR_WARNING;

  if (fillWidth > 0)
  {
    display.fillRoundRect(x, y, fillWidth, barHeight, 4, fillColor);
  }

  // Percentage text
  char percentStr[10];
  snprintf(percentStr, sizeof(percentStr), "%.0f%%", (Output1 / 255.0) * 100);
  display.setTextColor(COLOR_TEXT);
  display.drawString(percentStr, x + barWidth + 10, y + 5);
}

void drawStatusArea(int x, int y)
{
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("STATUS:", x, y);

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

  display.setTextColor(statusColor);
  display.drawString(statusText, x + 80, y);

  // WiFi status
  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString("WiFi:", x, y + 20);
  display.setTextColor(wifiConnected ? COLOR_PRIMARY : COLOR_DANGER);
  display.drawString(wifiConnected ? "Connected" : "Disconnected", x + 50, y + 20);
}

void drawMainButtons()
{
  // Manual control button
  addTouchArea(10, 360, 90, 40, 1);
  display.fillRoundRect(10, 360, 90, 40, 8, COLOR_INFO);
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("MANUAL", 25, 375);

  // Schedules button
  addTouchArea(110, 360, 90, 40, 2);
  display.fillRoundRect(110, 360, 90, 40, 8, COLOR_PRIMARY);
  display.drawString("SCHEDULES", 118, 375);

  // Emergency stop
  addTouchArea(210, 360, 100, 40, 3);
  uint32_t stopColor = emergencyStop ? COLOR_DANGER : 0x8000; // Dark red
  display.fillRoundRect(210, 360, 100, 40, 8, stopColor);
  display.setTextColor(COLOR_TEXT);
  display.drawString("E-STOP", 235, 375);

  // Graph button
  addTouchArea(10, 410, 90, 40, 4);
  display.fillRoundRect(10, 410, 90, 40, 8, COLOR_WARNING);
  display.drawString("GRAPH", 30, 425);

  // Settings
  addTouchArea(110, 410, 90, 40, 5);
  display.fillRoundRect(110, 410, 90, 40, 8, 0x4208); // Purple
  display.drawString("SETTINGS", 120, 425);

  // Reset (if emergency)
  if (emergencyStop)
  {
    addTouchArea(210, 410, 100, 40, 6);
    display.fillRoundRect(210, 410, 100, 40, 8, COLOR_PRIMARY);
    display.drawString("RESET", 240, 425);
  }
}

void drawScheduleProgress(int x, int y)
{
  display.fillRoundRect(x, y, 300, 50, 8, COLOR_CARD);
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("SCHEDULE: " + currentSchedule.name, x + 10, y + 5);

  char segmentInfo[50];
  snprintf(segmentInfo, sizeof(segmentInfo), "Segment %d/%d: %.0f°C",
           currentSchedule.currentSegment + 1, currentSchedule.segmentCount,
           currentSchedule.segments[currentSchedule.currentSegment].targetTemp);

  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString(segmentInfo, x + 10, y + 25);
}

void addTouchArea(int x, int y, int w, int h, int id)
{
  if (touchAreaCount < 20)
  {
    touchAreas[touchAreaCount] = {x, y, w, h, id};
    touchAreaCount++;
  }
}

void handleTouch()
{
  lgfx::touch_point_t tp;
  if (display.getTouch(&tp) && tp.size > 0)
  {
    for (int i = 0; i < touchAreaCount; i++)
    {
      TouchArea &area = touchAreas[i];
      if (tp.x >= area.x && tp.x <= area.x + area.w &&
          tp.y >= area.y && tp.y <= area.y + area.h)
      {
        handleTouchAction(area.id);
        delay(200); // Simple debounce
        break;
      }
    }
  }
}

void handleTouchAction(int actionId)
{
  switch (currentScreen)
  {
  case SCREEN_MAIN:
    switch (actionId)
    {
    case 1:
      currentScreen = SCREEN_MANUAL;
      drawManualScreen();
      break;
    case 2:
      currentScreen = SCREEN_SCHEDULES;
      drawSchedulesScreen();
      break;
    case 3:
      emergencyStop = true;
      systemEnabled = false;
      break;
    case 4:
      currentScreen = SCREEN_GRAPH;
      drawGraphScreen();
      break;
    case 5:
      currentScreen = SCREEN_SETTINGS;
      drawSettingsScreen();
      break;
    case 6:
      if (!systemEnabled)
        emergencyStop = false;
      break;
    }
    break;
    // Add other screen handlers...
  }
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

      if (fakedTemp1 < ambientTemp)
        fakedTemp1 = ambientTemp;
      if (fakedTemp2 < ambientTemp)
        fakedTemp2 = ambientTemp;
      if (fakedTemp1 > 1300)
        fakedTemp1 = 1300;
      if (fakedTemp2 > 1300)
        fakedTemp2 = 1300;
    }
    else
    {
      double coolingRate = 0.995;
      fakedTemp1 = fakedTemp1 * coolingRate + ambientTemp * (1 - coolingRate);
      fakedTemp2 = fakedTemp2 * coolingRate + ambientTemp * (1 - coolingRate);

      if (fakedTemp1 < ambientTemp + 0.1)
        fakedTemp1 = ambientTemp;
      if (fakedTemp2 < ambientTemp + 0.1)
        fakedTemp2 = ambientTemp;
    }
  }
  Input1 = fakedTemp1;
  Input2 = fakedTemp2;
#else
  Input1 = maxsensor1.readThermocoupleTemperature();
  Input2 = maxsensor2.readThermocoupleTemperature();

  uint8_t fault1 = maxsensor1.readFault();
  uint8_t fault2 = maxsensor2.readFault();

  if (fault1 || fault2)
  {
    emergencyStop = true;
    systemEnabled = false;
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

void handleFiringSchedule()
{
  // Implementation for firing schedule management
  // This will control ramp rates, soak times, etc.
}

void performSafetyChecks()
{
  if (Input1 > MAX_TEMPERATURE || Input2 > MAX_TEMPERATURE)
  {
    emergencyStop = true;
    systemEnabled = false;
  }

  if (systemEnabled && (millis() - heatingStartTime > MAX_HEATING_TIME))
  {
    emergencyStop = true;
    systemEnabled = false;
  }
}

void setupWiFi()
{
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  }
}

void setupWebServer()
{
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["temp1"] = Input1;
    doc["temp2"] = Input2;
    doc["avgTemp"] = (Input1 + Input2) / 2.0;
    doc["setpoint"] = Setpoint;
    doc["output"] = (Output1 / 255.0) * 100;
    doc["enabled"] = systemEnabled;
    doc["emergency"] = emergencyStop;
    doc["wifi"] = wifiConnected;
    doc["uptime"] = millis();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    JsonArray dataArray = doc["data"].to<JsonArray>();
    
    // Only return valid data points
    for (int i = 0; i < validDataCount && i < MAX_DATA_POINTS; i++) {
      int idx = (dataIndex - validDataCount + i + MAX_DATA_POINTS) % MAX_DATA_POINTS;
      if (dataLog[idx].timestamp > 0) {
        JsonObject point = dataArray.add<JsonObject>();
        point["time"] = dataLog[idx].timestamp;
        point["temp1"] = dataLog[idx].temp1;
        point["temp2"] = dataLog[idx].temp2;
        point["setpoint"] = dataLog[idx].setpoint;
        point["output"] = dataLog[idx].output;
      }
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Add more API endpoints...
  server.on("/api/setpoint", HTTP_POST, [](AsyncWebServerRequest *request)
            {
  if (request->hasParam("value", true)) {
    double newSetpoint = request->getParam("value", true)->value().toDouble();
    if (!systemEnabled && newSetpoint >= MIN_TEMPERATURE && newSetpoint <= MAX_TEMPERATURE) {
      Setpoint = newSetpoint;
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Invalid setpoint or system running");
    }
  } else {
    request->send(400, "text/plain", "Missing value parameter");
  } });

  server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *request)
            {
  if (!emergencyStop && Setpoint > MIN_TEMPERATURE) {
    systemEnabled = true;
    heatingStartTime = millis();
    request->send(200, "text/plain", "Started");
  } else {
    request->send(400, "text/plain", "Cannot start");
  } });

  server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *request)
            {
  systemEnabled = false;
  request->send(200, "text/plain", "Stopped"); });

  server.on("/api/emergency", HTTP_POST, [](AsyncWebServerRequest *request)
            {
  emergencyStop = true;
  systemEnabled = false;
  request->send(200, "text/plain", "Emergency stop activated"); });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request)
            {
  if (!systemEnabled) {
    emergencyStop = false;
    request->send(200, "text/plain", "System reset");
  } else {
    request->send(400, "text/plain", "Cannot reset while system is running");
  } });

  server.begin();
}

void updateCurrentScreen()
{
  switch (currentScreen)
  {
  case SCREEN_MAIN:
    drawMainScreen();
    break;
    // Add other screens...
  }
}

void drawManualScreen()
{
  // Implementation for manual control screen
}

void drawSchedulesScreen()
{
  // Implementation for firing schedules screen
}

void drawGraphScreen()
{
  // Implementation for graph screen
}

void drawSettingsScreen()
{
  // Implementation for settings screen
}

void displayError(const char *error)
{
  display.fillScreen(COLOR_DANGER);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("ERROR", 100, 100);
  display.setFont(&fonts::Font2);
  display.drawString(error, 50, 150);
}
