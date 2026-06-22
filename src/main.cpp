#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_MAX31865.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <TZ.h>
#include <EEPROM.h>

// ==============================================================================
// 1. CONFIGURATION & DEFINITIONS
// ==============================================================================

// --- WiFi & Discord ---
const char* WIFI_SSID = "WXPQZ_89875397";
const char* WIFI_PASSWORD = "46575654";
const char* discordWebhookURL = "https://discord.com/api/webhooks/1516479560051593257/1cDwgL91_j2XE7xFbDR0fujiFU3xh9qBnDYK-irpVRBVLJQked51Kz9ppBEWvYA34-Tr";

#define WIFI_RETRY_INTERVAL_MS 1800000UL 
#define WIFI_MAX_BLOCK_MS      60000UL   
#define DISCORD_INTERVAL_MS    60000UL   
#define NTP_MAX_BLOCK_MS       30000UL   

// --- Pin Definitions ---
#define SENSOR1_CS D0
#define SENSOR2_CS D4
#define AC_SENSOR D1
#define ANALOG_PIN A0 
#define RELAY_OUTPUT D2

// --- RTD Sensor Settings ---
#define RREF 430.0
#define RNOMINAL 100.0

// --- Operating Limits ---
#define MAX_COMPRESSOR_TEMP 200.0
#define MAX_FAN_TEMP 200.0
#define MIN_FAN_CURRENT 200.0

// --- Timing Constants ---
#define SENSOR_POLL_MS 2000 
#define SYSTEM_STARTUP_STABILIZATION_MS 30000 
#define SYSTEM_SHUTDOWN_STABILIZATION_MS 180000  // Anti-short-cycle
#define SYSTEM_OVERHEAT_PROTECTION_MS 900000 
#define SYSTEM_FAN_THERMAL_PROTECTION_MS 900000 
#define DEBOUNCE_DELAY_MS 50

// --- EEPROM Addresses ---
#define EEPROM_SIZE 512
#define ADDR_RUNTIME 0       // uint32_t (4 bytes)
#define ADDR_LAST_FAULT 4    // uint8_t (1 byte)

// ==============================================================================
// 2. GLOBALS & STATE ENUMS
// ==============================================================================

enum HvacState {
  STATE_IDLE = 0,
  STATE_STARTING = 1,
  STATE_RUNNING = 2,
  STATE_COOLDOWN = 3,
  STATE_OVERHEAT = 4,
  STATE_FAN_FAULT = 5,
  STATE_SENSOR_FAULT = 6
};

HvacState currentState = STATE_IDLE;
unsigned long stateStartTime = 0;

// Hardware Objects
Adafruit_MAX31865 rtdSensor1 = Adafruit_MAX31865(SENSOR1_CS);
Adafruit_MAX31865 rtdSensor2 = Adafruit_MAX31865(SENSOR2_CS);
WiFiClientSecure secured_client;
HTTPClient http;

// Network State
unsigned long lastWiFiAttempt = 0;
unsigned long lastDiscordSend = 0;
bool WIFI_READY = false;
bool NTP_SYNCED = false;

// Sensor Data
double COMPRESSOR_TEMP = 0.0;
double FAN_TEMP = 0.0;
double FAN_CURRENT = 0.0;
bool INPUT_SIGNAL = false;

// Debounce Tracking
int lastAcPinState = LOW;
unsigned long lastDebounceTime = 0;

// Runtime Tracking
uint32_t totalCompressorRuntimeSec = 0;
unsigned long compressorTurnOnTime = 0;

// Communication Queue Flags
String pendingDiscordMessage = "";
bool forceDiscordSend = false;

// ==============================================================================
// 3. UTILITY & EEPROM FUNCTIONS
// ==============================================================================

