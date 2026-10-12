/**
 * Smart Voice-Controlled Wheelchair
 * Web Speech API + Web Bluetooth (BLE UART) Controller
 * Department of Electronics & Telecommunication Engineering
 */

// Nordic UART Service standard UUIDs (universally supported across ESP32 BLE)
const BLE_CONFIG = {
  serviceUUID: "6e400001-b5a3-f393-e0a9-e50e24dcca9e",
  rxCharUUID:  "6e400002-b5a3-f393-e0a9-e50e24dcca9e", // Write to ESP32
  txCharUUID:  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // Read from ESP32
};

// UI Elements
const btnConnect = document.getElementById("btnConnect");
const btnConnectText = document.getElementById("btnConnectText");
const statusDot = document.getElementById("statusDot");
const statusLabel = document.getElementById("statusLabel");
const btnToggleVoice = document.getElementById("btnToggleVoice");
const btnVoiceText = document.getElementById("btnVoiceText");
const micIcon = document.getElementById("micIcon");
const listeningTag = document.getElementById("listeningTag");
const liveTranscript = document.getElementById("liveTranscript");
const lastCommandToken = document.getElementById("lastCommandToken");
const chkContinuous = document.getElementById("chkContinuous");
const chkWakeWord = document.getElementById("chkWakeWord");
const speedTag = document.getElementById("speedTag");
const valHeadlights = document.getElementById("valHeadlights");
const valSpeedDuty = document.getElementById("valSpeedDuty");
const logContainer = document.getElementById("logContainer");
const btnClearLog = document.getElementById("btnClearLog");
const btnToggleSpeed = document.getElementById("btnToggleSpeed");
const btnToggleLights = document.getElementById("btnToggleLights");
const btnEmergencyStop = document.getElementById("btnEmergencyStop");

// State Variables
let bleDevice = null;
let rxCharacteristic = null;
let txCharacteristic = null;
let isConnected = false;

let speechRecognition = null;
let isListening = false;
let shouldKeepListening = true;

let currentDirection = "STP";
let currentSpeed = "SLOW";
let lightsOn = false;
let lastProcessedTime = 0;
let lastCommandText = "";

// ==========================================================================
// 1. Logging Helper
// ==========================================================================
function addLog(message, type = "info") {
  const line = document.createElement("div");
  line.className = `log-line ${type}`;
  const time = new Date().toLocaleTimeString("en-US", { hour12: false });
  line.textContent = `[${time}] ${message}`;
  logContainer.appendChild(line);
  logContainer.scrollTop = logContainer.scrollHeight;
}

btnClearLog.addEventListener("click", () => {
  logContainer.innerHTML = "";
  addLog("Log cleared.", "info");
});

// ==========================================================================
// 2. Web Bluetooth Implementation
// ==========================================================================
btnConnect.addEventListener("click", async () => {
  if (isConnected) {
    disconnectBLE();
  } else {
    await connectBLE();
  }
});

async function connectBLE() {
  if (!navigator.bluetooth) {
    addLog("Web Bluetooth is not supported in this browser. Use Chrome on Android/PC.", "danger");
    alert("Web Bluetooth not supported! Please open this in Chrome on Android or PC.");
    return;
  }

  try {
    addLog("Scanning for ESP32 Bluetooth device...", "info");
    btnConnect.disabled = true;
    btnConnectText.textContent = "Connecting...";

    bleDevice = await navigator.bluetooth.requestDevice({
      filters: [
        { namePrefix: "Wheelchair" },
        { namePrefix: "ESP32" }
      ],
      optionalServices: [BLE_CONFIG.serviceUUID]
    });

    bleDevice.addEventListener("gattserverdisconnected", onDisconnected);

    addLog(`Found device: ${bleDevice.name}. Connecting GATT...`, "info");
    const server = await bleDevice.gatt.connect();

    const service = await server.getPrimaryService(BLE_CONFIG.serviceUUID);
    rxCharacteristic = await service.getCharacteristic(BLE_CONFIG.rxCharUUID);
    txCharacteristic = await service.getCharacteristic(BLE_CONFIG.txCharUUID);

    // Subscribe to telemetry responses from ESP32
    await txCharacteristic.startNotifications();
    txCharacteristic.addEventListener("characteristicvaluechanged", handleESP32Telemetry);

    // Update UI
    isConnected = true;
    statusDot.classList.add("connected");
    statusLabel.textContent = `CONNECTED (${bleDevice.name})`;
    btnConnectText.textContent = "Disconnect ESP32";
    btnConnect.classList.remove("btn-primary");
    btnConnect.classList.add("btn-secondary");
    btnToggleVoice.disabled = false;
    btnConnect.disabled = false;

    addLog(`Connected to ${bleDevice.name} successfully!`, "rx");

    // Automatically start voice listening upon connection
    if (!isListening) {
      startSpeechRecognition();
    }
  } catch (error) {
    addLog(`Bluetooth connection error: ${error.message || error}`, "danger");
    resetConnectionUI();
  }
}

