/*
 * ==============================================================================================
 * PROJECT: SMART VOICE-CONTROLLED WHEELCHAIR
 * INSTITUTION: Late G. N. Sapkal College of Engineering, Nashik
 * DEPARTMENT: Electronics and Telecommunication Engineering (B.E. E&TC 2026-2027)
 * STUDENTS: Pranjal Samadhan Sapkal, Purva Pradip Kothawade, Premal Bachhav
 * GUIDE: Prof. S. K. Khaire
 * 
 * TARGET HARDWARE: ESP32 DevKit V1 (30-Pin Microcontroller)
 * COMPATIBILITY: ESP32 Arduino Core 2.x and 3.x+ Fully Supported!
 * COMMUNICATION: Web Bluetooth (BLE Nordic UART Service)
 * BAUD RATE: 115200 bps
 * ==============================================================================================
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_arduino_version.h>

// ----------------------------------------------------------------------------------------------
// 1. PIN DEFINITIONS (Optimized for 30-Pin ESP32 DevKit V1)
// ----------------------------------------------------------------------------------------------
#define LED_STATUS_PIN      2   // Built-in Blue LED on 30-pin board

// TB6612FNG Dual MOSFET Motor Driver Pins
#define MOTOR_PWMA_PIN      25  // Left Motor Speed (PWM Pin)
#define MOTOR_AIN1_PIN      26  // Left Motor Direction 1
#define MOTOR_AIN2_PIN      27  // Left Motor Direction 2
#define MOTOR_STBY_PIN      14  // Hardware Standby (HIGH = Active, LOW = Motors Free)
#define MOTOR_BIN1_PIN      12  // Right Motor Direction 1
#define MOTOR_BIN2_PIN      13  // Right Motor Direction 2
#define MOTOR_PWMB_PIN      33  // Right Motor Speed (PWM Pin)

// Safety Sensors (Planned for when hardware arrives)
#define US_TRIG_PIN         5   // HC-SR04 Ultrasonic Trigger
#define US_ECHO_PIN         18  // HC-SR04 Ultrasonic Echo
#define IR_LEFT_PIN         19  // Left Ground / Cliff Proximity Sensor
#define IR_RIGHT_PIN        21  // Right Ground / Cliff Proximity Sensor
#define ESTOP_BUTTON_PIN    23  // Physical Emergency Stop Button (Active LOW)

// Peripheral & Indicators
#define HEADLIGHT_PIN       4   // Front Ultra-bright Headlights
#define BUZZER_PIN          15  // Siren Warning Buzzer
#define LDR_ANALOG_PIN      34  // Ambient Light Sensor (ADC1 CH6 - Input only)

// PWM Configuration for ESP32 LEDC hardware timers
#define PWM_FREQ            1000
#define PWM_RESOLUTION      8   // 8-bit resolution (0 - 255)
#define PWM_CHANNEL_A       0
#define PWM_CHANNEL_B       1

// Speed Duty Cycles
#define SPEED_SLOW_DUTY     115 // ~45% duty cycle
#define SPEED_FAST_DUTY     204 // ~80% duty cycle

// ----------------------------------------------------------------------------------------------
// 2. BLE NORDIC UART SERVICE DEFINITIONS
// ----------------------------------------------------------------------------------------------
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e" // UART Service UUID
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // Web App writes here
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // ESP32 notifies web app

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ----------------------------------------------------------------------------------------------
// 3. SYSTEM STATE VARIABLES
// ----------------------------------------------------------------------------------------------
enum WheelchairState {
  STATE_STOPPED,
  STATE_FORWARD,
  STATE_BACKWARD,
  STATE_LEFT,
  STATE_RIGHT,
  STATE_ESTOP
};

WheelchairState currentState = STATE_STOPPED;
uint8_t currentSpeedDuty = SPEED_SLOW_DUTY;
const char* currentSpeedName = "SLOW (45%)";
bool headlightsActive = false;
unsigned long lastHeartbeatMs = 0;

// ----------------------------------------------------------------------------------------------
// 4. FUNCTION DECLARATIONS & LEDC COMPATIBILITY WRAPPERS
// ----------------------------------------------------------------------------------------------
void initHardwarePins();
void applyMotorOutputs(WheelchairState state, uint8_t duty);
void processIncomingCommand(String cmd);
void printBanner();
void printStatusBox(const char* action, const char* leftMotor, const char* rightMotor);
void sendTelemetry(String msg);

// Core 2.x vs Core 3.x cross-version PWM writers
void writePWM_A(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(MOTOR_PWMA_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL_A, duty);
#endif
}

void writePWM_B(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(MOTOR_PWMB_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL_B, duty);
#endif
}

// ----------------------------------------------------------------------------------------------
// 5. BLE SERVER CALLBACKS
// ----------------------------------------------------------------------------------------------
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    digitalWrite(LED_STATUS_PIN, HIGH);
    Serial.println();
    Serial.println("==================================================================");
    Serial.println("  [BLE EVENT] Smartphone Connected to Wheelchair!");
    Serial.println("  [STATUS] Continuous Voice Pipeline is ONLINE & Ready.");
    Serial.println("==================================================================");
    Serial.println();
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    digitalWrite(LED_STATUS_PIN, LOW);
    applyMotorOutputs(STATE_STOPPED, 0); // Safety fail-safe on disconnect
    Serial.println();
    Serial.println("  [BLE EVENT] Smartphone Disconnected. Motors safely frozen.");
    Serial.println();
  }
};

// ----------------------------------------------------------------------------------------------
// 6. BLE CHARACTERISTIC WRITE CALLBACK (RECEIVES WEB APP TOKENS)
// ----------------------------------------------------------------------------------------------
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    rxValue.trim();

    if (rxValue.length() > 0) {
      // Brief LED pulse for visual command confirmation
      digitalWrite(LED_STATUS_PIN, LOW);
      delay(30);
      digitalWrite(LED_STATUS_PIN, HIGH);

      processIncomingCommand(rxValue);
    }
  }
};

// ----------------------------------------------------------------------------------------------
// 7. SETUP
// ----------------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  printBanner();
  initHardwarePins();

  // Initialize BLE Device
  Serial.println("[INIT] Initializing Bluetooth Low Energy (BLE)...");
  BLEDevice::init("Wheelchair-ESP32");

  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create Nordic UART Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create TX Characteristic (ESP32 -> Phone)
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  // Create RX Characteristic (Phone -> ESP32)
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // Start the service
  pService->start();

  // Start BLE Advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // iPhone connection optimization
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[INIT] BLE Advertising started as 'Wheelchair-ESP32'.");
  Serial.println("[INIT] Waiting for web app connection in Chrome...");
  Serial.println("------------------------------------------------------------------");
}

// ----------------------------------------------------------------------------------------------
// 8. MAIN LOOP
// ----------------------------------------------------------------------------------------------
void loop() {
  // Handle BLE reconnection cycle
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("[BLE] Re-advertising for connection...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Periodic heartbeat / status output every 5 seconds on Serial
  if (millis() - lastHeartbeatMs > 5000) {
    lastHeartbeatMs = millis();
    if (!deviceConnected) {
      Serial.println("[IDLE] Waiting for Web App connection... (Open Chrome on your phone)");
    }
  }

  delay(20);
}

// ----------------------------------------------------------------------------------------------
// 9. COMMAND PROCESSOR & STATE MACHINE
// ----------------------------------------------------------------------------------------------
void processIncomingCommand(String cmd) {
  cmd.toUpperCase();

  Serial.println();
  Serial.print("[RX PACKET RECEIVED]: ");
  Serial.println(cmd);

  if (cmd == "CMD:FWD") {
    currentState = STATE_FORWARD;
    applyMotorOutputs(currentState, currentSpeedDuty);
    printStatusBox("DRIVE FORWARD", "FORWARD (CW)", "FORWARD (CW)");
    sendTelemetry("ACK:FORWARD_OK");
  } 
  else if (cmd == "CMD:REV") {
    currentState = STATE_BACKWARD;
    applyMotorOutputs(currentState, currentSpeedDuty);
    printStatusBox("DRIVE BACKWARD", "REVERSE (CCW)", "REVERSE (CCW)");
    sendTelemetry("ACK:BACKWARD_OK");
  } 
  else if (cmd == "CMD:LFT") {
    currentState = STATE_LEFT;
    applyMotorOutputs(currentState, currentSpeedDuty);
    printStatusBox("PIVOT TURN LEFT", "REVERSE (CCW)", "FORWARD (CW)");
    sendTelemetry("ACK:LEFT_OK");
  } 
  else if (cmd == "CMD:RGT") {
    currentState = STATE_RIGHT;
    applyMotorOutputs(currentState, currentSpeedDuty);
    printStatusBox("PIVOT TURN RIGHT", "FORWARD (CW)", "REVERSE (CCW)");
    sendTelemetry("ACK:RIGHT_OK");
  } 
  else if (cmd == "CMD:STP") {
    currentState = STATE_STOPPED;
    applyMotorOutputs(currentState, 0);
    printStatusBox("MOTOR STOP", "BRAKE (LOW)", "BRAKE (LOW)");
    sendTelemetry("ACK:STOP_OK");
  } 
  else if (cmd == "CMD:ESTOP") {
    currentState = STATE_ESTOP;
    applyMotorOutputs(currentState, 0);
    digitalWrite(MOTOR_STBY_PIN, LOW); // Immediate TB6612 hardware cutoff
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
    printStatusBox("EMERGENCY SHUTDOWN", "HARDWARE CUTOFF", "HARDWARE CUTOFF");
    sendTelemetry("ACK:ESTOP_TRIGGERED");
  } 
  else if (cmd == "CMD:SPD_LOW") {
    currentSpeedDuty = SPEED_SLOW_DUTY;
    currentSpeedName = "SLOW (45%)";
    if (currentState != STATE_STOPPED && currentState != STATE_ESTOP) {
      applyMotorOutputs(currentState, currentSpeedDuty);
    }
    Serial.print("  [SPEED SETTING] Switched to: ");
    Serial.println(currentSpeedName);
    sendTelemetry("ACK:SPEED_SLOW");
  } 
  else if (cmd == "CMD:SPD_HIGH") {
    currentSpeedDuty = SPEED_FAST_DUTY;
    currentSpeedName = "FAST (80%)";
    if (currentState != STATE_STOPPED && currentState != STATE_ESTOP) {
      applyMotorOutputs(currentState, currentSpeedDuty);
    }
    Serial.print("  [SPEED SETTING] Switched to: ");
    Serial.println(currentSpeedName);
    sendTelemetry("ACK:SPEED_FAST");
  } 
  else if (cmd == "CMD:LGT_ON") {
    headlightsActive = true;
    digitalWrite(HEADLIGHT_PIN, HIGH);
    Serial.println("  [PERIPHERAL] Ultra-Bright Headlights: ON");
    sendTelemetry("ACK:LIGHTS_ON");
  } 
  else if (cmd == "CMD:LGT_OFF") {
    headlightsActive = false;
    digitalWrite(HEADLIGHT_PIN, LOW);
    Serial.println("  [PERIPHERAL] Ultra-Bright Headlights: OFF");
    sendTelemetry("ACK:LIGHTS_OFF");
  } 
  else {
    Serial.print("  [WARN] Unknown command token: ");
    Serial.println(cmd);
    sendTelemetry("ERR:UNKNOWN_TOKEN");
  }
}

// ----------------------------------------------------------------------------------------------
// 10. MOTOR OUTPUT CONTROLLER (TB6612FNG H-BRIDGE MAPPING)
// ----------------------------------------------------------------------------------------------
void applyMotorOutputs(WheelchairState state, uint8_t duty) {
  // When active, ensure STBY pin is HIGH
  if (state != STATE_STOPPED && state != STATE_ESTOP) {
    digitalWrite(MOTOR_STBY_PIN, HIGH);
  }

  switch (state) {
    case STATE_FORWARD:
      // Left Motor Forward
      digitalWrite(MOTOR_AIN1_PIN, HIGH);
      digitalWrite(MOTOR_AIN2_PIN, LOW);
      writePWM_A(duty);

      // Right Motor Forward
      digitalWrite(MOTOR_BIN1_PIN, HIGH);
      digitalWrite(MOTOR_BIN2_PIN, LOW);
      writePWM_B(duty);
      break;

    case STATE_BACKWARD:
      // Left Motor Reverse
      digitalWrite(MOTOR_AIN1_PIN, LOW);
      digitalWrite(MOTOR_AIN2_PIN, HIGH);
      writePWM_A(duty);

      // Right Motor Reverse
      digitalWrite(MOTOR_BIN1_PIN, LOW);
      digitalWrite(MOTOR_BIN2_PIN, HIGH);
      writePWM_B(duty);
      break;

    case STATE_LEFT:
      // Pivot Turn: Left Motor Reverse, Right Motor Forward
      digitalWrite(MOTOR_AIN1_PIN, LOW);
      digitalWrite(MOTOR_AIN2_PIN, HIGH);
      writePWM_A(duty);

      digitalWrite(MOTOR_BIN1_PIN, HIGH);
      digitalWrite(MOTOR_BIN2_PIN, LOW);
      writePWM_B(duty);
      break;

    case STATE_RIGHT:
      // Pivot Turn: Left Motor Forward, Right Motor Reverse
      digitalWrite(MOTOR_AIN1_PIN, HIGH);
      digitalWrite(MOTOR_AIN2_PIN, LOW);
      writePWM_A(duty);

      digitalWrite(MOTOR_BIN1_PIN, LOW);
      digitalWrite(MOTOR_BIN2_PIN, HIGH);
      writePWM_B(duty);
      break;

    case STATE_STOPPED:
    case STATE_ESTOP:
    default:
      // Active Brake: Both direction pins LOW, PWM 0
      digitalWrite(MOTOR_AIN1_PIN, LOW);
      digitalWrite(MOTOR_AIN2_PIN, LOW);
      writePWM_A(0);

      digitalWrite(MOTOR_BIN1_PIN, LOW);
      digitalWrite(MOTOR_BIN2_PIN, LOW);
      writePWM_B(0);

      if (state == STATE_ESTOP) {
        digitalWrite(MOTOR_STBY_PIN, LOW); // Cut motor bridge power completely
      }
      break;
  }
}

// ----------------------------------------------------------------------------------------------
// 11. HARDWARE PIN INITIALIZATION (Dual Core 2.x & 3.x Compatibility)
// ----------------------------------------------------------------------------------------------
void initHardwarePins() {
  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);

  pinMode(MOTOR_AIN1_PIN, OUTPUT);
  pinMode(MOTOR_AIN2_PIN, OUTPUT);
  pinMode(MOTOR_BIN1_PIN, OUTPUT);
  pinMode(MOTOR_BIN2_PIN, OUTPUT);
  pinMode(MOTOR_STBY_PIN, OUTPUT);
  digitalWrite(MOTOR_STBY_PIN, HIGH);

  pinMode(HEADLIGHT_PIN, OUTPUT);
  digitalWrite(HEADLIGHT_PIN, LOW);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Setup ESP32 Hardware PWM channels for Motor A & B
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  // ESP32 Arduino Core 3.0+ Modern API
  ledcAttach(MOTOR_PWMA_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(MOTOR_PWMB_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(MOTOR_PWMA_PIN, 0);
  ledcWrite(MOTOR_PWMB_PIN, 0);
#else
  // ESP32 Arduino Core 2.0.x Legacy API
  ledcSetup(PWM_CHANNEL_A, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(MOTOR_PWMA_PIN, PWM_CHANNEL_A);
  ledcWrite(PWM_CHANNEL_A, 0);

  ledcSetup(PWM_CHANNEL_B, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(MOTOR_PWMB_PIN, PWM_CHANNEL_B);
  ledcWrite(PWM_CHANNEL_B, 0);
#endif

  Serial.println("[INIT] All GPIO pins & PWM channels configured successfully.");
}

// ----------------------------------------------------------------------------------------------
// 12. TELEMETRY & FORMATTED SERIAL PRINTERS
// ----------------------------------------------------------------------------------------------
void sendTelemetry(String msg) {
  if (deviceConnected && pTxCharacteristic) {
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
  }
}

void printBanner() {
  Serial.println();
  Serial.println("+------------------------------------------------------------------+");
  Serial.println("|          SMART VOICE-CONTROLLED WHEELCHAIR (PROTOTYPE)           |");
  Serial.println("|  Late G. N. Sapkal College of Engineering - B.E. E&TC 2026-2027  |");
  Serial.println("|  Team: Pranjal Sapkal | Purva Kothawade | Premal Bachhav         |");
  Serial.println("|  Guide: Prof. S. K. Khaire                                       |");
  Serial.println("+------------------------------------------------------------------+");
  Serial.println("|  Controller : ESP32 DevKit V1 (30-Pin)                           |");
  Serial.println("|  Link       : Web Bluetooth Low Energy (Nordic UART Service)     |");
  Serial.println("|  Baud Rate  : 115200 bps                                         |");
  Serial.println("+------------------------------------------------------------------+");
  Serial.println();
}

void printStatusBox(const char* action, const char* leftMotor, const char* rightMotor) {
  Serial.println("  ┌────────────────────────────────────────────────────────────┐");
  Serial.printf ("  │ ACTION      : %-43s│\n", action);
  Serial.printf ("  │ LEFT MOTOR  : %-43s│\n", leftMotor);
  Serial.printf ("  │ RIGHT MOTOR : %-43s│\n", rightMotor);
  Serial.printf ("  │ SPEED PWM   : %-43s│\n", currentSpeedName);
  Serial.printf ("  │ HEADLIGHTS  : %-43s│\n", headlightsActive ? "ON" : "OFF");
  Serial.println("  └────────────────────────────────────────────────────────────┘");
}
