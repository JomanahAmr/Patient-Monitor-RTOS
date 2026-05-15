#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <string.h>

// =====================================================
// WiFi + API
// =====================================================

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* API_URL = "https://abood121212-vital-risk-api-new.hf.space";

// =====================================================
// Pin Mapping from Wokwi diagram
// =====================================================

#define LM35_PIN      4
#define NTC_RR_PIN    6
#define TOUCH_PIN     7
#define BUZZER_PIN    8

#define OLED_SDA      10
#define OLED_SCL      11

#define MAX_SDA       20
#define MAX_SCL       21
#define MAX30102_ADDR 0x22

// =====================================================
// OLED
// =====================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

TwoWire I2C_OLED = TwoWire(0);
TwoWire I2C_MAX  = TwoWire(1);

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &I2C_OLED, OLED_RESET);

// =====================================================
// RTOS Objects
// =====================================================

QueueHandle_t     resultQueue;       // [REQ: QUEUE]
SemaphoreHandle_t touchSemaphore;    // [REQ: SEMAPHORE] counting ISR events
SemaphoreHandle_t serialMutex;       // [REQ: MUTEX] Serial
SemaphoreHandle_t oledMutex;         // [REQ: MUTEX] OLED
SemaphoreHandle_t i2cMaxMutex;       // [REQ: MUTEX] I2C_MAX
SemaphoreHandle_t sensorDataMutex;   // [REQ: MUTEX] shared sensor data
SemaphoreHandle_t riskMutex;         // [REQ: MUTEX] latest risk result
SemaphoreHandle_t timingMutex;       // [REQ: MUTEX] TaskTiming structs
SemaphoreHandle_t raceMutex;         // [REQ: RACE CONDITION DEMO]
SemaphoreHandle_t prioLowMutex;      // [REQ: PRIORITY INVERSION DEMO]
SemaphoreHandle_t raceDoneSemaphore; // [REQ: RACE CONDITION DEMO]

// Task handles for stack high-water mark reporting
TaskHandle_t hAnalogTask=NULL, hCommTask=NULL, hDigitalTask=NULL;
TaskHandle_t hProcessingTask=NULL, hOutputTask=NULL, hLoggingTask=NULL;
TaskHandle_t hStressTask=NULL, hCpuLoadTask=NULL;
TaskHandle_t hRaceIncTask=NULL, hRaceDecTask=NULL;

// =====================================================
// Data Structures
// =====================================================

struct SensorData {
  float heartRate;
  float oxygenSaturation;
  float temperatureC;
  float temperatureF;
  float respiratoryRate;
  bool emergencyTouch;
};

struct RiskResult {
  SensorData vitals;
  char riskLabel[24];
  float riskProbability;
  bool apiOk;
};

// [REQ: TIMING MEASUREMENT] Per-task WCET, jitter, missed-deadline tracking
struct TaskTiming {
  uint32_t lastUs;
  uint32_t maxUs;          // WCET
  uint32_t maxJitterUs;    // worst jitter vs expected period
  uint32_t missedDeadlines;
  uint32_t prevWakeUs;     // used to compute actual period
};

// =====================================================
// Shared Data
// =====================================================

SensorData latestSensorData = {0.0, 0.0, 0.0, 0.0, 0.0, false};
RiskResult latestRiskResult;
bool wifiOk = false;

// [REQ: QUEUE] Dropped message counter
volatile uint32_t droppedMessageCount = 0;
volatile uint32_t queueOverwriteCount = 0;

// [REQ: SEMAPHORE WAKE-UP LATENCY]
volatile uint32_t isrTimestampUs  = 0;
volatile uint32_t wakeupLatencyUs = 0;
volatile uint32_t touchAcceptedCount = 0;
volatile uint32_t touchRejectedCount = 0;
volatile uint32_t touchLastAcceptedUs = 0;

// [REQ: FAULT INJECTION] Original fault flags (now volatile)
volatile bool injectAnalogFault = false;
volatile bool injectI2CFault    = false;
volatile bool injectApiFault    = false;
volatile bool stressMode        = false;

// [REQ: FAULT INJECTION] Extended fault flags
volatile bool injectAnalogOutOfRange    = false; // force invalid sensor values
volatile bool injectDigitalStuckPressed = false; // force emergencyTouch=true
volatile bool injectMax30102Failure     = false; // block I2C sensor read
volatile bool injectDelayedProcessing   = false; // add delay in ProcessingTask
volatile bool injectQueueFlood          = false; // flood queue to trigger drops
volatile bool injectMutexHoldTooLong    = false; // hold a shared-state mutex extra time
volatile bool injectCpuLoad             = false; // enable CpuLoadTask

// [REQ: RACE CONDITION DEMO]
volatile bool raceDemoUnsafe    = false;
volatile bool raceDemoWithMutex = false;
volatile bool raceUseMutex      = false;
volatile int  sharedCounter     = 0;

// [REQ: PRIORITY INVERSION DEMO]
volatile bool enablePriorityInversionDemo = false;

TaskTiming analogTiming     = {0, 0, 0, 0, 0};
TaskTiming commTiming       = {0, 0, 0, 0, 0};
TaskTiming digitalTiming    = {0, 0, 0, 0, 0};
TaskTiming processingTiming = {0, 0, 0, 0, 0};
TaskTiming outputTiming     = {0, 0, 0, 0, 0};
TaskTiming loggingTiming    = {0, 0, 0, 0, 0};

const uint32_t TOUCH_DEBOUNCE_US = 50000UL;

void safePrintln(const String& msg);
void safePrint(const String& msg);

bool readEmergencyTouchPressed() {
  return injectDigitalStuckPressed ? true : (digitalRead(TOUCH_PIN) == LOW);
}