String stateToString(HvacState state) {
  switch(state) {
    case STATE_IDLE: return "IDLE";
    case STATE_STARTING: return "STARTING";
    case STATE_RUNNING: return "RUNNING";
    case STATE_COOLDOWN: return "COOLDOWN";
    case STATE_OVERHEAT: return "OVERHEAT";
    case STATE_FAN_FAULT: return "FAN_FAULT";
    case STATE_SENSOR_FAULT: return "SENSOR_FAULT";
    default: return "UNKNOWN";
  }
}

void loadEEPROMData() {
  EEPROM.get(ADDR_RUNTIME, totalCompressorRuntimeSec);
  if (totalCompressorRuntimeSec == 0xFFFFFFFF) { // Uninitialized EEPROM
    totalCompressorRuntimeSec = 0;
    EEPROM.put(ADDR_RUNTIME, totalCompressorRuntimeSec);
    EEPROM.commit();
  }
  Serial.print("Total Logged Compressor Runtime (s): ");
  Serial.println(totalCompressorRuntimeSec);
}

void saveRuntime() {
  EEPROM.put(ADDR_RUNTIME, totalCompressorRuntimeSec);
  EEPROM.commit();
}

void logFault(HvacState faultState) {
  EEPROM.put(ADDR_LAST_FAULT, (uint8_t)faultState);
  EEPROM.commit();
}

// Queue a message for the non-blocking Comm Task
void queueMessage(String msg, bool force = false) {
  pendingDiscordMessage = msg;
  forceDiscordSend = force;
  Serial.println("[QUEUE]: " + msg);
}

void changeState(HvacState newState, String reason) {
  HvacState oldState = currentState;
  currentState = newState;
  stateStartTime = millis();
  
  String transitionMsg = "[STATE] " + stateToString(oldState) + " -> " + stateToString(newState) + " | " + reason;
  queueMessage(transitionMsg, true);

  // Runtime Tracking Logic
  if (newState == STATE_RUNNING) {
    compressorTurnOnTime = millis();
  } 
  else if (oldState == STATE_RUNNING) {
    uint32_t sessionSeconds = (millis() - compressorTurnOnTime) / 1000;
    totalCompressorRuntimeSec += sessionSeconds;
    saveRuntime();
    queueMessage("Session logged. Total Runtime: " + String(totalCompressorRuntimeSec) + "s", false);
  }

  // Fault Logging Logic
  if (newState == STATE_OVERHEAT || newState == STATE_FAN_FAULT || newState == STATE_SENSOR_FAULT) {
    logFault(newState);
  }
}

// ==============================================================================
// 4. SENSOR & HARDWARE TASKS
// ==============================================================================

double calculateRMSCurrent() {
  // True RMS Calculation
  const int numSamples = 500;
  unsigned long sumSquares = 0;
  int dcOffset = 512; // Assuming 10-bit ADC centered at VCC/2. Adjust if needed.
  
  for (int i = 0; i < numSamples; i++) {
    int sample = analogRead(ANALOG_PIN) - dcOffset;
    sumSquares += (sample * sample);
    delayMicroseconds(100); // Small delay to spread samples over AC wave
  }
  
  double meanSquare = (double)sumSquares / numSamples;
  double rmsRaw = sqrt(meanSquare);
  
  // Convert RAW RMS to Amps (Requires calibration multiplier)
  // For standard ACS712-30A it's ~66mV per Amp. 
  double calibratedAmps = rmsRaw * 0.074; // Replace 0.074 with your exact calibration factor
  return calibratedAmps;
}

void readSensorsTask() {
  static unsigned long lastSensorRead = 0;
  if (millis() - lastSensorRead < SENSOR_POLL_MS) return;
  lastSensorRead = millis();

  // Read Temperatures
  COMPRESSOR_TEMP = rtdSensor1.temperature(RNOMINAL, RREF); 
  FAN_TEMP = rtdSensor2.temperature(RNOMINAL, RREF);

  // Check for MAX31865 faults
  uint8_t fault1 = rtdSensor1.readFault();
  uint8_t fault2 = rtdSensor2.readFault();
  if (fault1) { rtdSensor1.clearFault(); COMPRESSOR_TEMP = 999.0; } // 999 acts as fail-safe trigger
  if (fault2) { rtdSensor2.clearFault(); FAN_TEMP = 999.0; }

  // Read RMS Current
  FAN_CURRENT = calculateRMSCurrent();
}