function disconnectBLE() {
  if (bleDevice && bleDevice.gatt.connected) {
    bleDevice.gatt.disconnect();
  }
  resetConnectionUI();
}

function onDisconnected() {
  addLog("ESP32 Bluetooth disconnected.", "warn");
  resetConnectionUI();
}

function resetConnectionUI() {
  isConnected = false;
  rxCharacteristic = null;
  txCharacteristic = null;
  statusDot.classList.remove("connected");
  statusLabel.textContent = "DISCONNECTED";
  btnConnectText.textContent = "Connect ESP32 (BLE)";
  btnConnect.classList.add("btn-primary");
  btnConnect.classList.remove("btn-secondary");
  btnConnect.disabled = false;
}

// Receive incoming serial telemetry from ESP32
function handleESP32Telemetry(event) {
  const value = new TextDecoder().decode(event.target.value);
  addLog(`ESP32 -> ${value.trim()}`, "rx");
}

// Send command token to ESP32 over BLE
async function sendCommand(token) {
  addLog(`TX -> ${token}`, "tx");

  if (!rxCharacteristic || !isConnected) {
    addLog(`[Local Simulation Mode]: Command '${token}' simulated. (BLE not connected)`, "warn");
    updateUIState(token);
    return;
  }

  try {
    const encoder = new TextEncoder();
    const payload = encoder.encode(token + "\n");
    await rxCharacteristic.writeValue(payload);
    updateUIState(token);
  } catch (error) {
    addLog(`Failed to send '${token}': ${error.message}`, "danger");
  }
}

// ==========================================================================
// 3. Web Speech Recognition (Continuous Assistant Mode)
// ==========================================================================
function initSpeechRecognition() {
  const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;

  if (!SpeechRecognition) {
    addLog("Web Speech API not supported in this browser. Use Chrome.", "danger");
    btnToggleVoice.disabled = true;
    liveTranscript.textContent = "Speech recognition not supported in this browser.";
    return;
  }

  speechRecognition = new SpeechRecognition();
  speechRecognition.continuous = true;
  speechRecognition.interimResults = true;
  speechRecognition.lang = "en-US";
  speechRecognition.maxAlternatives = 1;

  speechRecognition.onstart = () => {
    isListening = true;
    listeningTag.textContent = "LISTENING 🎙️";
    listeningTag.classList.add("active");
    btnVoiceText.textContent = "Stop Continuous Mic";
    btnToggleVoice.classList.add("listening");
    addLog("Microphone active. Continuous listening started.", "info");
  };

  speechRecognition.onresult = (event) => {
    let transcript = "";
    let isFinal = false;

    for (let i = event.resultIndex; i < event.results.length; i++) {
      transcript += event.results[i][0].transcript;
      if (event.results[i].isFinal) {
        isFinal = true;
      }
    }

    transcript = transcript.trim();
    if (transcript.length > 0) {
      liveTranscript.textContent = `"${transcript}"`;
      parseAndExecuteVoice(transcript.toLowerCase(), isFinal);
    }
  };

  speechRecognition.onerror = (event) => {
    if (event.error === "no-speech") {
      // Normal silence timeout; continuous loop will handle restart
      return;
    }
    addLog(`Speech Recognition warning: ${event.error}`, "warn");
  };

  speechRecognition.onend = () => {
    isListening = false;
    // Auto-restart if continuous mode is enabled and user hasn't explicitly stopped it
    if (shouldKeepListening && chkContinuous.checked) {
      setTimeout(() => {
        try {
          speechRecognition.start();
        } catch (e) {
          // Handled if already starting
        }
      }, 250);
    } else {
      listeningTag.textContent = "MIC IDLE";
      listeningTag.classList.remove("active");
      btnVoiceText.textContent = "Start Continuous Mic";
      btnToggleVoice.classList.remove("listening");
      addLog("Microphone stopped.", "info");
    }
  };
}

function startSpeechRecognition() {
  if (!speechRecognition) return;
  shouldKeepListening = true;
  try {
    speechRecognition.start();
  } catch (e) {
    console.log("Recognition already active", e);
  }
}

function stopSpeechRecognition() {
  shouldKeepListening = false;
  if (speechRecognition && isListening) {
    speechRecognition.stop();
  }
}

btnToggleVoice.addEventListener("click", () => {
  if (isListening) {
    stopSpeechRecognition();
  } else {
    startSpeechRecognition();
  }
});

chkContinuous.addEventListener("change", () => {
  if (!chkContinuous.checked && isListening) {
    stopSpeechRecognition();
  }
});

