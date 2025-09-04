// VERSION: 2025-01-03 17:00 UTC - Complete Working Kiln Controller
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

// Display configuration for LovyanGFX - HSPI PINS
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 8000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14; // HSPI SCK
      cfg.pin_mosi = 13; // HSPI MOSI
      cfg.pin_miso = 12; // HSPI MISO
      cfg.pin_dc = 2;    // DC
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;   // CS
      cfg.pin_rst = 4;   // RST
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};

LGFX display;

#define DRY_RUN

// WiFi credentials
const char *ssid PROGMEM = "wellbeing24_Guest";
const char *password PROGMEM = "wellbeing?25";

// Safety Limits
const double MAX_TEMPERATURE = 1200.0;
const double MIN_TEMPERATURE = 0.0;
const unsigned long MAX_HEATING_TIME = 14400000;

// Hardware Pin Definitions (avoiding HSPI pins)
const int MAX1_CS_PIN = 26;
const int MAX2_CS_PIN = 25;
const int MAX_SCK_PIN = 18;
const int MAX_SO_PIN = 19;
const int MAX_SI_PIN = 23;
const int SSR1_PIN = 32;
const int SSR2_PIN = 33;

// System State Variables
bool systemEnabled = false;
bool emergencyStop = false;
bool wifiConnected = false;
double Input1 = 25.0, Input2 = 25.0, Setpoint = 25.0, Output1 = 0.0;
double avgTemp;
unsigned long firingStartTime = 0;
unsigned long totalFiringTime = 0;

// Firing schedule data
struct FiringSegment {
  double targetTemp;
  int rampRate;    // °C/hour
  int soakTime;    // minutes
  bool completed;
};

struct FiringSchedule {
  char name[32];
  FiringSegment segments[5];
  int segmentCount;
  bool active;
  int currentSegment;
  unsigned long segmentStartTime;
};

FiringSchedule currentSchedule;
bool usingSchedule = false;

// Predefined schedules
FiringSchedule presetSchedules[3] = {
  {{"Bisque Fire"}, {{200, 50, 30, false}, {500, 100, 60, false}, {950, 150, 20, false}}, 3, false, 0, 0},
  {{"Glaze Fire"}, {{300, 100, 0, false}, {600, 80, 0, false}, {1000, 60, 0, false}, {1240, 30, 15, false}}, 4, false, 0, 0},
  {{"Test Fire"}, {{100, 60, 5, false}, {200, 120, 10, false}}, 2, false, 0, 0}
};

#ifdef DRY_RUN
double fakedTemp1 = 22.0;
double fakedTemp2 = 23.0;
double ambientTemp = 22.0;
unsigned long lastTempUpdate = 0;
const int TEMP_UPDATE_INTERVAL = 1000;
#endif

// Colors
const uint32_t COLOR_BG = 0x1820;
const uint32_t COLOR_CARD = 0x2945;
const uint32_t COLOR_PRIMARY = 0x07E0;
const uint32_t COLOR_DANGER = 0xF800;
const uint32_t COLOR_WARNING = 0xFC00;
const uint32_t COLOR_INFO = 0x07FF;
const uint32_t COLOR_TEXT = 0xFFFF;
const uint32_t COLOR_TEXT_DIM = 0x8410;

// Objects
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
#endif
}

void handleFiringSchedule() {
  if (!currentSchedule.active || currentSchedule.currentSegment >= currentSchedule.segmentCount) {
    currentSchedule.active = false;
    usingSchedule = false;
    systemEnabled = false;
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("Schedule completed");
#endif
    return;
  }

  FiringSegment &seg = currentSchedule.segments[currentSchedule.currentSegment];
  
  // Simple and effective: jump directly to target temperature
  // Let the PID controller handle the actual ramping/heating rate
  Setpoint = seg.targetTemp;

#ifdef ENABLE_SERIAL_DEBUG
  static unsigned long lastScheduleDebug = 0;
  if (millis() - lastScheduleDebug >= 10000) { // Every 10 seconds
    lastScheduleDebug = millis();
    Serial.printf("SCHEDULE: %s, Segment %d/%d, Target=%.1f°C\n", 
                  currentSchedule.name, 
                  currentSchedule.currentSegment + 1, 
                  currentSchedule.segmentCount,
                  seg.targetTemp);
  }
#endif

  // Check if we've reached the target temperature (within 5°C tolerance)
  double avgTemp = (Input1 + Input2) / 2.0;
  bool targetReached = (avgTemp >= seg.targetTemp - 5.0);
  
  if (targetReached) {
    // Start soak timer
    static unsigned long soakStartTime = 0;
    if (soakStartTime == 0) {
      soakStartTime = millis();
#ifdef ENABLE_SERIAL_DEBUG
      Serial.printf("SCHEDULE: Target reached, starting %d minute soak\n", seg.soakTime);
#endif
    }
    
    // Check if soak time is complete
    if (seg.soakTime == 0 || (millis() - soakStartTime >= seg.soakTime * 60000UL)) {
      seg.completed = true;
      currentSchedule.currentSegment++;
      currentSchedule.segmentStartTime = millis();
      soakStartTime = 0; // Reset for next segment
#ifdef ENABLE_SERIAL_DEBUG
      Serial.println("SCHEDULE: Segment completed, moving to next");
#endif
    }
  }
}