void debounceInputTask() {
  int reading = digitalRead(AC_SENSOR);
  if (reading != lastAcPinState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY_MS) {
    bool newState = (reading == HIGH);
    if (newState != INPUT_SIGNAL) {
      INPUT_SIGNAL = newState;
    }
  }
  lastAcPinState = reading;
}

// ==============================================================================
// 5. CONTROL TASK (STATE MACHINE)
// ==============================================================================

void controlTask() {
  unsigned long timeInState = millis() - stateStartTime;

  // Global Safety Interlock: Sensor Failure overrides everything
  if ((COMPRESSOR_TEMP == 999.0 || FAN_TEMP == 999.0) && currentState != STATE_SENSOR_FAULT) {
    changeState(STATE_SENSOR_FAULT, "Critical: RTD Sensor Disconnected or Faulted!");
    return;
  }

  switch (currentState) {
    case STATE_IDLE:
      digitalWrite(RELAY_OUTPUT, LOW);
      if (INPUT_SIGNAL) {
        if (COMPRESSOR_TEMP <= MAX_COMPRESSOR_TEMP && FAN_TEMP <= MAX_FAN_TEMP) {
          changeState(STATE_STARTING, "Thermostat call for cooling.");
        } else {
          changeState(STATE_OVERHEAT, "Thermostat call, but temps too high.");
        }
      }
      break;

    case STATE_STARTING:
      digitalWrite(RELAY_OUTPUT, HIGH);
      if (timeInState >= SYSTEM_STARTUP_STABILIZATION_MS) {
        changeState(STATE_RUNNING, "Startup stabilization complete.");
      }
      if (!INPUT_SIGNAL) {
        changeState(STATE_COOLDOWN, "Input OFF during startup.");
      }
      break;

    case STATE_RUNNING:
      digitalWrite(RELAY_OUTPUT, HIGH);
      if (!INPUT_SIGNAL) {
        changeState(STATE_COOLDOWN, "Normal shutdown sequence.");
      }
      else if (COMPRESSOR_TEMP > MAX_COMPRESSOR_TEMP || FAN_TEMP > MAX_FAN_TEMP) {
        changeState(STATE_OVERHEAT, "Overheat limit exceeded!");
      }
      else if (FAN_CURRENT < MIN_FAN_CURRENT) {
        // changeState(STATE_FAN_FAULT, "Fan current dropped below minimum!");
        // Uncomment above once FAN_CURRENT math is perfectly calibrated
      }
      break;

    case STATE_COOLDOWN:
      digitalWrite(RELAY_OUTPUT, LOW);
      if (timeInState >= SYSTEM_SHUTDOWN_STABILIZATION_MS) {
        changeState(STATE_IDLE, "Cooldown complete.");
      }
      break;

    case STATE_OVERHEAT:
      digitalWrite(RELAY_OUTPUT, LOW);
      if (timeInState >= SYSTEM_OVERHEAT_PROTECTION_MS) {
        changeState(STATE_IDLE, "Overheat timeout cleared.");
      }
      break;

    case STATE_FAN_FAULT:
      digitalWrite(RELAY_OUTPUT, LOW);
      if (timeInState >= SYSTEM_FAN_THERMAL_PROTECTION_MS) {
        changeState(STATE_IDLE, "Fan fault timeout cleared.");
      }
      break;

    case STATE_SENSOR_FAULT:
      digitalWrite(RELAY_OUTPUT, LOW);
      // Can only exit this state if sensors read normally again
      if (COMPRESSOR_TEMP != 999.0 && FAN_TEMP != 999.0) {
         changeState(STATE_COOLDOWN, "Sensors restored. Entering cooldown before retry.");
      }
      break;
  }
}