// ==========================================================================
// 4. Voice Parsing Engine & Command Dictionary
// ==========================================================================
function parseAndExecuteVoice(rawText, isFinal) {
  const now = Date.now();
  // Prevent duplicate execution within 650ms for same phrase
  if (rawText === lastCommandText && now - lastProcessedTime < 650) {
    return;
  }

  // Wake Word check if option is selected
  const requireWake = chkWakeWord.checked;
  let targetPhrase = rawText;

  if (requireWake) {
    const hasWakeWord = rawText.includes("chair") || rawText.includes("robot") || rawText.includes("wheelchair");
    if (!hasWakeWord) {
      return; // Ignore speech that doesn't mention wake word
    }
    // Remove wake word to parse clean action
    targetPhrase = rawText.replace(/chair|robot|wheelchair/g, "").trim();
  }

  // Command Keyword Dictionary
  let detectedToken = null;

  // STOP / HALT / RUKO (Highest Priority)
  if (/\b(stop|halt|freeze|pause|ruko|break|brake)\b/.test(targetPhrase)) {
    detectedToken = "CMD:STP";
  }
  // EMERGENCY STOP
  else if (/\b(emergency|danger|shut down|kill)\b/.test(targetPhrase)) {
    detectedToken = "CMD:ESTOP";
  }
  // FORWARD
  else if (/\b(forward|front|straight|go ahead|ahead|aage|chalo)\b/.test(targetPhrase)) {
    detectedToken = "CMD:FWD";
  }
  // BACKWARD
  else if (/\b(backward|back|reverse|peeche|piche)\b/.test(targetPhrase)) {
    detectedToken = "CMD:REV";
  }
  // LEFT
  else if (/\b(left|turn left|baye|baaye)\b/.test(targetPhrase)) {
    detectedToken = "CMD:LFT";
  }
  // RIGHT
  else if (/\b(right|turn right|daye|daaye)\b/.test(targetPhrase)) {
    detectedToken = "CMD:RGT";
  }
  // SPEED CONTROL
  else if (/\b(slow|slow down|dheere)\b/.test(targetPhrase)) {
    detectedToken = "CMD:SPD_LOW";
  }
  else if (/\b(fast|speed up|quick|tez|jaldi)\b/.test(targetPhrase)) {
    detectedToken = "CMD:SPD_HIGH";
  }
  // LIGHTS CONTROL
  else if (/\b(lights on|light on|turn on light)\b/.test(targetPhrase)) {
    detectedToken = "CMD:LGT_ON";
  }
  else if (/\b(lights off|light off|turn off light)\b/.test(targetPhrase)) {
    detectedToken = "CMD:LGT_OFF";
  }

  if (detectedToken) {
    lastProcessedTime = now;
    lastCommandText = rawText;
    lastCommandToken.textContent = `${detectedToken} (${rawText})`;
    sendCommand(detectedToken);
  }
}

// ==========================================================================
// 5. UI State Sync & Touch Manual Fallbacks
// ==========================================================================
function updateUIState(token) {
  const dirCells = ["dirFWD", "dirREV", "dirLFT", "dirRGT", "dirSTP"];
  dirCells.forEach(id => {
    const el = document.getElementById(id);
    if (el) el.classList.remove("active");
  });

  if (token === "CMD:FWD") {
    currentDirection = "FWD";
    document.getElementById("dirFWD").classList.add("active");
  } else if (token === "CMD:REV") {
    currentDirection = "REV";
    document.getElementById("dirREV").classList.add("active");
  } else if (token === "CMD:LFT") {
    currentDirection = "LFT";
    document.getElementById("dirLFT").classList.add("active");
  } else if (token === "CMD:RGT") {
    currentDirection = "RGT";
    document.getElementById("dirRGT").classList.add("active");
  } else if (token === "CMD:STP" || token === "CMD:ESTOP") {
    currentDirection = "STP";
    document.getElementById("dirSTP").classList.add("active");
  } else if (token === "CMD:SPD_LOW") {
    currentSpeed = "SLOW";
    speedTag.textContent = "SPEED: SLOW";
    valSpeedDuty.textContent = "45% (SLOW)";
  } else if (token === "CMD:SPD_HIGH") {
    currentSpeed = "FAST";
    speedTag.textContent = "SPEED: FAST";
    valSpeedDuty.textContent = "80% (FAST)";
  } else if (token === "CMD:LGT_ON") {
    lightsOn = true;
    valHeadlights.textContent = "ON";
  } else if (token === "CMD:LGT_OFF") {
    lightsOn = false;
    valHeadlights.textContent = "OFF";
  }
}

// Manual Touch Buttons
document.querySelectorAll(".pad-btn").forEach(btn => {
  btn.addEventListener("click", () => {
    const cmd = btn.getAttribute("data-cmd");
    sendCommand(`CMD:${cmd}`);
  });
});

btnToggleSpeed.addEventListener("click", () => {
  if (currentSpeed === "SLOW") {
    sendCommand("CMD:SPD_HIGH");
  } else {
    sendCommand("CMD:SPD_LOW");
  }
});

btnToggleLights.addEventListener("click", () => {
  if (lightsOn) {
    sendCommand("CMD:LGT_OFF");
  } else {
    sendCommand("CMD:LGT_ON");
  }
});

btnEmergencyStop.addEventListener("click", () => {
  sendCommand("CMD:ESTOP");
});

// Initialize Speech Recognition on page load
window.addEventListener("DOMContentLoaded", () => {
  initSpeechRecognition();
  // Enable voice button immediately so users can test speech without waiting for BLE
  btnToggleVoice.disabled = false;
});