bool enqueueLatestRiskResult(const RiskResult& result) {
  if (xQueueSend(resultQueue, &result, 0) == pdTRUE) {
    return true;
  }

  RiskResult discarded;
  bool removedOldest = (xQueueReceive(resultQueue, &discarded, 0) == pdTRUE);
  if (removedOldest) {
    droppedMessageCount++;
    queueOverwriteCount++;
  }

  if (xQueueSend(resultQueue, &result, 0) == pdTRUE) {
    if (removedOldest) {
      safePrintln("[QUEUE] Full - oldest result discarded to preserve the latest state");
    }
    return true;
  }

  droppedMessageCount++;
  safePrintln("[QUEUE] Full - failed to enqueue latest result");
  return false;
}

// =====================================================
// Safe Serial Functions
// =====================================================

void safePrintln(const String& msg) {
  if (serialMutex != NULL) {
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.println(msg);
      xSemaphoreGive(serialMutex);
    }
  } else {
    Serial.println(msg);
  }
}

void safePrint(const String& msg) {
  if (serialMutex != NULL) {
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.print(msg);
      xSemaphoreGive(serialMutex);
    }
  } else {
    Serial.print(msg);
  }
}

// =====================================================
// Diagnostics Helpers
// =====================================================

void updateTiming(TaskTiming* timing, uint32_t elapsedUs) {
  if (timingMutex != NULL && xSemaphoreTake(timingMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    timing->lastUs = elapsedUs;
    if (elapsedUs > timing->maxUs) timing->maxUs = elapsedUs;
    xSemaphoreGive(timingMutex);
  }
}

// [REQ: TIMING MEASUREMENT] Jitter + missed deadline for periodic tasks
void updatePeriodMetrics(TaskTiming* timing, uint32_t nowUs,
                         uint32_t expectedUs, uint32_t deadlineUs) {
  if (timingMutex != NULL && xSemaphoreTake(timingMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (timing->prevWakeUs != 0) {
      uint32_t actual = nowUs - timing->prevWakeUs;
      uint32_t jitter = (actual > expectedUs) ? (actual - expectedUs) : (expectedUs - actual);
      if (jitter > timing->maxJitterUs) timing->maxJitterUs = jitter;
      if (actual > deadlineUs) timing->missedDeadlines++;
    }
    timing->prevWakeUs = nowUs;
    xSemaphoreGive(timingMutex);
  }
}

TaskTiming readTimingSnapshot(TaskTiming* timing) {
  TaskTiming snapshot = {0, 0, 0, 0, 0};
  if (timingMutex != NULL && xSemaphoreTake(timingMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    snapshot = *timing;
    xSemaphoreGive(timingMutex);
  }
  return snapshot;
}

void printDiagnostics() {
  TaskTiming a = readTimingSnapshot(&analogTiming);
  TaskTiming c = readTimingSnapshot(&commTiming);
  TaskTiming d = readTimingSnapshot(&digitalTiming);
  TaskTiming p = readTimingSnapshot(&processingTiming);
  TaskTiming o = readTimingSnapshot(&outputTiming);
  TaskTiming l = readTimingSnapshot(&loggingTiming);

  safePrintln("=== Diagnostics ===");
  safePrintln("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED"));
  safePrintln("Queue occupancy: " + String(uxQueueMessagesWaiting(resultQueue)));
  safePrintln("Dropped messages: " + String(droppedMessageCount));
  safePrintln("Queue overwrites: " + String(queueOverwriteCount));
  safePrintln("Sem wakeup latency us: " + String(wakeupLatencyUs));
  safePrintln("Touch events accepted/rejected: " + String(touchAcceptedCount) + "/" + String(touchRejectedCount));
  safePrintln("[WCET us] A=" + String(a.maxUs) + " C=" + String(c.maxUs) +
              " D=" + String(d.maxUs) + " P=" + String(p.maxUs) +
              " O=" + String(o.maxUs) + " L=" + String(l.maxUs));
  safePrintln("[Jitter us] A=" + String(a.maxJitterUs) + " C=" + String(c.maxJitterUs) +
              " P=" + String(p.maxJitterUs) + " L=" + String(l.maxJitterUs));
  safePrintln("[MissedDL] A=" + String(a.missedDeadlines) + " C=" + String(c.missedDeadlines) +
              " P=" + String(p.missedDeadlines) + " L=" + String(l.missedDeadlines));
  safePrintln("[Faults] analog=" + String(injectAnalogFault) + " i2c=" + String(injectI2CFault) +
              " api=" + String(injectApiFault) + " oor=" + String(injectAnalogOutOfRange) +
              " stuck=" + String(injectDigitalStuckPressed) + " maxFail=" + String(injectMax30102Failure));
  safePrintln("[Faults2] delayProc=" + String(injectDelayedProcessing) +
              " qFlood=" + String(injectQueueFlood) + " mutexLong=" + String(injectMutexHoldTooLong) +
              " cpuLoad=" + String(injectCpuLoad) + " stress=" + String(stressMode));
  safePrintln("[StackHWM words] A=" + String(hAnalogTask ? uxTaskGetStackHighWaterMark(hAnalogTask) : 0) +
              " C=" + String(hCommTask ? uxTaskGetStackHighWaterMark(hCommTask) : 0) +
              " P=" + String(hProcessingTask ? uxTaskGetStackHighWaterMark(hProcessingTask) : 0) +
              " O=" + String(hOutputTask ? uxTaskGetStackHighWaterMark(hOutputTask) : 0));
}

void printCommandHelp() {
  safePrintln("Commands: help | diag | stress on|off");
  safePrintln(" fault analog|i2c|api|outofrange|stuck|maxfail|delayproc|qflood|mutexlong|cpuload on|off");
  safePrintln(" race unsafe | race mutex");
  safePrintln(" prio inversion on|off  (shows FreeRTOS mutex inheritance)");
}

void handleCommand(String cmd) {
  cmd.trim(); cmd.toLowerCase();
  if (cmd.length() == 0) return;
  if (cmd == "help") { printCommandHelp(); return; }
  if (cmd == "diag") { printDiagnostics(); return; }
  if (cmd == "fault analog on")      { injectAnalogFault=true;           safePrintln("[CMD] Analog fault ON"); return; }
  if (cmd == "fault analog off")     { injectAnalogFault=false;          safePrintln("[CMD] Analog fault OFF"); return; }
  if (cmd == "fault i2c on")         { injectI2CFault=true;              safePrintln("[CMD] I2C fault ON"); return; }
  if (cmd == "fault i2c off")        { injectI2CFault=false;             safePrintln("[CMD] I2C fault OFF"); return; }
  if (cmd == "fault api on")         { injectApiFault=true;              safePrintln("[CMD] API fault ON"); return; }
  if (cmd == "fault api off")        { injectApiFault=false;             safePrintln("[CMD] API fault OFF"); return; }
  if (cmd == "stress on")            { stressMode=true;                  safePrintln("[CMD] Stress ON"); return; }
  if (cmd == "stress off")           { stressMode=false;                 safePrintln("[CMD] Stress OFF"); return; }
  if (cmd == "fault outofrange on")  { injectAnalogOutOfRange=true;     safePrintln("[CMD] OOR fault ON"); return; }
  if (cmd == "fault outofrange off") { injectAnalogOutOfRange=false;    safePrintln("[CMD] OOR fault OFF"); return; }
  if (cmd == "fault stuck on")       { injectDigitalStuckPressed=true;  safePrintln("[CMD] Stuck ON"); return; }
  if (cmd == "fault stuck off")      { injectDigitalStuckPressed=false; safePrintln("[CMD] Stuck OFF"); return; }
  if (cmd == "fault maxfail on")     { injectMax30102Failure=true;      safePrintln("[CMD] MAX fail ON"); return; }
  if (cmd == "fault maxfail off")    { injectMax30102Failure=false;     safePrintln("[CMD] MAX fail OFF"); return; }
  if (cmd == "fault delayproc on")   { injectDelayedProcessing=true;    safePrintln("[CMD] DelayProc ON"); return; }
  if (cmd == "fault delayproc off")  { injectDelayedProcessing=false;   safePrintln("[CMD] DelayProc OFF"); return; }
  if (cmd == "fault qflood on")      { injectQueueFlood=true;           safePrintln("[CMD] QFlood ON"); return; }
  if (cmd == "fault qflood off")     { injectQueueFlood=false;          safePrintln("[CMD] QFlood OFF"); return; }
  if (cmd == "fault mutexlong on")   { injectMutexHoldTooLong=true;     safePrintln("[CMD] MutexLong ON"); return; }
  if (cmd == "fault mutexlong off")  { injectMutexHoldTooLong=false;    safePrintln("[CMD] MutexLong OFF"); return; }
  if (cmd == "fault cpuload on")     { injectCpuLoad=true;              safePrintln("[CMD] CPULoad ON"); return; }
  if (cmd == "fault cpuload off")    { injectCpuLoad=false;             safePrintln("[CMD] CPULoad OFF"); return; }
  if (cmd == "race unsafe")  { raceDemoUnsafe=true;  raceDemoWithMutex=false; safePrintln("[CMD] Race unsafe"); return; }
  if (cmd == "race mutex")   { raceDemoWithMutex=true; raceDemoUnsafe=false;  safePrintln("[CMD] Race mutex"); return; }
  if (cmd == "prio inversion on")  { enablePriorityInversionDemo=true;  safePrintln("[CMD] PrioInv ON"); return; }
  if (cmd == "prio inversion off") { enablePriorityInversionDemo=false; safePrintln("[CMD] PrioInv OFF"); return; }
  safePrintln("[CMD] Unknown. Type 'help'.");
}

// =====================================================
// Conversion Functions
// =====================================================

float celsiusToFahrenheit(float tempC) {
  return (tempC * 9.0 / 5.0) + 32.0;
}

// [REQ: ANALOG INPUT] LM35 temperature via ADC
float readLm35TemperatureC() {
  if (injectAnalogFault) return 0.0;
  // [REQ: FAULT INJECTION] out-of-range analog value
  if (injectAnalogOutOfRange) {
    safePrintln("[FAULT] Analog OOR: temp forced 0.0");
    return 0.0;
  }
  int   raw     = analogRead(LM35_PIN);
  // Custom chip outputs (temp_c/100)*3.3 V.
  // Wokwi ADC_11db full-scale ≈ 4.95 V (not 3.3 V).
  // Correction factor = 4.95/3.3 = 1.5
  // tempC = raw * (4.95/3.3) * 100 / 4095 = raw * 150 / 4095
  float tempC   = raw * 150.0 / 4095.0;
  return tempC;
}

// [REQ: ANALOG INPUT] NTC respiratory-rate simulation via ADC
float readRespiratoryRate() {
  if (injectAnalogFault) return 0.0;
  // [REQ: FAULT INJECTION] out-of-range analog value
  if (injectAnalogOutOfRange) {
    safePrintln("[FAULT] Analog OOR: RR forced 0.0");
    return 0.0;
  }

  int raw = analogRead(NTC_RR_PIN);

  if (raw <= 0) {
    raw = 1;
  }

  if (raw >= 4095) {
    raw = 4094;
  }

  float beta = 3950.0;
  float r0 = 10000.0;
  float t0 = 298.15;

  float resistance = r0 * raw / (4095.0 - raw);
  float tempK = 1.0 / ((1.0 / t0) + (1.0 / beta) * log(resistance / r0));
  float tempC = tempK - 273.15;

  // Simulation mapping from NTC temperature to respiratory rate
  float rr = (tempC - 34.0) * (30.0 - 12.0) / (41.0 - 34.0) + 12.0;

  if (rr < 8) {
    rr = 8;
  }

  if (rr > 35) {
    rr = 35;
  }

  return rr;
}

// =====================================================
// MAX30102 Custom Chip Read
//
// Expected chip register map:
// Register 0 = Heart Rate BPM
// Register 1 = SpO2 %
//
// ESP32 reads:
// write 0, read 1 byte -> HR
// write 1, read 1 byte -> SpO2
// =====================================================

// [REQ: COMMUNICATION SENSOR] MAX30102 custom I2C chip
bool readMax30102Vitals(float* heartRate, float* spo2) {
  // [REQ: FAULT INJECTION] disconnected / failed sensor
  if (injectI2CFault || injectMax30102Failure) {
    safePrintln("[I2C] Fault: MAX30102 read blocked");
    return false;
  }

  uint8_t hrValue = 0;
  uint8_t spo2Value = 0;

  if (xSemaphoreTake(i2cMaxMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

    // -----------------------------
    // Read Heart Rate from register 0
    // -----------------------------
    I2C_MAX.beginTransmission(MAX30102_ADDR);
    I2C_MAX.write(0);

    if (I2C_MAX.endTransmission() != 0) {
      xSemaphoreGive(i2cMaxMutex);
      safePrintln("[I2C] Failed to select HR register");
      return false;
    }

    I2C_MAX.requestFrom(MAX30102_ADDR, 1);

    if (I2C_MAX.available() >= 1) {
      hrValue = I2C_MAX.read();
    } else {
      xSemaphoreGive(i2cMaxMutex);
      safePrintln("[I2C] Failed to read HR");
      return false;
    }

    // -----------------------------
    // Read SpO2 from register 1
    // -----------------------------
    I2C_MAX.beginTransmission(MAX30102_ADDR);
    I2C_MAX.write(1);

    if (I2C_MAX.endTransmission() != 0) {
      xSemaphoreGive(i2cMaxMutex);
      safePrintln("[I2C] Failed to select SpO2 register");
      return false;
    }

    I2C_MAX.requestFrom(MAX30102_ADDR, 1);

    if (I2C_MAX.available() >= 1) {
      spo2Value = I2C_MAX.read();
    } else {
      xSemaphoreGive(i2cMaxMutex);
      safePrintln("[I2C] Failed to read SpO2");
      return false;
    }

    xSemaphoreGive(i2cMaxMutex);

    *heartRate = (float)hrValue;
    *spo2 = (float)spo2Value;

    safePrint("[I2C] HR=");
    safePrint(String(*heartRate));
    safePrint(" SpO2=");
    safePrintln(String(*spo2));

    return true;
  }

  safePrintln("[I2C] MAX30102 mutex timeout");
  return false;
}

// =====================================================
// WiFi
// =====================================================

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  safePrintln("Connecting to WiFi...");

  int timeout = 30;

  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    safePrintln("Waiting WiFi...");
    delay(1000);
    timeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    safePrint("WiFi connected. IP: ");
    safePrintln(WiFi.localIP().toString());
    return true;
  }

  safePrintln("WiFi connection failed");
  return false;
}

// =====================================================
// Local Fallback Decision
// =====================================================

RiskResult localFallbackDecision(SensorData data) {
  RiskResult result;
  result.vitals = data;
  result.apiOk = false;

  if (
    data.emergencyTouch ||
    data.heartRate < 60 || data.heartRate > 100 ||
    data.oxygenSaturation < 95 ||
    data.temperatureC < 35.5 || data.temperatureC > 37.5 ||
    data.respiratoryRate < 12 || data.respiratoryRate > 22
  ) {
    strcpy(result.riskLabel, "CRITICAL_RISK");
    result.riskProbability = 0.90;
  } else {
    strcpy(result.riskLabel, "LOW_RISK");
    result.riskProbability = 0.10;
  }

  if (data.emergencyTouch) {
    strcpy(result.riskLabel, "CRITICAL_RISK");
    result.riskProbability = 1.0;
  }

  return result;
}

// =====================================================
// API Call
// =====================================================

RiskResult sendVitalsToApi(SensorData data) {
  RiskResult result;
  result.vitals = data;
  result.apiOk = false;
  strcpy(result.riskLabel, "API_ERROR");
  result.riskProbability = 0.0;

  if (injectApiFault) {
    safePrintln("[API] Injected API fault");
    return result;
  }

  if (WiFi.status() != WL_CONNECTED) {
    safePrintln("[API] WiFi not connected");
    return result;
  }

  StaticJsonDocument<256> payloadDoc;

  payloadDoc["heart_rate"] = data.heartRate;
  payloadDoc["oxygen_saturation"] = data.oxygenSaturation;
  payloadDoc["temperature"] = data.temperatureF;
  payloadDoc["respiratory_rate"] = data.respiratoryRate;

  String payload;
  serializeJson(payloadDoc, payload);

  safePrintln("[API] Sending payload:");
  safePrintln(payload);

  HTTPClient http;
  http.begin(API_URL);
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");

  int statusCode = http.POST(payload);
  String responseText = http.getString();

  safePrint("[API] HTTP Status: ");
  safePrintln(String(statusCode));

  safePrintln("[API] Response:");
  safePrintln(responseText);

  if (statusCode != 200) {
    http.end();
    return result;
  }

  StaticJsonDocument<512> responseDoc;
  DeserializationError error = deserializeJson(responseDoc, responseText);

  if (error) {
    safePrintln("[API] JSON parse error");
    http.end();
    return result;
  }

  const char* risk = responseDoc["risk_label"] | "UNKNOWN";
  float probability = responseDoc["risk_probability"] | 0.0;

  strncpy(result.riskLabel, risk, sizeof(result.riskLabel) - 1);
  result.riskLabel[sizeof(result.riskLabel) - 1] = '\0';

  result.riskProbability = probability;
  result.apiOk = true;

  http.end();
  return result;
}

// =====================================================
// Touch Button ISR
// =====================================================

// [REQ: ISR] Touch interrupt — records timestamp for semaphore wake-up latency measurement
void IRAM_ATTR touchISR() {
  uint32_t nowUs = micros();
  if ((nowUs - touchLastAcceptedUs) < TOUCH_DEBOUNCE_US) {
    touchRejectedCount++;
    return;
  }

  touchLastAcceptedUs = nowUs;
  isrTimestampUs = nowUs; // [REQ: TIMING MEASUREMENT]
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  if (xSemaphoreGiveFromISR(touchSemaphore, &higherPriorityTaskWoken) != pdTRUE) {
    touchRejectedCount++;
    return;
  }
  touchAcceptedCount++;
  if (higherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// =====================================================
// RTOS Tasks
// =====================================================

// [REQ: ANALOG INPUT] [REQ: TIMING MEASUREMENT]
void AnalogSensorTask(void* parameter) {
  const TickType_t period = pdMS_TO_TICKS(1000);
  TickType_t lastWakeTime = xTaskGetTickCount();
  const uint32_t expectedUs = 1000000UL;

  while (true) {
    uint32_t startUs = micros();
    updatePeriodMetrics(&analogTiming, startUs, expectedUs, expectedUs + 50000UL);

    float tempC = readLm35TemperatureC();
    float tempF = celsiusToFahrenheit(tempC);
    float rr    = readRespiratoryRate();

    if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      latestSensorData.temperatureC    = tempC;
      latestSensorData.temperatureF    = tempF;
      latestSensorData.respiratoryRate = rr;
      xSemaphoreGive(sensorDataMutex);
    } else {
      safePrintln("[ANALOG] Sensor data mutex timeout");
    }

    updateTiming(&analogTiming, micros() - startUs);
    vTaskDelayUntil(&lastWakeTime, period);
  }
}

// [REQ: COMMUNICATION SENSOR] [REQ: TIMING MEASUREMENT]
void CommunicationSensorTask(void* parameter) {
  const TickType_t period = pdMS_TO_TICKS(1000);
  TickType_t lastWakeTime = xTaskGetTickCount();
  const uint32_t expectedUs = 1000000UL;

  while (true) {
    uint32_t startUs = micros();
    updatePeriodMetrics(&commTiming, startUs, expectedUs, expectedUs + 50000UL);

    float bpm  = 0.0;
    float spo2 = 0.0;
    bool  ok   = readMax30102Vitals(&bpm, &spo2);

    if (ok) {
      if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latestSensorData.heartRate        = bpm;
        latestSensorData.oxygenSaturation = spo2;
        xSemaphoreGive(sensorDataMutex);
      } else {
        safePrintln("[COMM] Sensor data mutex timeout");
      }
    } else {
      safePrintln("[COMM] MAX30102 data not updated");
    }

    updateTiming(&commTiming, micros() - startUs);
    vTaskDelayUntil(&lastWakeTime, period);
  }
}

// [REQ: DIGITAL INPUT] [REQ: SEMAPHORE] [REQ: FAULT INJECTION]
void DigitalInputTask(void* parameter) {
  bool lastReportedState = readEmergencyTouchPressed();

  while (true) {
    uint32_t startUs = micros();
    bool hadEvent = false;

    // Short timeout keeps the digital state fresh while still preferring ISR wake-ups.
    if (xSemaphoreTake(touchSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
      hadEvent = true;
      // [REQ: TIMING MEASUREMENT] semaphore wake-up latency
      wakeupLatencyUs = micros() - isrTimestampUs;
    }

    bool pressed = readEmergencyTouchPressed();
    if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      latestSensorData.emergencyTouch = pressed;
      xSemaphoreGive(sensorDataMutex);
    } else {
      safePrintln("[DIGITAL] Sensor data mutex timeout");
    }

    if (hadEvent || pressed != lastReportedState) {
      safePrintln(pressed ? "[DIGITAL] Emergency touch pressed" : "[DIGITAL] Emergency touch released");
      lastReportedState = pressed;
    }

    updateTiming(&digitalTiming, micros() - startUs);
  }
}

// [REQ: QUEUE] [REQ: MUTEX] [REQ: TIMING MEASUREMENT] [REQ: FAULT INJECTION]
void ProcessingTask(void* parameter) {
  const TickType_t period = pdMS_TO_TICKS(3000);
  TickType_t lastWakeTime = xTaskGetTickCount();
  const uint32_t expectedUs = 3000000UL;

  while (true) {
    uint32_t startUs = micros();
    updatePeriodMetrics(&processingTiming, startUs, expectedUs, expectedUs + 100000UL);

    // [REQ: FAULT INJECTION] delayed processing simulation
    if (injectDelayedProcessing) {
      safePrintln("[FAULT] DelayedProcessing: blocking 2000 ms");
      vTaskDelay(pdMS_TO_TICKS(2000));
    }

    SensorData snapshot;
    if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      snapshot = latestSensorData;
      xSemaphoreGive(sensorDataMutex);
    } else {
      safePrintln("[PROCESSING] Sensor data mutex timeout");
      vTaskDelayUntil(&lastWakeTime, period);
      continue;
    }

    RiskResult result;
    if (wifiOk && WiFi.status() == WL_CONNECTED) {
      result = sendVitalsToApi(snapshot);
      if (!result.apiOk) result = localFallbackDecision(snapshot);
    } else {
      result = localFallbackDecision(snapshot);
    }

    if (snapshot.emergencyTouch) {
      strcpy(result.riskLabel, "CRITICAL_RISK");
      result.riskProbability = 1.0;
      result.apiOk = false;
    }

    // [REQ: QUEUE] keep the newest state available to the output pipeline.
    enqueueLatestRiskResult(result);
    safePrintln("[QUEUE] Occupancy: " + String(uxQueueMessagesWaiting(resultQueue)));

    // [REQ: FAULT INJECTION] queue flood: send extra copies to fill queue
    if (injectQueueFlood) {
      for (int i = 0; i < 6; i++) {
        enqueueLatestRiskResult(result);
      }
      safePrintln("[FAULT] QueueFlood done. Dropped/overwritten: " + String(droppedMessageCount) + "/" + String(queueOverwriteCount));
    }

    // [REQ: MUTEX] protect latestRiskResult shared variable
    if (xSemaphoreTake(riskMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      latestRiskResult = result;
      // [REQ: FAULT INJECTION] mutex held too long simulation
      if (injectMutexHoldTooLong) {
        safePrintln("[FAULT] MutexHoldTooLong: holding riskMutex 500 ms");
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      xSemaphoreGive(riskMutex);
    } else {
      safePrintln("[PROCESSING] Risk mutex timeout");
    }

    updateTiming(&processingTiming, micros() - startUs);
    vTaskDelayUntil(&lastWakeTime, period);
  }
}

void OutputTask(void* parameter) {
  RiskResult result;

  while (true) {
    uint32_t startUs = micros();

    if (xQueueReceive(resultQueue, &result, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (xSemaphoreTake(oledMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);

        oled.setCursor(0, 0);
        oled.print("HR:");
        oled.print((int)result.vitals.heartRate);
        oled.print(" SpO2:");
        oled.print((int)result.vitals.oxygenSaturation);
        oled.print("%");

        oled.setCursor(0, 10);
        oled.print("Temp:");
        oled.print(result.vitals.temperatureC, 1);
        oled.print("C");

        oled.setCursor(0, 20);
        oled.print("RR:");
        oled.print((int)result.vitals.respiratoryRate);
        oled.print(" rpm");

        oled.setCursor(0, 30);
        oled.print("Touch:");
        oled.print(result.vitals.emergencyTouch ? "YES" : "NO");

        oled.setCursor(0, 42);

        if (String(result.riskLabel) == "CRITICAL_RISK") {
          oled.print("RISK: CRITICAL");
        } else if (String(result.riskLabel) == "LOW_RISK") {
          oled.print("RISK: LOW");
        } else {
          oled.print("RISK: UNKNOWN");
        }

        oled.setCursor(0, 54);

        if (result.apiOk) {
          oled.print("API OK P:");
        } else {
          oled.print("LOCAL P:");
        }

        oled.print(result.riskProbability, 2);

        oled.display();

        xSemaphoreGive(oledMutex);
      } else {
        safePrintln("[OUTPUT] OLED mutex timeout");
      }

      if (String(result.riskLabel) == "CRITICAL_RISK" || result.vitals.emergencyTouch) {
        tone(BUZZER_PIN, 1800);
      } else {
        noTone(BUZZER_PIN);
      }
    }

    updateTiming(&outputTiming, micros() - startUs);
  }
}

// [REQ: TIMING MEASUREMENT] [REQ: QUEUE] [REQ: MUTEX] Expanded diagnostic logging
void LoggingTask(void* parameter) {
  const TickType_t period = pdMS_TO_TICKS(3000);
  TickType_t lastWakeTime = xTaskGetTickCount();
  const uint32_t expectedUs = 3000000UL;

  while (true) {
    uint32_t startUs = micros();
    updatePeriodMetrics(&loggingTiming, startUs, expectedUs, expectedUs + 100000UL);

    TaskTiming  aSnap = readTimingSnapshot(&analogTiming);
    TaskTiming  cSnap = readTimingSnapshot(&commTiming);
    TaskTiming  pSnap = readTimingSnapshot(&processingTiming);
    TaskTiming  lSnap = readTimingSnapshot(&loggingTiming);

    SensorData data = {0.0, 0.0, 0.0, 0.0, 0.0, false};
    RiskResult  risk = {};
    strcpy(risk.riskLabel, "UNAVAILABLE");

    if (xSemaphoreTake(sensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      data = latestSensorData;
      xSemaphoreGive(sensorDataMutex);
    } else {
      safePrintln("[LOG] Sensor data mutex timeout");
    }

    if (xSemaphoreTake(riskMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      risk = latestRiskResult;
      xSemaphoreGive(riskMutex);
    } else {
      safePrintln("[LOG] Risk mutex timeout");
    }

    StaticJsonDocument<1024> doc;
    doc["heart_rate"]            = data.heartRate;
    doc["oxygen_saturation"]     = data.oxygenSaturation;
    doc["temperature_celsius"]   = data.temperatureC;
    doc["temperature_fahrenheit"]= data.temperatureF;
    doc["respiratory_rate"]      = data.respiratoryRate;
    doc["emergency_touch"]       = data.emergencyTouch;
    doc["risk_label"]            = risk.riskLabel;
    doc["risk_probability"]      = risk.riskProbability;
    doc["wifi"]                  = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
    // [REQ: QUEUE] queue metrics
    doc["queue_occupancy"]       = uxQueueMessagesWaiting(resultQueue);
    doc["dropped_messages"]      = droppedMessageCount;
    doc["queue_overwrites"]      = queueOverwriteCount;
    // [REQ: TIMING MEASUREMENT] WCET
    doc["wcet_analog_us"]        = aSnap.maxUs;
    doc["wcet_comm_us"]          = cSnap.maxUs;
    doc["wcet_proc_us"]          = pSnap.maxUs;
    doc["wcet_log_us"]           = lSnap.maxUs;
    // [REQ: TIMING MEASUREMENT] jitter
    doc["jitter_analog_us"]      = aSnap.maxJitterUs;
    doc["jitter_comm_us"]        = cSnap.maxJitterUs;
    doc["jitter_proc_us"]        = pSnap.maxJitterUs;
    doc["jitter_log_us"]         = lSnap.maxJitterUs;
    // [REQ: TIMING MEASUREMENT] missed deadlines
    doc["missed_dl_analog"]      = aSnap.missedDeadlines;
    doc["missed_dl_comm"]        = cSnap.missedDeadlines;
    doc["missed_dl_proc"]        = pSnap.missedDeadlines;
    doc["missed_dl_log"]         = lSnap.missedDeadlines;
    // [REQ: SEMAPHORE WAKE-UP LATENCY]
    doc["sem_wakeup_latency_us"] = wakeupLatencyUs;
    doc["touch_events_accepted"] = touchAcceptedCount;
    doc["touch_events_rejected"] = touchRejectedCount;
    // [REQ: FAULT INJECTION] fault status
    doc["fault_analog"]          = injectAnalogFault;
    doc["fault_i2c"]             = injectI2CFault;
    doc["fault_api"]             = injectApiFault;
    doc["fault_oor"]             = injectAnalogOutOfRange;
    doc["fault_stuck"]           = injectDigitalStuckPressed;
    doc["fault_maxfail"]         = injectMax30102Failure;
    doc["fault_delayproc"]       = injectDelayedProcessing;
    doc["fault_qflood"]          = injectQueueFlood;
    doc["fault_mutexlong"]       = injectMutexHoldTooLong;
    doc["fault_cpuload"]         = injectCpuLoad;
    doc["stress_mode"]           = stressMode;

    String logLine;
    serializeJson(doc, logLine);
    safePrintln("[LOG] " + logLine);

    updateTiming(&loggingTiming, micros() - startUs);
    vTaskDelayUntil(&lastWakeTime, period);
  }
}

void CommandTask(void* parameter) {
  String line;

  while (true) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();

      if (c == '\r') {
        continue;
      }

      if (c == '\n') {
        handleCommand(line);
        line = "";
      } else if (line.length() < 80) {
        line += c;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void StressTask(void* parameter) {
  while (true) {
    if (stressMode) {
      uint32_t startMs = millis();
      while ((millis() - startMs) < 150) {
        volatile uint32_t sink = micros();
        (void)sink;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// [REQ: CPU LOAD / STRESS TEST] CpuLoadTask — burns CPU when injectCpuLoad is true
void CpuLoadTask(void* parameter) {
  while (true) {
    if (injectCpuLoad) {
      uint32_t end = millis() + 200;
      while (millis() < end) {
        volatile float x = sqrtf((float)micros());
        (void)x;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void RaceIncTask(void* parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (int i = 0; i < 10000; i++) {
      if (raceUseMutex) {
        if (xSemaphoreTake(raceMutex, portMAX_DELAY) == pdTRUE) {
          sharedCounter++;
          xSemaphoreGive(raceMutex);
        }
      } else {
        int tmp = sharedCounter;
        if ((i % 32) == 0) taskYIELD();
        tmp++;
        sharedCounter = tmp;
      }

      if ((i % 250) == 0) taskYIELD();
    }

    xSemaphoreGive(raceDoneSemaphore);
  }
}

void RaceDecTask(void* parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (int i = 0; i < 10000; i++) {
      if (raceUseMutex) {
        if (xSemaphoreTake(raceMutex, portMAX_DELAY) == pdTRUE) {
          sharedCounter--;
          xSemaphoreGive(raceMutex);
        }
      } else {
        int tmp = sharedCounter;
        if ((i % 32) == 0) taskYIELD();
        tmp--;
        sharedCounter = tmp;
      }

      if ((i % 250) == 0) taskYIELD();
    }

    xSemaphoreGive(raceDoneSemaphore);
  }
}

// [REQ: RACE CONDITION DEMO] Runs two real tasks against the same shared counter.
void RaceDemoTask(void* parameter) {
  while (true) {
    bool runUnsafe = raceDemoUnsafe;
    bool runSafe   = raceDemoWithMutex;

    if (runUnsafe || runSafe) {
      raceDemoUnsafe = false;
      raceDemoWithMutex = false;
      raceUseMutex = runSafe;
      sharedCounter = 0;

      while (xSemaphoreTake(raceDoneSemaphore, 0) == pdTRUE) {
      }

      safePrintln(runSafe ? "[RACE] Starting SAFE demo with raceMutex..." :
                            "[RACE] Starting UNSAFE demo without mutex...");

      if (hRaceIncTask == NULL || hRaceDecTask == NULL) {
        safePrintln("[RACE] Worker tasks are not ready");
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      xTaskNotifyGive(hRaceIncTask);
      xTaskNotifyGive(hRaceDecTask);

      bool firstDone  = (xSemaphoreTake(raceDoneSemaphore, pdMS_TO_TICKS(2000)) == pdTRUE);
      bool secondDone = (xSemaphoreTake(raceDoneSemaphore, pdMS_TO_TICKS(2000)) == pdTRUE);

      if (firstDone && secondDone) {
        safePrintln((runSafe ? "[RACE] Mutex-protected result (expected 0): "
                             : "[RACE] Unsafe result (expected drift from 0 under contention): ") + String(sharedCounter));
      } else {
        safePrintln("[RACE] Demo timed out waiting for worker tasks");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// [REQ: PRIORITY INVERSION DEMO] Demonstrates wait time while FreeRTOS mutex inheritance is active.
void PrioInvLowTask(void* parameter) {
  while (true) {
    if (enablePriorityInversionDemo) {
      if (xSemaphoreTake(prioLowMutex, portMAX_DELAY) == pdTRUE) {
        safePrintln("[PRIOINV] Low-prio task acquired mutex, working 1000 ms...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // holds mutex while medium-prio runs
        xSemaphoreGive(prioLowMutex);
        safePrintln("[PRIOINV] Low-prio task released mutex");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

void PrioInvHighTask(void* parameter) {
  while (true) {
    if (enablePriorityInversionDemo) {
      uint32_t waitStart = micros();
      safePrintln("[PRIOINV] High-prio task waiting for mutex (priority inheritance should help)...");
      if (xSemaphoreTake(prioLowMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        uint32_t waited = micros() - waitStart;
        safePrintln("[PRIOINV] High-prio task got mutex after " + String(waited) + " us");
        xSemaphoreGive(prioLowMutex);
      } else {
        safePrintln("[PRIOINV] High-prio task TIMED OUT waiting for mutex!");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(4000));
  }
}

void PrioInvMidTask(void* parameter) {
  while (true) {
    if (enablePriorityInversionDemo) {
      // Medium-priority task consumes CPU, delaying the low-priority task release
      uint32_t end = millis() + 500;
      while (millis() < end) { volatile int x = millis(); (void)x; }
    }
    vTaskDelay(pdMS_TO_TICKS(3500));
  }
}

// =====================================================
// Setup
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TOUCH_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  analogReadResolution(12);
  analogSetPinAttenuation(LM35_PIN, ADC_11db); // custom chip spans 0-3.3V; needs full-range attenuation
  analogSetPinAttenuation(NTC_RR_PIN, ADC_11db);

  strcpy(latestRiskResult.riskLabel, "STARTING");
  latestRiskResult.riskProbability = 0.0;
  latestRiskResult.apiOk = false;
  latestRiskResult.vitals = latestSensorData;

  // Create RTOS objects
  serialMutex   = xSemaphoreCreateMutex();
  oledMutex     = xSemaphoreCreateMutex();
  i2cMaxMutex   = xSemaphoreCreateMutex();
  sensorDataMutex = xSemaphoreCreateMutex();
  riskMutex     = xSemaphoreCreateMutex();
  timingMutex   = xSemaphoreCreateMutex();
  raceMutex     = xSemaphoreCreateMutex();    // [REQ: RACE CONDITION DEMO]
  prioLowMutex  = xSemaphoreCreateMutex();   // [REQ: PRIORITY INVERSION DEMO]
  raceDoneSemaphore = xSemaphoreCreateCounting(2, 0);

  touchSemaphore = xSemaphoreCreateCounting(8, 0);  // [REQ: SEMAPHORE]

  resultQueue = xQueueCreate(5, sizeof(RiskResult));  // [REQ: QUEUE] size 5

  if (
    serialMutex == NULL || oledMutex == NULL || i2cMaxMutex == NULL ||
    sensorDataMutex == NULL || riskMutex == NULL || timingMutex == NULL ||
    raceMutex == NULL || prioLowMutex == NULL || raceDoneSemaphore == NULL ||
    touchSemaphore == NULL || resultQueue == NULL
  ) {
    Serial.println("Failed to create RTOS objects");
    while (true) {
      delay(1000);
    }
  }

  I2C_OLED.begin(OLED_SDA, OLED_SCL, 400000);
  I2C_MAX.begin(MAX_SDA, MAX_SCL, 400000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    safePrintln("OLED initialization failed");
  } else {
    if (xSemaphoreTake(oledMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(0, 0);
      oled.println("ESP32 Patient");
      oled.println("RTOS Monitor");
      oled.println();
      oled.println("Connecting WiFi");
      oled.display();
      xSemaphoreGive(oledMutex);
    }
  }

  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchISR, FALLING);

  safePrintln("ESP32 RTOS Patient Monitor Started");

  wifiOk = connectWiFi();

  if (xSemaphoreTake(oledMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print("WiFi: ");
    oled.println(wifiOk ? "OK" : "FAILED");
    oled.println();
    oled.println("RTOS API Mode");
    oled.display();
    xSemaphoreGive(oledMutex);
  }

  delay(1000);

  xTaskCreatePinnedToCore(AnalogSensorTask, "AnalogSensorTask", 4096, NULL, 2, &hAnalogTask, 1);
  xTaskCreatePinnedToCore(CommunicationSensorTask, "CommunicationTask", 4096, NULL, 2, &hCommTask, 1);
  xTaskCreatePinnedToCore(DigitalInputTask, "DigitalInputTask", 4096, NULL, 4, &hDigitalTask, 1);
  xTaskCreatePinnedToCore(ProcessingTask, "ProcessingTask", 8192, NULL, 3, &hProcessingTask, 1);
  xTaskCreatePinnedToCore(OutputTask, "OutputTask", 4096, NULL, 2, &hOutputTask, 1);
  xTaskCreatePinnedToCore(LoggingTask, "LoggingTask", 4096, NULL, 1, &hLoggingTask, 1);
  xTaskCreatePinnedToCore(CommandTask, "CommandTask", 4096, NULL, 1, NULL, 1);
  // [REQ: STRESS TEST] StressTask at priority 3 to compete with ProcessingTask
  xTaskCreatePinnedToCore(StressTask, "StressTask", 2048, NULL, 3, &hStressTask, 1);
  // [REQ: CPU LOAD] CpuLoadTask enabled via 'fault cpuload on' command
  xTaskCreatePinnedToCore(CpuLoadTask, "CpuLoadTask", 2048, NULL, 3, &hCpuLoadTask, 1);
  // [REQ: RACE CONDITION DEMO]
  xTaskCreatePinnedToCore(RaceIncTask, "RaceIncTask", 2048, NULL, 2, &hRaceIncTask, 1);
  xTaskCreatePinnedToCore(RaceDecTask, "RaceDecTask", 2048, NULL, 2, &hRaceDecTask, 1);
  xTaskCreatePinnedToCore(RaceDemoTask, "RaceDemoTask", 4096, NULL, 1, NULL, 1);
  // [REQ: PRIORITY INVERSION DEMO]
  xTaskCreatePinnedToCore(PrioInvLowTask,  "PrioInvLow",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(PrioInvHighTask, "PrioInvHigh", 2048, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(PrioInvMidTask,  "PrioInvMid",  2048, NULL, 2, NULL, 1);

  safePrintln("All RTOS tasks created");
  printCommandHelp();
}

// =====================================================
// Loop is empty because RTOS tasks run the system
// =====================================================

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
