# Kiln-Controller-250906
Kiln Controller built on an ESP32 with local touch screen, utilizing an integrated screen, expander board, thermocouple amplifiers and SSRs

ESP32 Kiln Controller Project Summary
Version: 2025-01-03 16:40 UTC
 Status: Fully functional with simulated temperatures, ready for real thermocouple integration
Hardware Configuration
ESP32 Board
Board Type: ESP32 with 320x480 TFT display (Hosyond-style)
Display Controller: ST7796 (320x480 pixels)
Touch Controller: FT6x06 series (currently disabled)
Flash Memory: 1.31MB available for program
RAM: 327KB available
Critical Pin Assignments
Display (HSPI Bus):
MOSI: GPIO 13
SCK: GPIO 14
MISO: GPIO 12
CS: GPIO 15
DC: GPIO 2
RST: GPIO 4
Backlight: GPIO 27
Thermocouples (VSPI Bus - for future MAX31856 amplifiers):
MOSI: GPIO 23
SCK: GPIO 18
MISO: GPIO 19
CS1: GPIO 26 (First thermocouple)
CS2: GPIO 25 (Second thermocouple)
Control Systems:
SSR1: GPIO 32 (Solid State Relay 1)
SSR2: GPIO 33 (Solid State Relay 2)
I2C SDA: GPIO 32 (Expander board)
I2C SCL: GPIO 25 (Expander board, address 0x27)
External Hardware (Future Integration)
Thermocouple Amplifiers: 2x Adafruit MAX31856 (K-type thermocouples)
Solid State Relays: 2x high-power SSRs for kiln elements
I2C Expander: MCP23017 at address 0x27 for additional I/O
Power Supply: 3.3V for ESP32, appropriate voltage for SSRs
Software Architecture
Core Libraries
#include <LovyanGFX.hpp>        // Display driver
#include <WiFi.h>               // Network connectivity
#include <ESPAsyncWebServer.h>  // Web server
#include <ArduinoJson.h>        // JSON API responses
#include <SPIFFS.h>             // File system for web files
#include <Adafruit_MAX31856.h>  // Thermocouple amplifiers
#include <PID_v1.h>             // Temperature control

Display Configuration (LovyanGFX)
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  
  // Configuration uses HSPI_HOST with pins 13/14/12
  // Panel: 320x480, no color inversion, no RGB order swap
  // Bus shared: false (dedicated to display)
};

Temperature Simulation (DRY_RUN Mode)
Ambient Temperature: 22°C starting point
Heating Rate: Proportional to PID output (0-255)
Thermal Mass: 0.02 coefficient for realistic response
Heat Loss: 0.999 cooling factor when heating disabled
Noise: ±0.10-0.25°C random variation for realism
Update Interval: 1000ms
Control System
PID Controller: Kp=50.0, Ki=10.0, Kd=5.0, output 0-255
Safety Limits: 0-1200°C operating range
Maximum Runtime: 4 hours continuous heating
Emergency Stop: Immediately disables all heating
Temperature Averaging: (Temp1 + Temp2) / 2
Firing Schedules (Predefined)
Bisque Fire: 200°C (50°C/hr, 30min) → 500°C (100°C/hr, 60min) → 950°C (150°C/hr, 20min)
Glaze Fire: 300°C (100°C/hr) → 600°C (80°C/hr) → 1000°C (60°C/hr) → 1240°C (30°C/hr, 15min)
Test Fire: 100°C (60°C/hr, 5min) → 200°C (120°C/hr, 10min)
Web Interface
File Structure (SPIFFS)
/data/
├── index.html      # Main interface HTML
├── style.css       # Professional styling with glassmorphism
└── script.js       # Real-time updates and control logic

API Endpoints
GET /api/status - Returns JSON with all system data
POST /api/control - Handles control commands:
action=start - Start heating system
action=stop - Stop heating system
action=emergency - Emergency stop
action=reset - Reset from emergency state
action=settemp&value=XXX - Set target temperature
action=schedule&index=X - Start firing schedule
Web Features
Real-time Updates: 2-second polling of system status
Responsive Design: Works on desktop, tablet, mobile
Professional UI: Gradient backgrounds, glass-morphism cards, animations
Activity Log: Timestamped system events with export capability
Keyboard Shortcuts: Ctrl+S (start), Ctrl+X (stop), Ctrl+E (emergency)
Input Validation: Temperature range checking, confirmation dialogs
Network Configuration
WiFi Settings
const char *ssid = "wellbeing24_Guest";
const char *password = "wellbeing?25";

Static IP Configuration (Optional)
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);  
IPAddress subnet(255, 255, 255, 0);

Safety Systems
Temperature Monitoring
Over-temperature Shutdown: >1200°C triggers emergency stop
Sensor Fault Detection: MAX31856 fault register monitoring
Dual Sensor Redundancy: Two independent thermocouples
Time-based Protection
Maximum Heating Time: 4-hour automatic shutoff
Firing Schedule Timeouts: Automatic progression through segments
Watchdog Functions: Regular safety check intervals
User Controls
Emergency Stop: Immediate heating disable from web or physical interface
Manual Override: Direct temperature setting with bounds checking
System Reset: Recovery from emergency states (when safe)
Development Environment
PlatformIO Configuration
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

Upload Commands
pio run --target uploadfs    # Upload web files to SPIFFS
pio run --target upload      # Upload main program
pio device monitor           # Connect to serial monitor

Debug Features
Serial Output: 115200 baud with timestamped messages
Version Tracking: All code versions timestamped for debugging
Memory Monitoring: Heap and PSRAM usage reporting
SPIFFS File Listing: Verification of uploaded web files
Transition to Production
Hardware Integration Steps
Disable DRY_RUN: Comment out #define DRY_RUN in main.cpp
Connect MAX31856 Modules: Wire to VSPI pins as defined
Install SSRs: Connect to GPIO 32/33 with appropriate driver circuits
Add Safety Interlocks: Physical emergency stops, thermal fuses
Calibrate Thermocouples: Verify temperature accuracy
Testing Sequence
Sensor Verification: Confirm thermocouple readings at known temperatures
Control Loop Testing: Verify PID response with low-power heating
Safety System Testing: Emergency stops, over-temperature protection
Communication Testing: Web interface functionality under load
Full System Integration: Complete firing schedule execution
Monitoring and Maintenance
Data Logging: Web interface activity log with export capability
Remote Monitoring: Web interface accessible from local network
System Status: Real-time temperature, power output, and safety status
Schedule Tracking: Progress monitoring through multi-segment firing cycles
Key Success Factors
Pin Conflict Resolution: Display on HSPI, sensors on VSPI
Modern Web Architecture: Separated HTML/CSS/JS with REST API
Real-time Synchronization: Display and web interface mirror each other
Comprehensive Safety: Multiple protection layers for high-temperature operation
Scalable Design: I2C expander ready for additional features
This system provides a solid foundation for ceramic kiln control with professional-grade safety features and modern web-based monitoring capabilities.

