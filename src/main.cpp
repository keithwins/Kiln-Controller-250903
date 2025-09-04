// Uncomment the line below if you have an MCP23017 for physical buttons
// #define USE_MCP23017

#include <Arduino.h>
#include <Adafruit_MAX31856.h>
#include <PID_v1.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

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
  char name[20]; // Fixed-size to avoid String
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
LGFX display;
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
unsigned long lastMemoryCheck = 0;

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

// --- Touch handling ---
struct TouchArea
{
  int x, y, w, h;
  int id;
};

TouchArea touchAreas[20];
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

// Minimal index.html with graph
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Kiln Controller</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js@3.9.1/dist/chart.min.js"></script>
  <style>
    body { font-family: Arial, sans-serif; background: #1a252f; color: white; text-align: center; }
    .card { background: #2c3e50; border-radius: 8px; padding: 10px; margin: 10px; }
    button { padding: 10px 20px; margin: 5px; background: #2196F3; border: none; color: white; cursor: pointer; }
    button.danger { background: #d32f2f; }
    button:disabled { opacity: 0.5; }
    canvas { max-width: 600px; margin: 0 auto; }
    input { padding: 8px; margin: 5px; }
  </style>
</head>
<body>
  <h1>Kiln Controller</h1>
  <div class="card">
    <h2>Status</h2>
    <p>Temp 1: <span id="temp1">-</span>°C</p>
    <p>Temp 2: <span id="temp2">-</span>°C</p>
    <p>Average: <span id="avgTemp">-</span>°C</p>
    <p>Setpoint: <span id="setpoint">-</span>°C</p>
    <p>Output: <span id="output">-</span>%</p>
    <p>Status: <span id="status">-</span></p>
  </div>
  <div class="card">
    <h2>Firing Schedule</h2>
    <p>Schedule: <span id="scheduleName">-</span></p>
    <p>Segment: <span id="currentSegment">-</span>/<span id="totalSegments">-</span></p>
    <canvas id="tempChart" width="600" height="300"></canvas>
  </div>
  <div class="card">
    <h2>Control</h2>
    <input type="number" id="setpointInput" placeholder="Enter setpoint (°C)" step="0.1">
    <button onclick="setSetpoint()">Set Setpoint</button>
    <button onclick="start()">Start</button>
    <button onclick="stop()">Stop</button>
    <button class="danger" onclick="emergency()">Emergency Stop</button>
    <button onclick="reset()">Reset</button>
  </div>
  <script>
    let chart;
    function initChart() {
      const ctx = document.getElementById('tempChart').getContext('2d');
      chart = new Chart(ctx, {
        type: 'line',
        data: {
          datasets: [
            { label: 'Temp 1', data: [], borderColor: '#00f', fill: false },
            { label: 'Temp 2', data: [], borderColor: '#0ff', fill: false },
            { label: 'Setpoint', data: [], borderColor: '#f00', fill: false },
            { label: 'Schedule', data: [], borderColor: '#ff0', fill: false, borderDash: [5, 5] }
          ]
        },
        options: {
          scales: {
            x: { type: 'linear', title: { display: true, text: 'Time (s)' } },
            y: { title: { display: true, text: 'Temperature (°C)' }, suggestedMin: 0, suggestedMax: 1300 }
          }
        }
      });
    }
    function updateStatus() {
      fetch('/api/status').then(res => res.json()).then(data => {
        document.getElementById('temp1').innerText = data.temp1.toFixed(1);
        document.getElementById('temp2').innerText = data.temp2.toFixed(1);
        document.getElementById('avgTemp').innerText = data.avgTemp.toFixed(1);
        document.getElementById('setpoint').innerText = data.setpoint.toFixed(1);
        document.getElementById('output').innerText = data.output.toFixed(0);
        document.getElementById('status').innerText = data.emergency ? 'Emergency Stop' : data.enabled ? 'Heating' : 'Ready';
      }).catch(err => console.error('Status fetch error:', err));
    }
    function updateGraph() {
      fetch('/api/data').then(res => res.json()).then(data => {
        const now = Date.now() / 1000;
        chart.data.datasets[0].data = data.data.map(d => ({ x: (d.time / 1000), y: d.temp1 }));
        chart.data.datasets[1].data = data.data.map(d => ({ x: (d.time / 1000), y: d.temp2 }));
        chart.data.datasets[2].data = data.data.map(d => ({ x: (d.time / 1000), y: d.setpoint }));
        chart.update();
      }).catch(err => console.error('Data fetch error:', err));
      fetch('/api/schedule').then(res => res.json()).then(data => {
        document.getElementById('scheduleName').innerText = data.name || 'None';
        document.getElementById('currentSegment').innerText = data.currentSegment + 1;
        document.getElementById('totalSegments').innerText = data.segmentCount;
        chart.data.datasets[3].data = data.points.map(p => ({ x: p.time / 1000, y: p.temp }));
        chart.update();
      }).catch(err => console.error('Schedule fetch error:', err));
    }
    function setSetpoint() {
      const value = document.getElementById('setpointInput').value;
      fetch('/api/setpoint', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, 
        body: `value=${value}` 
      }).then(res => res.text()).then(alert)
        .catch(err => console.error('Setpoint fetch error:', err));
    }
    function start() { 
      fetch('/api/start', { method: 'POST' }).then(res => res.text()).then(alert)
        .catch(err => console.error('Start fetch error:', err)); 
    }
    function stop() { 
      fetch('/api/stop', { method: 'POST' }).then(res => res.text()).then(alert)
        .catch(err => console.error('Stop fetch error:', err)); 
    }
    function emergency() { 
      fetch('/api/emergency', { method: 'POST' }).then(res => res.text()).then(alert)
        .catch(err => console.error('Emergency fetch error:', err)); 
    }
    function reset() { 
      fetch('/api/reset', { method: 'POST' }).then(res => res.text()).then(alert)
        .catch(err => console.error('Reset fetch error:', err)); 
    }
    initChart();
    setInterval(updateStatus, 2000);
    setInterval(updateGraph, 5000);
    updateStatus();
    updateGraph();
  </script>
</body>
</html>
)rawliteral";

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
void checkMemory();

void setup()
{
  Serial.begin(115200);
  Serial.println("Advanced Kiln Controller Starting...");

  // Initialize display
  display.init();
  display.setRotation(0);
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
  Serial.println("Starting I2C scan...");
  for (byte address = 1; address < 127; address++)
  {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error == 0)
    {
      Serial.printf("Device found at 0x%02X\n", address);
    }
  }
  Serial.println("I2C scan complete");

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
  pinMode(SSR1_PIN, OUTPUT);
  pinMode(SSR2_PIN, OUTPUT);
  digitalWrite(SSR1_PIN, LOW);
  digitalWrite(SSR2_PIN, LOW);

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

  // Memory diagnostics
  if (currentTime - lastMemoryCheck >= 500)
  {
    lastMemoryCheck = currentTime;
    checkMemory();
  }

  delay(10);
}

void checkMemory()
{
  Serial.printf("Free Heap: %d bytes | Min Free Heap: %d bytes\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  size_t free8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t free32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
  Serial.printf("Free 8-bit: %d | Free 32-bit: %d\n", free8bit, free32bit);
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
}

void drawMainScreen()
{
  static double lastInput1 = -999, lastInput2 = -999, lastSetpoint = -999, lastOutput = -999;
  static bool lastSystemEnabled = false, lastEmergencyStop = false, lastWifiConnected = false;

  if (lastInput1 == -999)
  {
    display.fillScreen(COLOR_BG);
    touchAreaCount = 0;

    display.setFont(&fonts::Font4);
    display.setTextColor(COLOR_TEXT);
    display.drawString("KILN STATUS", 60, 10);

    display.fillRoundRect(10, 60, 140, 60, 8, COLOR_CARD);
    display.drawRoundRect(10, 60, 140, 60, 8, COLOR_INFO);
    display.setFont(&fonts::Font2);
    display.setTextColor(COLOR_TEXT_DIM);
    display.drawString("TEMP 1", 20, 65);

    display.fillRoundRect(170, 60, 140, 60, 8, COLOR_CARD);
    display.drawRoundRect(170, 60, 140, 60, 8, COLOR_INFO);
    display.drawString("TEMP 2", 180, 65);

    display.fillRoundRect(10, 130, 140, 60, 8, COLOR_CARD);
    display.drawRoundRect(10, 130, 140, 60, 8, COLOR_PRIMARY);
    display.drawString("AVERAGE", 20, 135);

    display.fillRoundRect(170, 130, 140, 60, 8, COLOR_CARD);
    display.drawRoundRect(170, 130, 140, 60, 8, COLOR_WARNING);
    display.drawString("TARGET", 180, 135);

    display.setFont(&fonts::Font2);
    display.setTextColor(COLOR_TEXT);
    display.drawString("POWER OUTPUT", 10, 180);
    display.fillRoundRect(10, 200, 300, 25, 4, COLOR_CARD);

    display.drawString("STATUS:", 10, 240);
    display.drawString("WiFi:", 10, 260);

    drawMainButtons();
  }

  if (Input1 != lastInput1)
  {
    drawTemperatureCard(10, 60, "TEMP 1", Input1, COLOR_INFO);
    lastInput1 = Input1;
  }
  if (Input2 != lastInput2)
  {
    drawTemperatureCard(170, 60, "TEMP 2", Input2, COLOR_INFO);
    lastInput2 = Input2;
  }
  if (Input1 != lastInput1 || Input2 != lastInput2)
  {
    drawTemperatureCard(10, 130, "AVERAGE", (Input1 + Input2) / 2.0, COLOR_PRIMARY);
  }
  if (Setpoint != lastSetpoint)
  {
    drawTemperatureCard(170, 130, "TARGET", Setpoint, COLOR_WARNING);
    lastSetpoint = Setpoint;
  }
  if (Output1 != lastOutput)
  {
    drawPowerBar(10, 200);
    lastOutput = Output1;
  }
  if (systemEnabled != lastSystemEnabled || emergencyStop != lastEmergencyStop || wifiConnected != lastWifiConnected)
  {
    drawStatusArea(10, 240);
    lastSystemEnabled = systemEnabled;
    lastEmergencyStop = emergencyStop;
    lastWifiConnected = wifiConnected;
  }

  if (usingSchedule && currentSchedule.active)
  {
    drawScheduleProgress(10, 300);
  }
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
  display.fillRect(x + barWidth + 10, y + 5, 50, 20, COLOR_BG);
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
  display.fillRect(x + 80, y, 200, 20, COLOR_BG);
  display.drawString(statusText, x + 80, y);

  display.setTextColor(wifiConnected ? COLOR_PRIMARY : COLOR_DANGER);
  display.fillRect(x + 50, y + 20, 100, 20, COLOR_BG);
  display.drawString(wifiConnected ? "Connected" : "Disconnected", x + 50, y + 20);
}

void drawMainButtons()
{
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);

  addTouchArea(10, 360, 90, 40, 1);
  display.fillRoundRect(10, 360, 90, 40, 8, COLOR_INFO);
  display.drawString("MANUAL", 25, 375);

  addTouchArea(110, 360, 90, 40, 2);
  display.fillRoundRect(110, 360, 90, 40, 8, COLOR_PRIMARY);
  display.drawString("SCHEDULES", 118, 375);

  addTouchArea(210, 360, 100, 40, 3);
  uint32_t stopColor = emergencyStop ? COLOR_DANGER : 0x8000;
  display.fillRoundRect(210, 360, 100, 40, 8, stopColor);
  display.drawString("E-STOP", 235, 375);

  addTouchArea(10, 410, 90, 40, 4);
  display.fillRoundRect(10, 410, 90, 40, 8, COLOR_WARNING);
  display.drawString("GRAPH", 30, 425);

  addTouchArea(110, 410, 90, 40, 5);
  display.fillRoundRect(110, 410, 90, 40, 8, 0x4208);
  display.drawString("SETTINGS", 120, 425);

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

  char nameStr[30];
  snprintf(nameStr, sizeof(nameStr), "SCHEDULE: %s", currentSchedule.name);
  display.drawString(nameStr, x + 10, y + 5);

  char segmentInfo[30];
  snprintf(segmentInfo, sizeof(segmentInfo), "Segment %d/%d: %.0f C",
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
  static unsigned long lastTouch = 0;
  lgfx::touch_point_t tp;
  if (display.getTouch(&tp) && tp.size > 0 && millis() - lastTouch > 200)
  {
    lastTouch = millis();
    for (int i = 0; i < touchAreaCount; i++)
    {
      TouchArea &area = touchAreas[i];
      if (tp.x >= area.x && tp.x <= area.x + area.w &&
          tp.y >= area.y && tp.y <= area.y + area.h)
      {
        handleTouchAction(area.id);
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
      drawMainScreen();
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
      {
        emergencyStop = false;
        drawMainScreen();
      }
      break;
    }
    break;
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
    Serial.printf("Thermocouple fault - MAX1: 0x%02X, MAX2: 0x%02X\n", fault1, fault2);
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
  // Placeholder for firing schedule logic
}

void performSafetyChecks()
{
  if (Input1 > MAX_TEMPERATURE || Input2 > MAX_TEMPERATURE)
  {
    emergencyStop = true;
    systemEnabled = false;
    Serial.println("Over-temperature detected");
  }

  if (systemEnabled && (millis() - heatingStartTime > MAX_HEATING_TIME))
  {
    emergencyStop = true;
    systemEnabled = false;
    Serial.println("Max heating time exceeded");
  }
}

void setupWiFi()
{
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.println("\nWiFi connection failed");
  }
}

void setupWebServer()
{
  Serial.println("Setting up web server...");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving / request");
              unsigned long start = millis();
              request->send_P(200, "text/html", index_html);
              Serial.printf("Served / in %lu ms\n", millis() - start);
            });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/status request");
              unsigned long start = millis();
              StaticJsonDocument<200> doc;
              doc["temp1"] = Input1;
              doc["temp2"] = Input2;
              doc["avgTemp"] = (Input1 + Input2) / 2.0;
              doc["setpoint"] = Setpoint;
              doc["output"] = (Output1 / 255.0) * 100;
              doc["enabled"] = systemEnabled;
              doc["emergency"] = emergencyStop;
              doc["wifi"] = wifiConnected;
              doc["uptime"] = millis();

              char response[200];
              serializeJson(doc, response, sizeof(response));
              request->send(200, "application/json", response);
              Serial.printf("Served /api/status in %lu ms\n", millis() - start);
            });

  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/data request");
              unsigned long start = millis();
              StaticJsonDocument<512> doc;
              JsonArray dataArray = doc["data"].to<JsonArray>();
              
              int pointsToSend = min(validDataCount, 25);
              for (int i = 0; i < pointsToSend; i++) {
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
              
              char response[512];
              serializeJson(doc, response, sizeof(response));
              request->send(200, "application/json", response);
              Serial.printf("Served /api/data in %lu ms\n", millis() - start);
            });

  server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/schedule request");
              unsigned long start = millis();
              StaticJsonDocument<256> doc;
              if (usingSchedule && currentSchedule.active) {
                doc["name"] = currentSchedule.name;
                doc["segmentCount"] = currentSchedule.segmentCount;
                doc["currentSegment"] = currentSchedule.currentSegment;
                JsonArray points = doc["points"].to<JsonArray>();
                unsigned long timeOffset = currentSchedule.segmentStartTime;
                double prevTemp = 0.0;
                for (int i = 0; i < currentSchedule.segmentCount; i++) {
                  FiringSegment &seg = currentSchedule.segments[i];
                  unsigned long rampTime = (seg.rampRate > 0) ? ((seg.targetTemp - prevTemp) * 3600000.0 / seg.rampRate) : 0;
                  unsigned long soakTime = seg.soakTime * 60000UL;
                  JsonObject point1 = points.add<JsonObject>();
                  point1["time"] = timeOffset;
                  point1["temp"] = prevTemp;
                  timeOffset += rampTime;
                  JsonObject point2 = points.add<JsonObject>();
                  point2["time"] = timeOffset;
                  point2["temp"] = seg.targetTemp;
                  if (seg.soakTime > 0) {
                    timeOffset += soakTime;
                    JsonObject point3 = points.add<JsonObject>();
                    point3["time"] = timeOffset;
                    point3["temp"] = seg.targetTemp;
                  }
                  prevTemp = seg.targetTemp;
                }
              } else {
                doc["name"] = "None";
                doc["segmentCount"] = 0;
                doc["currentSegment"] = 0;
                doc["points"] = JsonArray();
              }
              
              char response[256];
              serializeJson(doc, response, sizeof(response));
              request->send(200, "application/json", response);
              Serial.printf("Served /api/schedule in %lu ms\n", millis() - start);
            });

  server.on("/api/setpoint", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/setpoint request");
              unsigned long start = millis();
              double newSetpoint = -1;
              if (request->hasParam("value", true)) {
                Serial.println("Found 'value' in POST body");
                newSetpoint = request->getParam("value", true)->value().toDouble();
              } else if (request->hasParam("setpoint", true)) {
                Serial.println("Found 'setpoint' in POST body");
                newSetpoint = request->getParam("setpoint", true)->value().toDouble();
              } else if (request->hasParam("value")) {
                Serial.println("Found 'value' in query params");
                newSetpoint = request->getParam("value")->value().toDouble();
              } else if (request->hasParam("setpoint")) {
                Serial.println("Found 'setpoint' in query params");
                newSetpoint = request->getParam("setpoint")->value().toDouble();
              } else {
                Serial.println("Missing value or setpoint parameter");
                request->send(400, "text/plain", "Missing value or setpoint parameter");
                return;
              }
              if (!systemEnabled && newSetpoint >= MIN_TEMPERATURE && newSetpoint <= MAX_TEMPERATURE) {
                Setpoint = newSetpoint;
                Serial.printf("Setpoint updated to %.1f\n", newSetpoint);
                request->send(200, "text/plain", "OK");
              } else {
                Serial.println("Invalid setpoint or system running");
                request->send(400, "text/plain", "Invalid setpoint or system running");
              }
              Serial.printf("Served /api/setpoint in %lu ms\n", millis() - start);
            });

  server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/start request");
              unsigned long start = millis();
              if (!emergencyStop && Setpoint > MIN_TEMPERATURE) {
                systemEnabled = true;
                heatingStartTime = millis();
                request->send(200, "text/plain", "Started");
              } else {
                request->send(400, "text/plain", "Cannot start");
              }
              Serial.printf("Served /api/start in %lu ms\n", millis() - start);
            });

  server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/stop request");
              unsigned long start = millis();
              systemEnabled = false;
              request->send(200, "text/plain", "Stopped");
              Serial.printf("Served /api/stop in %lu ms\n", millis() - start);
            });

  server.on("/api/emergency", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/emergency request");
              unsigned long start = millis();
              emergencyStop = true;
              systemEnabled = false;
              request->send(200, "text/plain", "Emergency stop activated");
              Serial.printf("Served /api/emergency in %lu ms\n", millis() - start);
            });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("Serving /api/reset request");
              unsigned long start = millis();
              if (!systemEnabled) {
                emergencyStop = false;
                request->send(200, "text/plain", "System reset");
              } else {
                request->send(400, "text/plain", "Cannot reset while system is running");
              }
              Serial.printf("Served /api/reset in %lu ms\n", millis() - start);
            });

  // Fallback to SPIFFS if index.html exists
  if (SPIFFS.exists("/index.html"))
  {
    Serial.println("Serving index.html from SPIFFS");
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  }

  server.begin();
  Serial.println("Web server started");
}

void updateCurrentScreen()
{
  switch (currentScreen)
  {
  case SCREEN_MAIN:
    drawMainScreen();
    break;
  }
}

void drawManualScreen()
{
  // Placeholder for manual control screen
}

void drawSchedulesScreen()
{
  // Placeholder for firing schedules screen
}

void drawGraphScreen()
{
  // Placeholder for graph screen
}

void drawSettingsScreen()
{
  // Placeholder for settings screen
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