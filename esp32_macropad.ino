#include "common.h"

// -----------------------------------------------------------------------------
// Global variable definitions (extern declarations are in common.h)
// -----------------------------------------------------------------------------

String stored_ssid = "";
String stored_pass = "";
String stored_tv_ip = DEFAULT_TV_IP;
String stored_tv_auth = DEFAULT_TV_AUTH;
String api_token = "";
uint32_t config_rev = 0;
bool events_enabled = false;
int stored_tv_port = DEFAULT_TV_PORT;
int pairingReqToken = 0;

String dev_names[3] = { "Device 1", "Device 2", "Device 3" };
String dev_bindings[3][11];
String tv_name = "TV";
String tv_bindings[11];

PadMode currentMode = MODE_BLE;
uint8_t currentBleSlot = 0; // 0 = Device 1, 1 = Device 2, 2 = Device 3
const char* bleDeviceName = "BertoPad";

// Per-host bond tracking: bonded peer BLE address string per host slot
char hostBondAddr[3][18] = { "" };
bool hostBondKnown[3] = { false };

// Button timing for long-press bond clearing and Mode+Host forget
unsigned long hostBtnPressStart[3] = { 0 };

bool inSetupMode = false;
bool setup_done = false;
bool mdnsStarted = false;

size_t otaTotalSize = 0;
size_t otaBytesWritten = 0;
String setupSsid = "";
String setupPass = "";

Preferences prefs;
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BleKeyboard bleKeyboard("BertoPad", "Studio MacroPad", 100);
WiFiClientSecure tvClient;
unsigned long lastTvKeepAlive = 0;

// -----------------------------------------------------------------------------
// Display dim / off / light-sleep power management (Step 5)
// -----------------------------------------------------------------------------
DisplayState g_displayState = DISPLAY_ON;
unsigned long g_lastActivity = 0;
unsigned long g_dimMs = 60000;
unsigned long g_offMs = 300000;
bool g_sleeping = false;

// -----------------------------------------------------------------------------
// Hardware Slot Definitions (11 Slots per Device Profile)
// -----------------------------------------------------------------------------
const char* const SLOT_IDS[11] = {
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "dial", "enc_cw", "enc_ccw"
};

const char* const DEFAULT_BLANK_BINDINGS[11] = {
  "", "", "", "", "", "", "", "", "", "", ""
};

const char* const DEFAULT_TV_BINDINGS[11] = {
  "TV:POWER", "APP:YOUTUBE", "APP:DISNEY", "APP:TUBI",
  "APP:NETFLIX", "APP:PRIME", "TV:INFO", "TV:HOME",
  "TV:MUTE", "TV:VOL_UP", "TV:VOL_DOWN"
};

const char* const SUPPORTED_ACTIONS[] = {
  "MEDIA:PLAY_PAUSE", "MEDIA:MUTE", "MEDIA:VOL_UP", "MEDIA:VOL_DOWN",
  "MEDIA:NEXT", "MEDIA:PREV", "KEY:F13", "KEY:F14", "KEY:F15", "KEY:F16",
  "MACRO:COPY", "MACRO:PASTE", "MACRO:CUT", "MACRO:UNDO", "MACRO:REDO",
  "MACRO:SELECT_ALL", "MACRO:SAVE", "MACRO:SPOTLIGHT", "MACRO:LOCK",
  "MACRO:NEW_TAB", "MACRO:CLOSE_TAB", "TV:POWER", "TV:VOL_UP",
  "TV:VOL_DOWN", "TV:MUTE", "TV:HOME", "TV:BACK", "TV:OK", "TV:INPUT",
  "APP:YOUTUBE", "APP:DISNEY", "APP:TUBI", "APP:NETFLIX", "APP:PRIME",
  "APP:APPLETV",
  "WIN:COPY", "WIN:PASTE", "WIN:CUT", "WIN:UNDO", "WIN:REDO",
  "WIN:SELECT_ALL", "WIN:SAVE", "WIN:NEW_TAB", "WIN:CLOSE_TAB", "WIN:LOCK",
  "WIN:SEARCH", "WIN:EXPLORER", "WIN:TASK_MGR", "WIN:SNIP", "WIN:DESKTOP",
  "WIN:RUN",
  "BLE:HOST1", "BLE:HOST2", "BLE:HOST3", "BLE:NEXT_HOST",
  "TV:UP", "TV:DOWN", "TV:LEFT", "TV:RIGHT", "TV:EXIT", "TV:MENU",
  "TV:INFO", "TV:GUIDE", "TV:CC", "TV:CH_UP", "TV:CH_DOWN", "TV:PREV_CH",
  "TV:PLAY", "TV:PAUSE", "TV:STOP", "TV:REWIND", "TV:FFWD"
};
const size_t SUPPORTED_ACTION_COUNT = sizeof(SUPPORTED_ACTIONS) / sizeof(SUPPORTED_ACTIONS[0]);