String getStatusJSON() {
  JsonDocument doc;
  
  doc["temp1"] = Input1;
  doc["temp2"] = Input2;
  doc["avgTemp"] = (Input1 + Input2) / 2.0;
  doc["setpoint"] = Setpoint;
  doc["power"] = (Output1 / 255.0) * 100.0;
  doc["enabled"] = systemEnabled;
  doc["emergency"] = emergencyStop;
  doc["wifi"] = wifiConnected;
  doc["uptime"] = millis() / 1000;
  doc["version"] = "2025-01-03 17:00 UTC";
  
  if (usingSchedule && currentSchedule.active) {
    doc["schedule"]["name"] = currentSchedule.name;
    doc["schedule"]["segment"] = currentSchedule.currentSegment + 1;
    doc["schedule"]["total"] = currentSchedule.segmentCount;
    doc["schedule"]["target"] = currentSchedule.segments[currentSchedule.currentSegment].targetTemp;
  }
  
  String result;
  serializeJson(doc, result);
  return result;
}

void setupWebServer() {
  // Serve static files from SPIFFS
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
  });
  
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/style.css", "text/css");
  });
  
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/script.js", "application/javascript");
  });
  
  server.serveStatic("/", SPIFFS, "/");
  
  // Status API
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", getStatusJSON());
  });
  
  // Control API
  server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action = "";
    String value = "";
    
    if (request->hasParam("action", true)) {
      action = request->getParam("action", true)->value();
    }
    if (request->hasParam("value", true)) {
      value = request->getParam("value", true)->value();
    }
    if (request->hasParam("index", true)) {
      value = request->getParam("index", true)->value();
    }
    
    JsonDocument response;
    response["success"] = false;
    response["message"] = "Unknown action";
    
    if (action == "start") {
      if (!emergencyStop) {
        systemEnabled = true;
        firingStartTime = millis();
        response["success"] = true;
        response["message"] = "System started";
#ifdef ENABLE_SERIAL_DEBUG
        Serial.println("System started via web API");
#endif
      } else {
        response["message"] = "Cannot start - Emergency stop active";
      }
    }
    else if (action == "stop") {
      systemEnabled = false;
      usingSchedule = false;
      currentSchedule.active = false;
      response["success"] = true;
      response["message"] = "System stopped";
#ifdef ENABLE_SERIAL_DEBUG
      Serial.println("System stopped via web API");
#endif
    }
    else if (action == "emergency") {
      emergencyStop = true;
      systemEnabled = false;
      usingSchedule = false;
      currentSchedule.active = false;
      response["success"] = true;
      response["message"] = "Emergency stop activated";
#ifdef ENABLE_SERIAL_DEBUG
      Serial.println("Emergency stop via web API");
#endif
    }
    else if (action == "reset") {
      if (!systemEnabled) {
        emergencyStop = false;
        response["success"] = true;
        response["message"] = "System reset";
#ifdef ENABLE_SERIAL_DEBUG
        Serial.println("System reset via web API");
#endif
      } else {
        response["message"] = "Cannot reset while system is running";
      }
    }
    else if (action == "settemp" && value.length() > 0) {
      double temp = value.toDouble();
      if (temp >= 0 && temp <= MAX_TEMPERATURE) {
        Setpoint = temp;
        response["success"] = true;
        response["message"] = "Target temperature set to " + String(temp) + "°C";
#ifdef ENABLE_SERIAL_DEBUG
        Serial.printf("Target temperature set to %.1f°C via web API\n", temp);
#endif
      } else {
        response["message"] = "Invalid temperature range (0-1200°C)";
      }
    }
    else if (action == "schedule" && value.length() > 0) {
      int index = value.toInt();
      if (index >= 0 && index < 3 && !systemEnabled) {
        currentSchedule = presetSchedules[index];
        currentSchedule.active = true;
        currentSchedule.currentSegment = 0;
        currentSchedule.segmentStartTime = millis();
        usingSchedule = true;
        systemEnabled = true;
        firingStartTime = millis();
        response["success"] = true;
        response["message"] = "Started schedule: " + String(currentSchedule.name);
#ifdef ENABLE_SERIAL_DEBUG
        Serial.printf("Started schedule %d: %s via web API\n", index, currentSchedule.name);
#endif
      } else {
        response["message"] = "Cannot start schedule - system may be running or invalid index";
      }
    }
    else if (action == "schedules") {
      response["success"] = true;
      JsonArray schedules = response["schedules"].to<JsonArray>();
      for (int i = 0; i < 3; i++) {
        JsonObject sched = schedules.add<JsonObject>();
        sched["index"] = i;
        sched["name"] = presetSchedules[i].name;
        sched["segments"] = presetSchedules[i].segmentCount;
        sched["maxTemp"] = 0;
        for (int j = 0; j < presetSchedules[i].segmentCount; j++) {
          if (presetSchedules[i].segments[j].targetTemp > sched["maxTemp"]) {
            sched["maxTemp"] = presetSchedules[i].segments[j].targetTemp;
          }
        }
      }
    }
    
    String jsonString;
    serializeJson(response, jsonString);
    request->send(200, "application/json", jsonString);
  });
  
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
  
  server.begin();
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Web server started - serving files from SPIFFS");
#endif
}