// ==============================================================================
// 6. COMMUNICATION TASK
// ==============================================================================

String getTimeString() {
  if (!NTP_SYNCED) return "Time not synced";
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
  return String(buffer);
}

void executeDiscordSend() {
  if (!WIFI_READY || pendingDiscordMessage == "") return;

  if (!http.begin(secured_client, discordWebhookURL)) {
    Serial.println("Failed to setup HTTP Client for Discord.");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String payloadBody = pendingDiscordMessage + "\\n" +
    "TS: " + getTimeString() + "\\n" +
    "State: " + stateToString(currentState) + "\\n" +
    "CT: " + String(COMPRESSOR_TEMP, 1) + " | FT: " + String(FAN_TEMP, 1) + " | FC: " + String(FAN_CURRENT, 2) + "\\n" +
    "Runtime: " + String(totalCompressorRuntimeSec) + "s";

  String payload = "{\"content\":\"" + payloadBody + "\"}";

  int httpResponseCode = http.POST(payload);
  if (httpResponseCode > 0) {
    Serial.println("Discord Sent: HTTP " + String(httpResponseCode));
  } else {
    Serial.println("Discord Error: " + http.errorToString(httpResponseCode));
  }

  http.end();
  pendingDiscordMessage = ""; // Clear queue
  forceDiscordSend = false;
  lastDiscordSend = millis();
}

void communicationTask() {
  // Handle Periodic Status Update
  if (WIFI_READY && (millis() - lastDiscordSend >= DISCORD_INTERVAL_MS)) {
    if (pendingDiscordMessage == "") {
      queueMessage("Routine Status Update", false);
    }
  }

  // Execute Send if queued
  if (pendingDiscordMessage != "") {
    if (forceDiscordSend || (millis() - lastDiscordSend >= DISCORD_INTERVAL_MS)) {
       executeDiscordSend();
    }
  }
}

// --- WiFi Setup Functions (Unchanged Logic, Modularized) ---
bool syncNTP(unsigned long maxBlockMs) {
  configTime(TZ_Asia_Kolkata, "pool.ntp.org", "time.google.com", "time.nist.gov");
  unsigned long startTime = millis();
  while ((millis() - startTime) < maxBlockMs) {
    time_t now = time(nullptr);
    if (now > 1700000000) return true;
    delay(500);
  }
  return false;
}

void connectWiFiTask() {
  if (millis() - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS && lastWiFiAttempt != 0) return;
  if (WiFi.status() == WL_CONNECTED) return;

  lastWiFiAttempt = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_MAX_BLOCK_MS) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    WIFI_READY = true;
    secured_client.setInsecure();
    NTP_SYNCED = syncNTP(NTP_MAX_BLOCK_MS);
    Serial.println("\nWiFi Connected.");
    queueMessage("System Boot/WiFi Restored.", true);
  } else {
    WIFI_READY = false;
  }
}


// ==============================================================================
// 7. SETUP & MAIN LOOP
// ==============================================================================

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  delay(1000); 

  pinMode(AC_SENSOR, INPUT);
  pinMode(RELAY_OUTPUT, OUTPUT);
  digitalWrite(RELAY_OUTPUT, LOW);

  rtdSensor1.begin(MAX31865_2WIRE);
  rtdSensor2.begin(MAX31865_2WIRE);

  loadEEPROMData();
  
  // Initial WiFi attempt
  connectWiFiTask(); 
  
  changeState(STATE_IDLE, "System Boot Completed.");
}

void loop() {
  // The magic of a non-blocking architecture:
  // These tasks run as fast as the CPU allows.
  
  debounceInputTask();    // Rapidly checks AC pin
  readSensorsTask();      // Reads SPI and Analog on intervals
  controlTask();          // Evaluates State Machine logic
  communicationTask();    // Handles Discord HTTP POSTs
  
  // Optional: Background WiFi recovery if connection drops
  if (WiFi.status() != WL_CONNECTED) {
     WIFI_READY = false;
     connectWiFiTask();
  }
}