// -----------------------------------------------------------------------------
// Buxton State-Table Encoder
// -----------------------------------------------------------------------------
volatile int encoderDelta = 0;
volatile uint8_t encState = 0;
const uint8_t ttable[7][4] = {
  {0x0, 0x2, 0x4, 0x0}, {0x3, 0x0, 0x1, 0x10},
  {0x3, 0x2, 0x0, 0x0}, {0x3, 0x2, 0x1, 0x0},
  {0x6, 0x0, 0x4, 0x0}, {0x6, 0x5, 0x0, 0x20},
  {0x6, 0x5, 0x4, 0x0},
};

void IRAM_ATTR isrEncoder() {
  uint8_t pinstate = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  encState = ttable[encState & 0x0F][pinstate];
  uint8_t result = encState & 0x30;
  if (result == 0x10) encoderDelta++;
  else if (result == 0x20) encoderDelta--;
}

struct Button {
  uint8_t pin;
  bool currentState;
  bool lastState;
  unsigned long lastDebounceTime;
};
Button buttons[] = {
  { PIN_BTN1, HIGH, HIGH, 0 }, { PIN_BTN2, HIGH, HIGH, 0 },
  { PIN_BTN3, HIGH, HIGH, 0 }, { PIN_BTN4, HIGH, HIGH, 0 },
  { PIN_BTN5, HIGH, HIGH, 0 }, { PIN_BTN6, HIGH, HIGH, 0 },
  { PIN_BTN7, HIGH, HIGH, 0 }, { PIN_BTN8, HIGH, HIGH, 0 },
  { PIN_BTN_MODE, HIGH, HIGH, 0 }, { PIN_ENC_SW, HIGH, HIGH, 0 }
};

char toastMessage[24] = "";
unsigned long toastExpiry = 0;
bool screenDirty = true;
const unsigned long DEBOUNCE_TIME = 35;
unsigned long modeBtnPressStart = 0;

// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(PIN_LED_BT, OUTPUT);
  pinMode(PIN_LED_WIFI, OUTPUT);
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) pinMode(buttons[i].pin, INPUT_PULLUP);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrEncoder, CHANGE);
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.display();

  delay(10);
  // Enter Setup Mode if Mode button is held down during boot
  if (digitalRead(PIN_BTN_MODE) == LOW) {
    inSetupMode = true;
  }

  loadConfiguration();

  // Only enter setup mode if Mode button is held on boot, or if completely brand new
  if (digitalRead(PIN_BTN_MODE) == LOW) {
    inSetupMode = true;
  } else if (!isDeviceConfigured()) {
    inSetupMode = true;
  } else {
    inSetupMode = false;
  }

  if (inSetupMode) {
    emitSetupStatus();
  }

  // Single factory MAC for all hosts; one BLE name ("BertoPad") forever.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  bleKeyboard.begin();

  WiFi.mode(WIFI_STA);
  if (stored_ssid.length() > 0) {
    WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
  }

  tvMutex = xSemaphoreCreateMutex();
  vizioQueue = xQueueCreate(20, sizeof(VizioCommand));
  xTaskCreatePinnedToCore(vizioWorkerTask, "VizioWorker", 8192, NULL, 1, NULL, 0);
  setupWebServer();

  char readyMsg[24];
  snprintf(readyMsg, sizeof(readyMsg), "%s Ready", dev_names[currentBleSlot].c_str());
  showToast(readyMsg, 800);
}