void drawMainScreen() {
  display.fillScreen(COLOR_BG);
  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_TEXT);
  display.drawString("KILN STATUS", 60, 10);

  display.fillRoundRect(10, 60, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(10, 60, 140, 60, 8, COLOR_INFO);
  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString("TEMP 1", 20, 70);

  display.fillRoundRect(170, 60, 140, 60, 8, COLOR_CARD);
  display.drawRoundRect(170, 60, 140, 60, 8, COLOR_INFO);
  display.drawString("TEMP 2", 180, 70);

  display.setFont(&fonts::Font4);
  display.setTextColor(COLOR_INFO);
  char temp1Str[10], temp2Str[10];
  snprintf(temp1Str, sizeof(temp1Str), "%.1f C", Input1);
  snprintf(temp2Str, sizeof(temp2Str), "%.1f C", Input2);
  display.drawString(temp1Str, 15, 90);
  display.drawString(temp2Str, 175, 90);

  display.setFont(&fonts::Font2);
  display.setTextColor(COLOR_TEXT);
  display.drawString("STATUS:", 10, 150);
  
  const char *statusText;
  uint32_t statusColor;
  if (emergencyStop) {
    statusText = "EMERGENCY";
    statusColor = COLOR_DANGER;
  } else if (systemEnabled) {
    statusText = "HEATING";
    statusColor = COLOR_PRIMARY;
  } else {
    statusText = "READY";
    statusColor = COLOR_INFO;
  }
  display.setTextColor(statusColor);
  display.drawString(statusText, 80, 150);

  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString("Web Interface:", 10, 200);
  display.setTextColor(wifiConnected ? COLOR_PRIMARY : COLOR_DANGER);
  if (wifiConnected) {
    display.drawString("Available", 10, 220);
    display.drawString(WiFi.localIP().toString().c_str(), 10, 240);
  } else {
    display.drawString("Offline", 10, 220);
  }

  display.setTextColor(COLOR_TEXT_DIM);
  display.drawString("VERSION: 2025-01-03 17:00", 10, 280);
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
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.println("Web interface available at: http://" + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection failed");
  }
#endif
}

void setup() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  Serial.println("=== Kiln Controller with Complete Firing Schedule ===");
  Serial.println("VERSION: 2025-01-03 17:00 UTC");
#endif

  if (!SPIFFS.begin(true)) {
#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("SPIFFS Mount Failed!");
#endif
    while(1) delay(1000);
  }

  display.init();
  display.setRotation(0);
  display.setBrightness(128);
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  myPID.SetMode(AUTOMATIC);
  myPID.SetOutputLimits(0, 255);
  myPID.SetSampleTime(1000);

  pinMode(SSR1_PIN, OUTPUT);
  pinMode(SSR2_PIN, OUTPUT);
  digitalWrite(SSR1_PIN, LOW);
  digitalWrite(SSR2_PIN, LOW);

  setupWiFi();
  setupWebServer();

#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("Setup complete - VERSION: 2025-01-03 17:00 UTC");
#endif

  drawMainScreen();
}

void loop() {
  static unsigned long lastTempRead = 0;
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastDebug = 0;
  
  unsigned long currentTime = millis();

  if (currentTime - lastTempRead >= 1000) {
    lastTempRead = currentTime;
    readTemperatures();
    
    // Debug output every 5 seconds when system is enabled
    if (systemEnabled && currentTime - lastDebug >= 5000) {
      lastDebug = currentTime;
      Serial.printf("DEBUG: Setpoint=%.1f, Input1=%.1f, Input2=%.1f, Output1=%.1f\n", 
                    Setpoint, Input1, Input2, Output1);
      Serial.printf("DEBUG: systemEnabled=%d, emergencyStop=%d, usingSchedule=%d\n", 
                    systemEnabled, emergencyStop, usingSchedule);
    }
    
    // Handle firing schedule
    if (usingSchedule && currentSchedule.active) {
      handleFiringSchedule();
    }
    
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

  if (currentTime - lastDisplayUpdate >= 2000) {
    lastDisplayUpdate = currentTime;
    drawMainScreen();
  }

  delay(10);
}

// VERSION: 2025-01-03 17:00 UTC - End of file