void loop() {
  server.handleClient();
  g_lastActivity = millis();

  // Web Serial listener for index.html / web app over USB
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    g_lastActivity = millis();
    if (line.length() > MAX_REQUEST_BODY) {
      Serial.println("{\"ok\":false,\"message\":\"Request too large.\"}");
    } else if (line.length() > 0 && line.startsWith("{")) {
      processSerialPacket(line);
    }
  }

  const bool wifi = WiFi.status() == WL_CONNECTED;
  digitalWrite(PIN_LED_BT, bleKeyboard.isConnected() ? HIGH : LOW);
  digitalWrite(PIN_LED_WIFI, wifi ? HIGH : LOW);
  static bool lastBle = false, lastWifi = false;
  const bool ble = bleKeyboard.isConnected();
  if (ble != lastBle || wifi != lastWifi) {
    lastBle = ble;
    lastWifi = wifi;
    g_lastActivity = millis();
    emitEvent(String("{\"event\":\"connection\",\"ble\":") + (ble ? "true" : "false") + ",\"wifi\":" + (wifi ? "true" : "false") + "}");
    // Remember the bonded peer address for the currently active host slot
    if (ble) {
      NimBLEServer* pServer = NimBLEDevice::getServer();
      if (pServer) {
        auto peers = pServer->getPeerDevices();
        if (!peers.empty()) {
          auto connInfo = pServer->getPeerInfoByHandle(peers[0]);
          saveHostBondAddr(currentBleSlot, connInfo.getAddress());
        }
      }
    }
  }
  if (wifi && !mdnsStarted) mdnsStarted = MDNS.begin("macropad");

  if (encoderDelta != 0) {
    g_lastActivity = millis();
    noInterrupts();
    int delta = encoderDelta;
    encoderDelta = 0;
    interrupts();

    // Mode button held while rotating dial: cycle Device 1 -> 2 -> 3
    if (digitalRead(PIN_BTN_MODE) == LOW) {
      modeBtnPressStart = 0; // Prevent entering setup portal on release
      int newSlot = (currentBleSlot + (delta > 0 ? 1 : (MAX_BLE_SLOTS - 1))) % MAX_BLE_SLOTS;
      switchBleDevice(newSlot);
    } else {
      int steps = min(abs(delta), 8);
      for (int s = 0; s < steps; ++s) handleEncoder(delta > 0 ? 1 : -1);
    }
  }

  unsigned long now = millis();
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
    int reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastState) buttons[i].lastDebounceTime = now;
    if (now - buttons[i].lastDebounceTime > DEBOUNCE_TIME && reading != buttons[i].currentState) {
      buttons[i].currentState = reading;
      if (buttons[i].pin == PIN_BTN_MODE) {
        if (reading == LOW) {
          modeBtnPressStart = now;  // short press fires on release
          g_lastActivity = millis();
        } else if (modeBtnPressStart > 0) {
          modeBtnPressStart = 0;
          handleButtonPress(PIN_BTN_MODE);
        }
      } else {
        int h = buttonToHostIndex(buttons[i].pin);
        if (h >= 0) {
          if (reading == LOW) {
            hostBtnPressStart[h] = now;  // host button pressed — start timer
            g_lastActivity = millis();
          } else {
            // Host button released
            if (hostBtnPressStart[h] > 0) {
              hostBtnPressStart[h] = 0;
              handleButtonPress(buttons[i].pin);  // short press → execute action
            }
          }
        } else if (reading == LOW) {
          handleButtonPress(buttons[i].pin);  // non-host buttons: immediate action
          g_lastActivity = millis();
        }
      }
    }
    buttons[i].lastState = reading;
  }

  // Host button long-press (≥1 s): clear ONLY that host's bond
  for (int h = 0; h < 3; ++h) {
    if (hostBtnPressStart[h] > 0 && now - hostBtnPressStart[h] >= 1000) {
      clearHostBond(h);
      char msg[24];
      snprintf(msg, sizeof(msg), "Host %d cleared", h + 1);
      showToast(msg, 1000);
      screenDirty = true;
      hostBtnPressStart[h] = 0;  // prevent repeat
    }
  }

  // Mode + Host button held 5 s: hardware Forget for that slot (no web app needed)
  static unsigned long modeAndHostStart = 0;
  static int modeAndHostIdx = -1;
  bool modeHeld = (modeBtnPressStart > 0);
  int hostHeld = -1;
  for (int h = 0; h < 3; ++h) if (hostBtnPressStart[h] > 0) { hostHeld = h; break; }
  if (modeHeld && hostHeld >= 0) {
    if (modeAndHostStart == 0 || modeAndHostIdx != hostHeld) {
      modeAndHostStart = now;
      modeAndHostIdx = hostHeld;
    } else if (now - modeAndHostStart >= 5000) {
      clearHostBond(hostHeld);
      resetProfile(String("HOST") + String(hostHeld + 1));
      char msg[24];
      snprintf(msg, sizeof(msg), "Host %d forgotten", hostHeld + 1);
      showToast(msg, 1000);
      screenDirty = true;
      modeAndHostStart = 0;
      modeAndHostIdx = -1;
      modeBtnPressStart = 0;
      hostBtnPressStart[hostHeld] = 0;
    }
  } else {
    modeAndHostStart = 0;
    modeAndHostIdx = -1;
  }

  if (modeBtnPressStart > 0 && now - modeBtnPressStart > 3000) {
    startSetupPortal();
    modeBtnPressStart = 0;
    screenDirty = true;
  }
  if (toastExpiry > 0 && now >= toastExpiry) {
    toastExpiry = 0;
    screenDirty = true;
  }
  if (screenDirty) renderScreen();

  // --- Step 5: Display dim / off / light sleep ---
  if (!g_sleeping) {
    unsigned long idle = now - g_lastActivity;
    if (idle >= g_offMs) {
      // Turn off display, keep BT LED dim, enter light sleep
      g_displayState = DISPLAY_OFF;
      screenDirty = true;
      renderScreen();  // returns early since DISPLAY_OFF

      analogWrite(PIN_LED_BT, 40);   // dim BT LED (~15 %)
      digitalWrite(PIN_LED_WIFI, LOW);
      display.ssd1306_command(SSD1306_DISPLAYOFF);

      // Wake on any button or encoder pin (active LOW)
      uint64_t ext1_mask = 0;
      for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        ext1_mask |= (1ULL << buttons[i].pin);
      ext1_mask |= (1ULL << PIN_ENC_CLK) | (1ULL << PIN_ENC_DT);
      esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW);

      g_sleeping = true;
      esp_light_sleep_start();

      // --- Woke up from light sleep ---
      g_sleeping = false;
      g_lastActivity = millis();
      display.ssd1306_command(SSD1306_DISPLAYON);
      display.setContrast(255);
      g_displayState = DISPLAY_ON;
      screenDirty = true;
      digitalWrite(PIN_LED_BT, bleKeyboard.isConnected() ? HIGH : LOW);
      digitalWrite(PIN_LED_WIFI, wifi ? HIGH : LOW);
    } else if (idle >= g_dimMs) {
      if (g_displayState != DISPLAY_DIM) {
        g_displayState = DISPLAY_DIM;
        screenDirty = true;
      }
    } else if (g_displayState != DISPLAY_ON) {
      g_displayState = DISPLAY_ON;
      screenDirty = true;
    }
  }

  delay(1);
}
