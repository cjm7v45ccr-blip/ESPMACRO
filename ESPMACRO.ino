#include <esp_system.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Update.h>
#include "mbedtls/base64.h"

// -----------------------------------------------------------------------------
// Lightweight NimBLE Engine
// -----------------------------------------------------------------------------
#define USE_NIMBLE
#include <NimBLEDevice.h>
#include <BleKeyboard.h>

#include "config.h"
#include "bitmaps.h"

// Firmware Versioning & Defaults
// -----------------------------------------------------------------------------
#ifndef API_VERSION
#define API_VERSION 2
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "4"
#endif
#ifndef SETUP_AP_PREFIX
#define SETUP_AP_PREFIX "MacroPad"
#endif

#define MAX_BLE_SLOTS 3

// -----------------------------------------------------------------------------
// Hardware Slot Definitions (11 Slots per Device Profile)
// -----------------------------------------------------------------------------
const char* const SLOT_IDS[11] = { 
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "dial", "enc_cw", "enc_ccw" 
};

// All Bluetooth slots start blank out of the box until mapped on the website
const char* const DEFAULT_BLANK_BINDINGS[11] = {
  "", "", "", "", "", "", "", "", "", "", ""
};

// Default TV Controls (Wi-Fi Vizio)
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

enum PadMode { MODE_BLE, MODE_TV };

String stored_ssid = "";
String stored_pass = "";
String stored_tv_ip = DEFAULT_TV_IP;
String stored_tv_auth = DEFAULT_TV_AUTH;
String api_token = "";
uint32_t config_rev = 0;      
bool events_enabled = false;  
int stored_tv_port = DEFAULT_TV_PORT;
int pairingReqToken = 0;

// 3 Separate Profiles for Device 1, Device 2, Device 3 + TV Profile
String dev_names[3] = { "Device 1", "Device 2", "Device 3" };
String dev_bindings[3][11];
String tv_name = "TV";
String tv_bindings[11];

PadMode currentMode = MODE_BLE;
uint8_t currentBleSlot = 0; // 0 = Device 1, 1 = Device 2, 2 = Device 3
String bleDeviceName = "Studio Pad 1";

bool inSetupMode = false;
bool setup_done = false;
bool mdnsStarted = false;

// OTA Stream Flash Tracking
size_t otaTotalSize = 0;
size_t otaBytesWritten = 0;
String setupSsid = "";
String setupPass = "";

Preferences prefs;
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BleKeyboard bleKeyboard("Studio Pad 1", "Studio MacroPad", 100);
WiFiClientSecure tvClient;
unsigned long lastTvKeepAlive = 0;

// Asynchronous TV Command Queue
enum CommandType { CMD_KEY, CMD_APP };
struct VizioCommand {
  CommandType type;
  int codeset;
  int code;
  char appId[8];
  int nameSpace;
  char message[64];
};
QueueHandle_t vizioQueue;

char toastMessage[24] = "";
unsigned long toastExpiry = 0;
bool screenDirty = true;
const unsigned long DEBOUNCE_TIME = 35;
unsigned long modeBtnPressStart = 0;

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

// -----------------------------------------------------------------------------
// Logitech M720-Style Virtual MAC & Multi-Host Switching
// -----------------------------------------------------------------------------
void setSlotMacAddress(uint8_t slot) {
  if (slot == 0) return; // Slot 0 uses default factory eFuse MAC
  uint64_t efuse = ESP.getEfuseMac();
  uint8_t baseMac[6];
  baseMac[0] = (uint8_t)(efuse);
  baseMac[1] = (uint8_t)(efuse >> 8);
  baseMac[2] = (uint8_t)(efuse >> 16);
  baseMac[3] = (uint8_t)(efuse >> 24);
  baseMac[4] = (uint8_t)(efuse >> 32);
  baseMac[5] = (uint8_t)((efuse >> 40) + (slot * 4));
  baseMac[0] &= ~0x01; // Ensure unicast
  esp_base_mac_addr_set(baseMac);
}

void renderScreen();

void switchBleDevice(uint8_t newSlot) {
  if (newSlot >= MAX_BLE_SLOTS) return;
  if (newSlot == currentBleSlot) {
    char msg[24];
    snprintf(msg, sizeof(msg), "%s Active", dev_names[currentBleSlot].c_str());
    strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
    toastExpiry = millis() + 700;
    renderScreen();
    return;
  }

  prefs.begin("config", false);
  prefs.putUChar("ble_slot", newSlot);
  prefs.end();

  char msg[24];
  snprintf(msg, sizeof(msg), "%s...", dev_names[newSlot].c_str());
  strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
  toastExpiry = millis() + 1500;
  renderScreen();
  delay(350);
  ESP.restart(); // Reboots into the new Bluetooth identity & profile
}

// -----------------------------------------------------------------------------
// Bounded JSON Helpers
// -----------------------------------------------------------------------------
const size_t MAX_REQUEST_BODY = 4096;

String jsonQuote(const String& value) {
  String result = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\\' || c == '\"') { result += '\\'; result += c; }
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if ((uint8_t)c >= 0x20) result += c;
  }
  result += "\"";
  return result;
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Pad-Token");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

void sendJson(int status, const String& payload) {
  addCorsHeaders();
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", payload);
}

void sendError(int status, const char* code, const char* message) {
  String payload = "{\"ok\":false,\"error\":\"" + String(code) +
                   "\",\"message\":" + jsonQuote(message) + "}";
  sendJson(status, payload);
}

bool requireAuth() {
  if (api_token.length() == 0) { sendError(403, "setup_required", "Set an API token over USB before using the HTTP API."); return false; }
  if (server.header("X-Pad-Token") != api_token) { sendError(401, "unauthorized", "Missing or invalid API token."); return false; }
  return true;
}

bool requestBody(String& body) {
  if (!requireAuth()) return false;
  if (!server.hasArg("plain")) { sendError(400, "missing_body", "A JSON request body is required."); return false; }
  body = server.arg("plain");
  if (body.length() == 0 || body.length() > MAX_REQUEST_BODY) {
    sendError(413, "invalid_body", "The request body is empty or too large.");
    return false;
  }
  return true;
}

bool jsonString(const String& body, const char* key, String& out, size_t maxLength) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) return false;
  int colon = body.indexOf(':', keyPos + needle.length());
  if (colon < 0) return false;
  int pos = colon + 1;
  while (pos < (int)body.length() && isspace((unsigned char)body[pos])) ++pos;
  if (pos >= (int)body.length() || body[pos] != '\"') return false;
  ++pos;
  String value = "";
  bool escaped = false;
  for (; pos < (int)body.length(); ++pos) {
    char c = body[pos];
    if (escaped) {
      if (c != '\\' && c != '\"' && c != '/' && c != 'n' && c != 'r' && c != 't') return false;
      if (c == 'n') value += '\n';
      else if (c == 'r') value += '\r';
      else if (c == 't') value += '\t';
      else value += c;
      escaped = false;
      continue;
    }
    if (c == '\\') { escaped = true; continue; }
    if (c == '\"') { out = value; return value.length() <= maxLength; }
    if ((uint8_t)c < 0x20 || value.length() >= maxLength) return false;
    value += c;
  }
  return false;
}

bool jsonInt(const String& body, const char* key, int& out) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) return false;
  int colon = body.indexOf(':', keyPos + needle.length());
  if (colon < 0) return false;
  int pos = colon + 1;
  while (pos < (int)body.length() && isspace((unsigned char)body[pos])) ++pos;
  bool quoted = pos < (int)body.length() && body[pos] == '\"';
  if (quoted) ++pos;
  int start = pos;
  while (pos < (int)body.length() && isdigit((unsigned char)body[pos])) ++pos;
  if (start == pos || (quoted && (pos >= (int)body.length() || body[pos] != '\"'))) return false;
  out = body.substring(start, pos).toInt();
  return true;
}

bool isSupportedAction(const String& action) {
  if (action == "" || action == "NONE") return true; // Unassigned is valid
  if (action.length() < 3 || action.length() > 512) return false;

  // Validate multi-step macros recursively
  if (action.indexOf("||") >= 0) {
    int start = 0;
    while (start < (int)action.length()) {
      int idx = action.indexOf("||", start);
      String sub = (idx >= 0) ? action.substring(start, idx) : action.substring(start);
      sub.trim();
      if (!isSupportedAction(sub)) return false;
      if (idx < 0) break;
      start = idx + 2;
    }
    return true;
  }

  for (size_t i = 0; i < SUPPORTED_ACTION_COUNT; ++i) if (action == SUPPORTED_ACTIONS[i]) return true;
  if (action.startsWith("SHORTCUT:")) {
    int firstColon = action.indexOf(':');
    int secondColon = action.indexOf(':', firstColon + 1);
    return (secondColon > firstColon && action.length() > (size_t)(secondColon + 1));
  }
  if (action.startsWith("TVKEY:")) {
    int firstColon = action.indexOf(':');
    int secondColon = action.indexOf(':', firstColon + 1);
    return (secondColon > firstColon && action.length() > (size_t)(secondColon + 1));
  }
  if (action.startsWith("TVAPP:")) {
    int firstColon = action.indexOf(':');
    int secondColon = action.indexOf(':', firstColon + 1);
    return (secondColon > firstColon && action.length() > (size_t)(secondColon + 1));
  }
  if (action.startsWith("TEXT:") && action.length() > 5) return true;
  return false;
}

bool jsonHasKey(const String& body, const char* key) {
  return body.indexOf("\"" + String(key) + "\"") >= 0;
}

int slotIndex(const String& slot) {
  for (int i = 0; i < 11; ++i) if (slot == SLOT_IDS[i]) return i;
  return -1;
}

// Routes profile IDs to the appropriate device slot index
int profileToHostIndex(const String& profile) {
  if (profile == "HOST1" || profile == "H1" || profile == "DEV1" || profile == "D1") return 0;
  if (profile == "HOST2" || profile == "H2" || profile == "DEV2" || profile == "D2") return 1;
  if (profile == "HOST3" || profile == "H3" || profile == "DEV3" || profile == "D3") return 2;
  return currentBleSlot; // "MAC" maps to the currently active device
}

bool validProfile(const String& profile) { 
  return profile == "MAC" || profile == "TV" || 
         profile == "HOST1" || profile == "HOST2" || profile == "HOST3" ||
         profile == "H1" || profile == "H2" || profile == "H3" ||
         profile == "DEV1" || profile == "DEV2" || profile == "DEV3"; 
}

bool validAlias(const String& value) {
  if (value.length() < 1 || value.length() > 20) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (!(isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')) return false;
  }
  return value[0] != ' ' && value[value.length() - 1] != ' ';
}

bool validHost(const String& value) {
  if (value.length() < 1 || value.length() > 63) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
  }
  return true;
}

bool validSsid(const String& value) { return value.length() > 0 && value.length() <= 32; }
bool validPass(const String& value) { return value.length() <= 63; }

bool validPin(const String& value) {
  if (value.length() < 4 || value.length() > 8) return false;
  for (size_t i = 0; i < value.length(); ++i) if (!isdigit((unsigned char)value[i])) return false;
  return true;
}

String& profileName(const String& profile) { 
  if (profile == "TV") return tv_name;
  return dev_names[profileToHostIndex(profile)];
}

String* profileBindings(const String& profile) { 
  if (profile == "TV") return tv_bindings;
  return dev_bindings[profileToHostIndex(profile)];
}

const char* profilePrefix(const String& profile) { 
  if (profile == "TV") return "t_";
  int idx = profileToHostIndex(profile);
  if (idx == 0) return "d1_";
  if (idx == 1) return "d2_";
  return "d3_";
}

String deviceId() {
  char buf[24];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(buf, sizeof(buf), "studio-pad-%02x%02x%02x", (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  return String(buf);
}

void emitEvent(const String& json) {
  if (events_enabled) Serial.println(json);
}

void bumpRev() {
  config_rev++;
  prefs.begin("config", false); prefs.putUInt("rev", config_rev); prefs.end();
  emitEvent("{\"event\":\"config\",\"rev\":" + String(config_rev) + "}");
}

void saveProfile(const String& profile) {
  String* bindings = profileBindings(profile);
  prefs.begin("matrix", false);
  prefs.putString((String(profilePrefix(profile)) + "name").c_str(), profileName(profile));
  for (int i = 0; i < 11; ++i) prefs.putString((String(profilePrefix(profile)) + SLOT_IDS[i]).c_str(), bindings[i]);
  prefs.end();
  bumpRev();
}

void saveBinding(const String& profile, int index, const String& action) {
  String* bindings = profileBindings(profile);
  bindings[index] = action;
  prefs.begin("matrix", false);
  prefs.putString((String(profilePrefix(profile)) + SLOT_IDS[index]).c_str(), action);
  prefs.end();
  bumpRev();
}

void resetProfile(const String& profile) {
  String* bindings = profileBindings(profile);
  const char* const* defaults = (profile == "TV" ? DEFAULT_TV_BINDINGS : DEFAULT_BLANK_BINDINGS);
  for (int i = 0; i < 11; ++i) bindings[i] = defaults[i];
  if (profile == "TV") {
    tv_name = "TV";
  } else {
    int idx = profileToHostIndex(profile);
    dev_names[idx] = String("Device ") + String(idx + 1);
  }
  saveProfile(profile);
}

bool tvIsConfigured() { return stored_tv_ip.length() > 0 || stored_tv_auth.length() > 0; }

// -----------------------------------------------------------------------------
// Low-Latency BLE Keystrokes & Media Controls
// -----------------------------------------------------------------------------
void showToast(const char* msg, unsigned long durationMs = 1000) {
  strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
  toastMessage[sizeof(toastMessage) - 1] = '\0';
  toastExpiry = millis() + durationMs;
  screenDirty = true;
}

void setActiveMode(PadMode mode) {
  currentMode = mode;
  prefs.begin("config", false); prefs.putUChar("mode", mode == MODE_TV ? 1 : 0); prefs.end();
  String toast = (mode == MODE_BLE ? dev_names[currentBleSlot] : "Profile: TV");
  showToast(toast.c_str(), 800);
  screenDirty = true;
  emitEvent(String("{\"event\":\"profile\",\"active\":\"") + (mode == MODE_BLE ? "MAC" : "TV") + "\"}");
}

void sendBleMedia(const MediaKeyReport key) {
  bleKeyboard.press(key); delay(10); bleKeyboard.release(key);
}

void sendBleKey(uint8_t key) {
  bleKeyboard.press(key); delay(10); bleKeyboard.release(key);
}

// -----------------------------------------------------------------------------
// Vizio HTTPS Direct Connection Engine
// -----------------------------------------------------------------------------
SemaphoreHandle_t tvMutex = nullptr;

String sendVizioRequestUnlocked(const char* method, const String& path, const String& payload, int portOverride = 0) {
  if (WiFi.status() != WL_CONNECTED || !validHost(stored_tv_ip)) return "";
  const int portToUse = portOverride > 0 ? portOverride : stored_tv_port;
  if (!tvClient.connected() || millis() - lastTvKeepAlive > 15000) {
    tvClient.stop();
    tvClient.setInsecure();
    tvClient.setTimeout(3000);
    if (!tvClient.connect(stored_tv_ip.c_str(), portToUse)) return "";
  }
  lastTvKeepAlive = millis();
  tvClient.printf("%s %s HTTP/1.1\r\n", method, path.c_str());
  tvClient.printf("Host: %s:%d\r\n", stored_tv_ip.c_str(), portToUse);
  tvClient.println("Content-Type: application/json");
  tvClient.printf("Content-Length: %d\r\n", payload.length());
  if (stored_tv_auth.length() > 0) tvClient.printf("AUTH: %s\r\n", stored_tv_auth.c_str());
  tvClient.println("Connection: keep-alive\r\n");
  if (payload.length() > 0) tvClient.print(payload);

  String response = "";
  unsigned long waitStart = millis();
  int headerEnd = -1, contentLength = -1;
  while (tvClient.connected() && millis() - waitStart < 800 && response.length() < 2048) {
    while (tvClient.available() && response.length() < 2048) {
      response += (char)tvClient.read();
      waitStart = millis();
    }
    if (headerEnd < 0) {
      int end = response.indexOf("\r\n\r\n");
      if (end >= 0) {
        headerEnd = end + 4;
        String head = response.substring(0, headerEnd);
        head.toLowerCase();
        int cl = head.indexOf("content-length:");
        if (cl >= 0) contentLength = head.substring(cl + 15).toInt();
      }
    }
    if (headerEnd >= 0 && contentLength >= 0 && (int)response.length() >= headerEnd + contentLength) break;
    delay(1);
  }
  return response;
}

String sendVizioRequestRaw(const char* method, const String& path, const String& payload, int portOverride = 0) {
  if (!tvMutex || xSemaphoreTake(tvMutex, pdMS_TO_TICKS(4000)) != pdTRUE) return "";
  String response = sendVizioRequestUnlocked(method, path, payload, portOverride);
  xSemaphoreGive(tvMutex);
  return response;
}

void vizioWorkerTask(void*) {
  VizioCommand cmd;
  for (;;) {
    if (xQueueReceive(vizioQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;
    if (cmd.type == CMD_KEY) {
      char body[128];
      snprintf(body, sizeof(body), "{\"KEYLIST\":[{\"CODESET\":%d,\"CODE\":%d,\"ACTION\":\"KEYPRESS\"}]}", cmd.codeset, cmd.code);
      sendVizioRequestRaw("PUT", "/key_command/", body);
    } else {
      char body[256];
      snprintf(body, sizeof(body), "{\"VALUE\":{\"APP_ID\":\"%s\",\"NAME_SPACE\":%d,\"MESSAGE\":\"%s\"}}", cmd.appId, cmd.nameSpace, cmd.message);
      sendVizioRequestRaw("PUT", "/app/launch", body);
    }
  }
}

bool dispatchKey(int codeset, int code) {
  VizioCommand cmd = {};
  cmd.type = CMD_KEY; cmd.codeset = codeset; cmd.code = code;
  return xQueueSend(vizioQueue, &cmd, 0) == pdTRUE;
}

bool dispatchApp(const char* appId, int nameSpace, const char* message) {
  VizioCommand cmd = {};
  cmd.type = CMD_APP; cmd.nameSpace = nameSpace;
  snprintf(cmd.appId, sizeof(cmd.appId), "%s", appId);
  snprintf(cmd.message, sizeof(cmd.message), "%s", message);
  return xQueueSend(vizioQueue, &cmd, 0) == pdTRUE;
}

void pressModifier(const String& mod) {
  if (mod == "ctrl") bleKeyboard.press(KEY_LEFT_CTRL);
  else if (mod == "shift") bleKeyboard.press(KEY_LEFT_SHIFT);
  else if (mod == "alt" || mod == "opt") bleKeyboard.press(KEY_LEFT_ALT);
  else if (mod == "gui" || mod == "cmd" || mod == "win") bleKeyboard.press(KEY_LEFT_GUI);
}

void pressKey(const String& k) {
  if (k.length() == 1) {
    bleKeyboard.press((uint8_t)k[0]);
  } else if (k == "enter" || k == "return") {
    bleKeyboard.press(KEY_RETURN);
  } else if (k == "esc" || k == "escape") {
    bleKeyboard.press(KEY_ESC);
  } else if (k == "tab") {
    bleKeyboard.press(KEY_TAB);
  } else if (k == "space") {
    bleKeyboard.press(' ');
  } else if (k == "backspace") {
    bleKeyboard.press(KEY_BACKSPACE);
  } else if (k == "delete" || k == "del") {
    bleKeyboard.press(KEY_DELETE);
  } else if (k == "insert") {
    bleKeyboard.press(KEY_INSERT);
  } else if (k == "home") {
    bleKeyboard.press(KEY_HOME);
  } else if (k == "end") {
    bleKeyboard.press(KEY_END);
  } else if (k == "pageup" || k == "pgup") {
    bleKeyboard.press(KEY_PAGE_UP);
  } else if (k == "pagedown" || k == "pgdn") {
    bleKeyboard.press(KEY_PAGE_DOWN);
  } else if (k == "up") {
    bleKeyboard.press(KEY_UP_ARROW);
  } else if (k == "down") {
    bleKeyboard.press(KEY_DOWN_ARROW);
  } else if (k == "left") {
    bleKeyboard.press(KEY_LEFT_ARROW);
  } else if (k == "right") {
    bleKeyboard.press(KEY_RIGHT_ARROW);
  } else if (k == "capslock") {
    bleKeyboard.press(KEY_CAPS_LOCK);
  } else if (k.startsWith("f") && k.length() >= 2) {
    int fnum = k.substring(1).toInt();
    if (fnum >= 1 && fnum <= 12) bleKeyboard.press(KEY_F1 + (fnum - 1));
    else if (fnum >= 13 && fnum <= 24) bleKeyboard.press(KEY_F13 + (fnum - 13));
  }
}

bool executeShortcut(const String& act) {
  int firstColon = act.indexOf(':');
  int secondColon = act.indexOf(':', firstColon + 1);
  if (secondColon < 0) return false;
  String mods = act.substring(firstColon + 1, secondColon);
  String key = act.substring(secondColon + 1);
  mods.toLowerCase();
  key.toLowerCase();
  int start = 0;
  while (start < (int)mods.length()) {
    int comma = mods.indexOf(',', start);
    String mod = (comma >= 0) ? mods.substring(start, comma) : mods.substring(start);
    mod.trim();
    if (mod.length() > 0) pressModifier(mod);
    if (comma < 0) break;
    start = comma + 1;
  }
  pressKey(key);
  delay(12);
  bleKeyboard.releaseAll();
  return true;
}

bool executeTvKey(const String& act) {
  int firstColon = act.indexOf(':');
  int secondColon = act.indexOf(':', firstColon + 1);
  if (secondColon < 0) return false;
  int codeset = act.substring(firstColon + 1, secondColon).toInt();
  int code = act.substring(secondColon + 1).toInt();
  return dispatchKey(codeset, code);
}

bool executeTvApp(const String& act) {
  int c1 = act.indexOf(':');
  int c2 = act.indexOf(':', c1 + 1);
  if (c2 < 0) return false;
  int c3 = act.indexOf(':', c2 + 1);
  String appId = act.substring(c1 + 1, c2);
  int nameSpace = (c3 >= 0 ? act.substring(c2 + 1, c3) : act.substring(c2 + 1)).toInt();
  String url = (c3 >= 0) ? act.substring(c3 + 1) : "";
  return dispatchApp(appId.c_str(), nameSpace, url.c_str());
}

bool executeText(const String& act) {
  String text = act.substring(5);
  bleKeyboard.print(text);
  return true;
}

bool isBleAction(const String& action) {
  return action.startsWith("MEDIA:") || action.startsWith("KEY:") || action.startsWith("MACRO:") ||
         action.startsWith("WIN:") || action.startsWith("SHORTCUT:") || action.startsWith("TEXT:") ||
         action.startsWith("BLE:");
}

bool isTvAction(const String& action) {
  return action.startsWith("TV:") || action.startsWith("APP:") ||
         action.startsWith("TVKEY:") || action.startsWith("TVAPP:");
}

bool executeAction(const String& act) {
  // If button is unassigned/blank, do nothing safely
  if (act == "" || act == "NONE") return true; 

  // --- Handle Multi-Step Macros (action1||action2||action3) ---
  if (act.indexOf("||") >= 0) {
    int start = 0;
    while (start < (int)act.length()) {
      int idx = act.indexOf("||", start);
      String sub = (idx >= 0) ? act.substring(start, idx) : act.substring(start);
      sub.trim();
      if (sub.length() > 0) {
        executeAction(sub);
        delay(50); // Pause between macro steps
      }
      if (idx < 0) break;
      start = idx + 2;
    }
    return true;
  }
  // ------------------------------------------------------------

  // Support for custom delay step if specified by web app (e.g., DELAY:200)
  if (act.startsWith("DELAY:")) {
    int ms = act.substring(6).toInt();
    if (ms > 0) delay(ms);
    return true;
  }

  // Host switching actions
  if (act == "BLE:HOST1") { switchBleDevice(0); return true; }
  else if (act == "BLE:HOST2") { switchBleDevice(1); return true; }
  else if (act == "BLE:HOST3") { switchBleDevice(2); return true; }
  else if (act == "BLE:NEXT_HOST") { switchBleDevice((currentBleSlot + 1) % MAX_BLE_SLOTS); return true; }

  if (isBleAction(act) && !bleKeyboard.isConnected()) { showToast("BLE Disconnected", 700); return false; }
  if (isTvAction(act) && (WiFi.status() != WL_CONNECTED || !tvIsConfigured())) { showToast("TV Unavailable", 700); return false; }

  if (act == "MEDIA:PLAY_PAUSE") { sendBleMedia(KEY_MEDIA_PLAY_PAUSE); showToast("Play/Pause"); }
  else if (act == "MEDIA:MUTE") { sendBleMedia(KEY_MEDIA_MUTE); showToast("Mute"); }
  else if (act == "MEDIA:VOL_UP") { sendBleMedia(KEY_MEDIA_VOLUME_UP); showToast("Vol +"); }
  else if (act == "MEDIA:VOL_DOWN") { sendBleMedia(KEY_MEDIA_VOLUME_DOWN); showToast("Vol -"); }
  else if (act == "MEDIA:NEXT") { sendBleMedia(KEY_MEDIA_NEXT_TRACK); showToast("Next Track"); }
  else if (act == "MEDIA:PREV") { sendBleMedia(KEY_MEDIA_PREVIOUS_TRACK); showToast("Prev Track"); }
  else if (act == "KEY:F13") { sendBleKey(KEY_F13); showToast("F13"); }
  else if (act == "KEY:F14") { sendBleKey(KEY_F14); showToast("F14"); }
  else if (act == "KEY:F15") { sendBleKey(KEY_F15); showToast("F15"); }
  else if (act == "KEY:F16") { sendBleKey(KEY_F16); showToast("F16"); }
  else if (act == "MACRO:COPY") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('c'); delay(10); bleKeyboard.releaseAll(); showToast("Copy"); }
  else if (act == "MACRO:PASTE") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('v'); delay(10); bleKeyboard.releaseAll(); showToast("Paste"); }
  else if (act == "MACRO:CUT") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('x'); delay(10); bleKeyboard.releaseAll(); showToast("Cut"); }
  else if (act == "MACRO:UNDO") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('z'); delay(10); bleKeyboard.releaseAll(); showToast("Undo"); }
  else if (act == "MACRO:REDO") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('z'); delay(10); bleKeyboard.releaseAll(); showToast("Redo"); }
  else if (act == "MACRO:SELECT_ALL") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('a'); delay(10); bleKeyboard.releaseAll(); showToast("Select All"); }
  else if (act == "MACRO:SAVE") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('s'); delay(10); bleKeyboard.releaseAll(); showToast("Save"); }
  else if (act == "MACRO:SPOTLIGHT") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(' '); delay(10); bleKeyboard.releaseAll(); showToast("Spotlight"); }
  else if (act == "MACRO:LOCK") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('q'); delay(10); bleKeyboard.releaseAll(); showToast("Lock Screen"); }
  else if (act == "MACRO:NEW_TAB") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('t'); delay(10); bleKeyboard.releaseAll(); showToast("New Tab"); }
  else if (act == "MACRO:CLOSE_TAB") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('w'); delay(10); bleKeyboard.releaseAll(); showToast("Close Tab"); }
  else if (act == "WIN:COPY") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('c'); delay(10); bleKeyboard.releaseAll(); showToast("Copy"); }
  else if (act == "WIN:PASTE") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('v'); delay(10); bleKeyboard.releaseAll(); showToast("Paste"); }
  else if (act == "WIN:CUT") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('x'); delay(10); bleKeyboard.releaseAll(); showToast("Cut"); }
  else if (act == "WIN:UNDO") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('z'); delay(10); bleKeyboard.releaseAll(); showToast("Undo"); }
  else if (act == "WIN:REDO") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('y'); delay(10); bleKeyboard.releaseAll(); showToast("Redo"); }
  else if (act == "WIN:SELECT_ALL") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('a'); delay(10); bleKeyboard.releaseAll(); showToast("Select All"); }
  else if (act == "WIN:SAVE") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('s'); delay(10); bleKeyboard.releaseAll(); showToast("Save"); }
  else if (act == "WIN:NEW_TAB") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('t'); delay(10); bleKeyboard.releaseAll(); showToast("New Tab"); }
  else if (act == "WIN:CLOSE_TAB") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press('w'); delay(10); bleKeyboard.releaseAll(); showToast("Close Tab"); }
  else if (act == "WIN:LOCK") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('l'); delay(10); bleKeyboard.releaseAll(); showToast("Lock PC"); }
  else if (act == "WIN:SEARCH") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('s'); delay(10); bleKeyboard.releaseAll(); showToast("Win Search"); }
  else if (act == "WIN:EXPLORER") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('e'); delay(10); bleKeyboard.releaseAll(); showToast("Explorer"); }
  else if (act == "WIN:TASK_MGR") { bleKeyboard.press(KEY_LEFT_CTRL); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press(KEY_ESC); delay(10); bleKeyboard.releaseAll(); showToast("Task Mgr"); }
  else if (act == "WIN:SNIP") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press(KEY_LEFT_SHIFT); bleKeyboard.press('s'); delay(10); bleKeyboard.releaseAll(); showToast("Snipping"); }
  else if (act == "WIN:DESKTOP") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('d'); delay(10); bleKeyboard.releaseAll(); showToast("Desktop"); }
  else if (act == "WIN:RUN") { bleKeyboard.press(KEY_LEFT_GUI); bleKeyboard.press('r'); delay(10); bleKeyboard.releaseAll(); showToast("Run Dialog"); }
  else if (act == "TV:POWER") { if (!dispatchKey(11, 2)) return false; showToast("TV Power"); }
  else if (act == "TV:VOL_UP") { if (!dispatchKey(5, 1)) return false; showToast("TV Vol +"); }
  else if (act == "TV:VOL_DOWN") { if (!dispatchKey(5, 0)) return false; showToast("TV Vol -"); }
  else if (act == "TV:MUTE") { if (!dispatchKey(5, 4)) return false; showToast("TV Mute"); }
  else if (act == "TV:HOME") { if (!dispatchKey(4, 3)) return false; showToast("TV Home"); }
  else if (act == "TV:BACK") { if (!dispatchKey(4, 0)) return false; showToast("TV Back"); }
  else if (act == "TV:OK") { if (!dispatchKey(3, 2)) return false; showToast("TV OK"); }
  else if (act == "TV:INPUT") { if (!dispatchKey(7, 1)) return false; showToast("TV Input"); }
  else if (act == "TV:UP") { if (!dispatchKey(3, 8)) return false; showToast("TV Up"); }
  else if (act == "TV:DOWN") { if (!dispatchKey(3, 0)) return false; showToast("TV Down"); }
  else if (act == "TV:LEFT") { if (!dispatchKey(3, 1)) return false; showToast("TV Left"); }
  else if (act == "TV:RIGHT") { if (!dispatchKey(3, 7)) return false; showToast("TV Right"); }
  else if (act == "TV:EXIT") { if (!dispatchKey(9, 0)) return false; showToast("TV Exit"); }
  else if (act == "TV:MENU") { if (!dispatchKey(4, 8)) return false; showToast("TV Menu"); }
  else if (act == "TV:INFO") { if (!dispatchKey(4, 15)) return false; showToast("TV Info"); }
  else if (act == "TV:GUIDE") { if (!dispatchKey(4, 12)) return false; showToast("TV Guide"); }
  else if (act == "TV:CC") { if (!dispatchKey(4, 4)) return false; showToast("TV Captions"); }
  else if (act == "TV:CH_UP") { if (!dispatchKey(8, 1)) return false; showToast("TV Ch +"); }
  else if (act == "TV:CH_DOWN") { if (!dispatchKey(8, 0)) return false; showToast("TV Ch -"); }
  else if (act == "TV:PREV_CH") { if (!dispatchKey(8, 2)) return false; showToast("TV Prev Ch"); }
  else if (act == "TV:PLAY") { if (!dispatchKey(2, 3)) return false; showToast("TV Play"); }
  else if (act == "TV:PAUSE") { if (!dispatchKey(2, 2)) return false; showToast("TV Pause"); }
  else if (act == "TV:STOP") { if (!dispatchKey(2, 9)) return false; showToast("TV Stop"); }
  else if (act == "TV:REWIND") { if (!dispatchKey(2, 1)) return false; showToast("TV Rewind"); }
  else if (act == "TV:FFWD") { if (!dispatchKey(2, 0)) return false; showToast("TV Fast Fwd"); }
  else if (act == "APP:YOUTUBE") { if (!dispatchApp("1", 5, "https://www.youtube.com/tv")) return false; showToast("YouTube"); }
  else if (act == "APP:DISNEY") { if (!dispatchApp("75", 4, "https://cd-dmgz.bamgrid.com/bbd/vizio_tv/index.html")) return false; showToast("Disney+"); }
  else if (act == "APP:TUBI") { if (!dispatchApp("4", 3, "")) return false; showToast("Tubi"); }
  else if (act == "APP:NETFLIX") { if (!dispatchApp("2", 5, "")) return false; showToast("Netflix"); }
  else if (act == "APP:PRIME") { if (!dispatchApp("8", 4, "")) return false; showToast("Prime Video"); }
  else if (act == "APP:APPLETV") { if (!dispatchApp("128", 4, "")) return false; showToast("Apple TV+"); }
  else if (act.startsWith("SHORTCUT:")) { if (!executeShortcut(act)) return false; showToast("Shortcut"); }
  else if (act.startsWith("TEXT:")) { if (!executeText(act)) return false; showToast("Typed Text"); }
  else if (act.startsWith("TVKEY:")) { if (!executeTvKey(act)) return false; showToast("TV Command"); }
  else if (act.startsWith("TVAPP:")) { if (!executeTvApp(act)) return false; showToast("TV App"); }
  else return false;
  return true;
}

// -----------------------------------------------------------------------------
// Unified Data Model & Status Builders
// -----------------------------------------------------------------------------
String profileJson(const String& id) {
  String* bindings = profileBindings(id);
  String json = "{\"id\":" + jsonQuote(id) + ",\"name\":" + jsonQuote(profileName(id)) + ",\"controls\":{";
  for (int i = 0; i < 11; ++i) {
    if (i) json += ',';
    json += jsonQuote(SLOT_IDS[i]) + ":" + jsonQuote(bindings[i]);
  }
  return json + "}}";
}

String buildStatusJson() {
  const bool wifi = WiFi.status() == WL_CONNECTED;
  String json = "{\"ok\":true,\"setup_mode\":" + String(inSetupMode ? "true" : "false") + 
                ",\"setup_done\":" + String(setup_done ? "true" : "false") + 
                ",\"setupMode\":" + String(inSetupMode ? "true" : "false") + 
                ",\"api_version\":" + String(API_VERSION) + ",\"rev\":" + String(config_rev) + ",";
  json += "\"device\":{\"id\":" + jsonQuote(deviceId()) + ",\"name\":\"Studio MacroPad\",\"firmware\":\"" FIRMWARE_VERSION "\",\"connection_type\":" + jsonQuote(wifi ? "USB + Wi-Fi" : "USB") + "},";
  json += "\"connection\":{\"wifi_connected\":" + String(wifi ? "true" : "false") + ",\"ip\":" + jsonQuote(wifi ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + ",\"ble_connected\":" + String(bleKeyboard.isConnected() ? "true" : "false") + "},";
  json += "\"ble\":{\"active_host\":" + String(currentBleSlot + 1) + ",\"max_hosts\":" + String(MAX_BLE_SLOTS) + ",\"device_name\":" + jsonQuote(bleDeviceName) + "},";
  json += "\"diagnostics\":{\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"http_api_enabled\":" + String(api_token.length() > 0 ? "true" : "false") + ",\"events_enabled\":" + String(events_enabled ? "true" : "false") + "},";
  json += "\"tv\":{\"configured\":" + String(tvIsConfigured() ? "true" : "false") + ",\"paired\":" + String(stored_tv_auth.length() > 0 ? "true" : "false") + ",\"host\":" + jsonQuote(stored_tv_ip) + ",\"port\":" + String(stored_tv_port) + "},";
  
  // Returns all 4 profiles (Host 1, Host 2, Host 3, TV)
  json += "\"profiles\":{\"active\":" + jsonQuote(currentMode == MODE_BLE ? (String("HOST") + String(currentBleSlot + 1)) : "TV") + 
          ",\"items\":[" + profileJson("HOST1") + "," + profileJson("HOST2") + "," + profileJson("HOST3") + "," + profileJson("TV") + "]},";
  
  json += "\"ble_slot\":" + String(currentBleSlot + 1) + ",\"wifi_connected\":" + String(wifi ? "true" : "false") + ",\"ip\":" + jsonQuote(wifi ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"ssid\":" + jsonQuote(stored_ssid) + ",\"tv_ip\":" + jsonQuote(stored_tv_ip) + ",\"tv_port\":" + String(stored_tv_port) + ",\"has_auth\":" + String(stored_tv_auth.length() > 0 ? "true" : "false") + ",";

  // Flat structure for website backwards compatibility:
  for (int i = 0; i < 8; ++i) {
    json += "\"mac_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[currentBleSlot][i]) + ",";
  }
  json += "\"mac_dial\":" + jsonQuote(dev_bindings[currentBleSlot][8]) + ",";

  for (int i = 0; i < 8; ++i) {
    json += "\"tv_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(tv_bindings[i]) + ",";
  }
  json += "\"tv_dial\":" + jsonQuote(tv_bindings[8]) + "}";
  return json;
}

void sendStatus() {
  if (!requireAuth()) return;
  sendJson(200, buildStatusJson());
}

bool readProfilePayload(const String& body, String& profile, String values[11]) {
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile)) return false;
  for (int i = 0; i < 11; ++i) if (!jsonString(body, SLOT_IDS[i], values[i], 512) || !isSupportedAction(values[i])) return false;
  return true;
}

void handleSaveBinding() {
  String body, profile, slot, action;
  if (!requestBody(body)) return;
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile) || 
      !jsonString(body, "slot", slot, 8) || slotIndex(slot) < 0 || 
      !jsonString(body, "action", action, 512) || !isSupportedAction(action)) {
    sendError(422, "invalid_assignment", "Use a supported profile, control, and action."); 
    return;
  }
  saveBinding(profile, slotIndex(slot), action);
  if (!setup_done) {
    setup_done = true;
    prefs.begin("config", false); prefs.putBool("setup_done", true); prefs.end();
  }
  sendJson(200, "{\"ok\":true,\"message\":\"Assignment saved to the device.\"}");
}

void handleSaveProfile() {
  String body, profile, values[11];
  if (!requestBody(body)) return;
  if (!readProfilePayload(body, profile, values)) {
    sendError(422, "invalid_profile", "Submit all supported controls with valid actions.");
    return;
  }
  String* bindings = profileBindings(profile);
  for (int i = 0; i < 11; ++i) bindings[i] = values[i];
  saveProfile(profile);
  sendJson(200, "{\"ok\":true,\"message\":\"Profile saved to the device.\"}");
}

void handleRenameProfile() {
  String body, profile, name;
  if (!requestBody(body)) return;
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile) || !jsonString(body, "name", name, 20) || !validAlias(name)) {
    sendError(422, "invalid_name", "Names use 1–20 letters, numbers, spaces, hyphens, or underscores."); return;
  }
  profileName(profile) = name; 
  saveProfile(profile);
  sendJson(200, "{\"ok\":true,\"message\":\"Profile renamed.\"}");
}

void handleCopyProfile() {
  String body, source, target;
  if (!requestBody(body)) return;
  if (!jsonString(body, "source", source, 6) || !jsonString(body, "target", target, 6) || !validProfile(source) || !validProfile(target) || source == target) {
    sendError(422, "invalid_copy", "Choose two different supported device profiles."); return;
  }
  String* from = profileBindings(source); String* to = profileBindings(target);
  for (int i = 0; i < 11; ++i) to[i] = from[i];
  saveProfile(target);
  sendJson(200, "{\"ok\":true,\"message\":\"Assignments copied to the target profile.\"}");
}

void handleExportConfig() {
  if (!requireAuth()) return;
  String json = "{\"schema\":\"studio-pad-config\",\"version\":2,\"tv_name\":" + jsonQuote(tv_name);
  for (int h = 0; h < 3; ++h) {
    String pfx = "h" + String(h + 1) + "_";
    json += ",\"" + pfx + "name\":" + jsonQuote(dev_names[h]);
    for (int i = 0; i < 11; ++i) {
      json += ",\"" + pfx + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[h][i]);
    }
  }
  for (int i = 0; i < 11; ++i) {
    json += ",\"tv_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(tv_bindings[i]);
  }
  json += ",\"mac_name\":" + jsonQuote(dev_names[currentBleSlot]);
  for (int i = 0; i < 11; ++i) {
    json += ",\"mac_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[currentBleSlot][i]);
  }
  json += "}";
  server.sendHeader("Content-Disposition", "attachment; filename=studio-pad-backup.json");
  sendJson(200, json);
}

void handleImportConfig() {
  String body, schema;
  if (!requestBody(body)) return;
  int version = 0;
  if (!jsonString(body, "schema", schema, 32) || schema != "studio-pad-config" || !jsonInt(body, "version", version)) {
    sendError(422, "incompatible_backup", "This is not a compatible Studio Pad configuration backup."); return;
  }
  // Restore TV
  String impTvName;
  if (jsonString(body, "tv_name", impTvName, 20) && validAlias(impTvName)) tv_name = impTvName;
  for (int i = 0; i < 11; ++i) {
    String tvKey = String("tv_") + SLOT_IDS[i];
    if (jsonHasKey(body, tvKey.c_str())) jsonString(body, tvKey.c_str(), tv_bindings[i], 128);
  }
  saveProfile("TV");

  // Restore 3 Devices
  for (int h = 0; h < 3; ++h) {
    String pfx = "h" + String(h + 1) + "_";
    String hName;
    if (jsonString(body, (pfx + "name").c_str(), hName, 20) && validAlias(hName)) dev_names[h] = hName;
    for (int i = 0; i < 11; ++i) {
      String key = pfx + SLOT_IDS[i];
      if (jsonHasKey(body, key.c_str())) jsonString(body, key.c_str(), dev_bindings[h][i], 512);
    }
    String hTag = String("HOST") + String(h + 1);
    saveProfile(hTag);
  }
  sendJson(200, "{\"ok\":true,\"message\":\"Configuration restored to the device.\"}");
}

void handleSaveWifi() {
  String body, ssid, pass;
  if (!requestBody(body)) return;
  if (!jsonString(body, "ssid", ssid, 32) || !jsonString(body, "pass", pass, 63) || !validSsid(ssid) || !validPass(pass)) {
    sendError(422, "invalid_wifi", "Enter a Wi-Fi network name up to 32 characters and a password up to 63 characters."); return;
  }
  stored_ssid = ssid; stored_pass = pass;
  prefs.begin("config", false); prefs.putString("ssid", stored_ssid); prefs.putString("pass", stored_pass); prefs.end();
  WiFi.mode(WIFI_AP_STA); WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
  sendJson(202, "{\"ok\":true,\"message\":\"Wi-Fi credentials saved. The device is connecting.\"}");
}

void handleSaveTvConfig() {
  String body, host, token;
  int port = 0;
  if (!requestBody(body)) return;
  if (!jsonString(body, "tv_ip", host, 63) || !jsonInt(body, "tv_port", port) || !jsonString(body, "token", token, 128) || !validHost(host) || port < 1 || port > 65535) {
    sendError(422, "invalid_tv_config", "Enter a valid TV host and port. The token may be blank until pairing is complete."); return;
  }
  stored_tv_ip = host; stored_tv_port = port;
  if (token.length() > 0) { stored_tv_auth = token; stored_tv_auth.trim(); }
  prefs.begin("config", false); prefs.putString("tv_ip", stored_tv_ip); prefs.putInt("tv_port", stored_tv_port); prefs.putString("tv_auth", stored_tv_auth); prefs.end();
  sendJson(200, "{\"ok\":true,\"message\":\"TV configuration saved.\"}");
}

void handleTvTest() {
  if (WiFi.status() != WL_CONNECTED || !tvIsConfigured()) { sendError(409, "tv_unavailable", "Connect Wi-Fi and configure the TV before testing it."); return; }
  String response = sendVizioRequestRaw("GET", "/state/device/power_mode", "");
  if (response.indexOf("200 OK") != -1 || response.indexOf("\"ITEMS\"") != -1) sendJson(200, "{\"ok\":true,\"message\":\"The TV responded.\"}");
  else sendError(502, "tv_no_response", "The TV did not respond. Check its host, port, and network access.");
}

void handlePairStart() {
  String body, host;
  int port = 0;
  if (!requestBody(body)) return;
  if (!jsonString(body, "tv_ip", host, 63) || !jsonInt(body, "tv_port", port) || !validHost(host) || port < 1 || port > 65535) {
    sendError(422, "invalid_tv_config", "Enter a valid TV host and port before requesting a PIN."); return;
  }
  stored_tv_ip = host; stored_tv_port = port;
  String response = sendVizioRequestRaw("PUT", "/pairing/start", "{\"DEVICE_ID\":\"esp32_macropad\",\"DEVICE_NAME\":\"Studio Pad\"}", stored_tv_port);
  int tokenPos = response.indexOf("\"PAIRING_REQ_TOKEN\":");
  if (tokenPos < 0) { sendError(502, "pairing_start_failed", "The TV did not issue a pairing request. Check its host and network access."); return; }
  pairingReqToken = response.substring(tokenPos + 20).toInt();
  prefs.begin("config", false); prefs.putString("tv_ip", stored_tv_ip); prefs.putInt("tv_port", stored_tv_port); prefs.end();
  sendJson(200, "{\"ok\":true,\"message\":\"PIN requested. Look at the TV screen.\"}");
}

void handlePairSubmit() {
  String body, pin;
  if (!requestBody(body)) return;
  if (!jsonString(body, "pin", pin, 8) || !validPin(pin) || pairingReqToken == 0) { sendError(422, "invalid_pin", "Request a pairing PIN first, then enter the 4–8 digit value from the TV."); return; }
  char payload[256];
  snprintf(payload, sizeof(payload), "{\"DEVICE_ID\":\"esp32_macropad\",\"CHALLENGE_TYPE\":1,\"RESPONSE_VALUE\":\"%s\",\"PAIRING_REQ_TOKEN\":%d}", pin.c_str(), pairingReqToken);
  String response = sendVizioRequestRaw("PUT", "/pairing/pair", payload);
  int tokenPos = response.indexOf("\"AUTH_TOKEN\":\"");
  if (tokenPos < 0) { sendError(400, "pairing_rejected", "The TV rejected that PIN. Request a new one and try again."); return; }
  int start = tokenPos + 14; int end = response.indexOf('"', start);
  if (end <= start) { sendError(502, "pairing_response_invalid", "The TV returned an invalid pairing response."); return; }
  stored_tv_auth = response.substring(start, end); stored_tv_auth.trim(); pairingReqToken = 0;
  prefs.begin("config", false); prefs.putString("tv_auth", stored_tv_auth); prefs.end();
  sendJson(200, "{\"ok\":true,\"message\":\"TV pairing completed and stored on the device.\"}");
}

void handleForgetTv() {
  String body, confirm;
  if (!requestBody(body)) return;
  if (!jsonString(body, "confirm", confirm, 8) || confirm != "FORGET") { sendError(422, "confirmation_required", "Type the confirmation value before forgetting this TV configuration."); return; }
  tvClient.stop(); pairingReqToken = 0; stored_tv_ip = ""; stored_tv_port = DEFAULT_TV_PORT; stored_tv_auth = "";
  prefs.begin("config", false); prefs.remove("tv_ip"); prefs.remove("tv_port"); prefs.remove("tv_auth"); prefs.end();
  sendJson(200, "{\"ok\":true,\"message\":\"TV host and pairing token removed from Studio Pad.\"}");
}

void handleResetDeviceSettings() {
  String body, confirm;
  if (!requestBody(body)) return;
  if (!jsonString(body, "confirm", confirm, 8) || confirm != "RESET") { sendError(422, "confirmation_required", "Type the confirmation value before restoring profile defaults."); return; }
  resetProfile("HOST1"); 
  resetProfile("HOST2"); 
  resetProfile("HOST3"); 
  resetProfile("TV");
  sendJson(200, "{\"ok\":true,\"message\":\"All profiles restored to blank defaults.\"}");
}

void handleTestAction() {
  String body, action;
  if (!requestBody(body)) return;
  if (!jsonString(body, "action", action, 128) || !isSupportedAction(action)) { sendError(422, "unsupported_action", "That action is not available on this firmware."); return; }
  if (!executeAction(action)) { sendError(409, "action_unavailable", isBleAction(action) ? "Connect Bluetooth before testing this action." : "Connect Wi-Fi and configure the TV before testing this action."); return; }
  sendJson(202, "{\"ok\":true,\"message\":\"Action accepted by Studio Pad.\"}");
}

void handleTestKey() {
  String body; int codeset = 0, code = 0;
  if (!requestBody(body)) return;
  if (!jsonInt(body, "codeset", codeset) || !jsonInt(body, "code", code) || !tvIsConfigured() || WiFi.status() != WL_CONNECTED) { sendError(422, "invalid_tv_command", "Configure the TV and send a supported key command."); return; }
  if (!dispatchKey(codeset, code)) { sendError(503, "queue_full", "The TV command queue is busy. Try again."); return; }
  sendJson(202, "{\"ok\":true,\"message\":\"TV key queued.\"}");
}

void handleTestApp() {
  String body, action;
  if (!requestBody(body)) return;
  if (!jsonString(body, "action", action, 128)) {
    String appId;
    if (!jsonString(body, "appId", appId, 8)) { sendError(422, "invalid_app", "Send a supported APP action."); return; }
    if (appId == "1") action = "APP:YOUTUBE";
    else if (appId == "75") action = "APP:DISNEY";
    else if (appId == "4") action = "APP:TUBI";
    else if (appId == "2") action = "APP:NETFLIX";
    else if (appId == "8") action = "APP:PRIME";
    else if (appId == "128") action = "APP:APPLETV";
  }
  if (!isSupportedAction(action) || !action.startsWith("APP:")) { sendError(422, "invalid_app", "Send a supported APP action."); return; }
  if (!executeAction(action)) { sendError(409, "action_unavailable", "Connect Wi-Fi and configure the TV before testing this action."); return; }
  sendJson(202, "{\"ok\":true,\"message\":\"Streaming app launch queued.\"}");
}

void handleActivateProfile() {
  String body, profile;
  if (!requestBody(body)) return;
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile)) { sendError(422, "invalid_profile", "Use MAC or TV."); return; }
  setActiveMode(profile == "TV" ? MODE_TV : MODE_BLE);
  sendJson(200, "{\"ok\":true,\"message\":\"Profile activated.\"}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/plain", "Studio Pad Pro: Web portal available or use USB Web Serial."); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/device", HTTP_GET, sendStatus);
  server.on("/api/customizer/save", HTTP_POST, handleSaveBinding);
  server.on("/api/profile/save", HTTP_POST, handleSaveProfile);
  server.on("/api/profile/rename", HTTP_POST, handleRenameProfile);
  server.on("/api/profile/copy", HTTP_POST, handleCopyProfile);
  server.on("/api/profile/activate", HTTP_POST, handleActivateProfile);
  server.on("/api/config/export", HTTP_GET, handleExportConfig);
  server.on("/api/config/import", HTTP_POST, handleImportConfig);
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/token/save", HTTP_POST, handleSaveTvConfig);
  server.on("/api/tv/test", HTTP_POST, handleTvTest);
  server.on("/api/pair/start", HTTP_POST, handlePairStart);
  server.on("/api/pair/submit", HTTP_POST, handlePairSubmit);
  server.on("/api/tv/forget", HTTP_POST, handleForgetTv);
  server.on("/api/device/reset-settings", HTTP_POST, handleResetDeviceSettings);
  server.on("/api/test/action", HTTP_POST, handleTestAction);
  server.on("/api/test/key", HTTP_POST, handleTestKey);
  server.on("/api/test/app", HTTP_POST, handleTestApp);

  server.on("/api/setup/complete", HTTP_POST, []() {
    int doneVal = 1;
    if (server.hasArg("plain")) jsonInt(server.arg("plain"), "setup_done", doneVal);
    setup_done = (doneVal != 0);
    inSetupMode = false;
    prefs.begin("config", false); 
    prefs.putBool("setup_done", setup_done); 
    prefs.end();
    screenDirty = true;
    showToast("Setup Saved", 1000);
    emitSetupStatus();
    sendJson(200, "{\"ok\":true,\"message\":\"Setup saved to Pad.\"}");
  });

  server.on("/api/setup/enter", HTTP_POST, []() {
    int doneVal = 0;
    if (server.hasArg("plain")) jsonInt(server.arg("plain"), "setup_done", doneVal);
    setup_done = (doneVal != 0);
    inSetupMode = true;
    prefs.begin("config", false); 
    prefs.putBool("setup_done", setup_done); 
    prefs.end();
    screenDirty = true;
    emitSetupStatus();
    sendJson(200, "{\"ok\":true,\"message\":\"Entered setup mode.\"}");
  });

  server.on("/api/ble/switch", HTTP_POST, []() {
    String body;
    int slot = 0;
    if (!requestBody(body)) return;
    if (!jsonInt(body, "slot", slot) || slot < 1 || slot > MAX_BLE_SLOTS) {
      sendError(422, "invalid_slot", "Slot must be 1, 2, or 3.");
      return;
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Switching BLE host...\"}");
    delay(200);
    switchBleDevice(slot - 1);
  });

  static const char* headerKeys[] = { "X-Pad-Token" };
  server.collectHeaders(headerKeys, 1);
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) { addCorsHeaders(); server.send(204, "text/plain", ""); return; }
    sendError(404, "not_found", "This API route does not exist.");
  });
  server.begin();
}

// -----------------------------------------------------------------------------
// USB Web Serial Command Processor
// -----------------------------------------------------------------------------
void processSerialPacket(const String& body) {
  String path = "";
  if (!jsonString(body, "path", path, 64)) return;

  if (path == "/api/status" || path == "/api/device") {
    Serial.println(buildStatusJson());
    return;
  }

  // --- USB Serial Stream Flash Handlers ---
  if (path == "/api/ota/begin") {
    int size = 0;
    jsonInt(body, "size", size);
    otaTotalSize = (size > 0) ? (size_t)size : UPDATE_SIZE_UNKNOWN;
    otaBytesWritten = 0;

    // Disconnect BLE & release keys to free memory and prevent interference
    if (bleKeyboard.isConnected()) bleKeyboard.releaseAll();

    // Show visual status on OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 16);
    display.println("UPDATING FIRMWARE");
    display.setCursor(10, 36);
    display.println("Streaming via USB...");
    display.display();

    if (Update.begin(otaTotalSize, U_FLASH)) {
      Serial.println("{\"ok\":true,\"message\":\"OTA begin ready.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Update.begin failed.\"}");
    }
    return;
  }

  if (path == "/api/ota/chunk") {
    String b64 = "";
    // Accepts either "data" or "chunk" keys
    if (!jsonString(body, "data", b64, 3500)) {
      jsonString(body, "chunk", b64, 3500);
    }

    if (b64.length() == 0) {
      Serial.println("{\"ok\":false,\"message\":\"Missing chunk data.\"}");
      return;
    }

    // Decode base64 stream directly to buffer
    uint8_t decodeBuf[2560];
    size_t outLen = 0;
    int ret = mbedtls_base64_decode(decodeBuf, sizeof(decodeBuf), &outLen, 
                                    (const unsigned char*)b64.c_str(), b64.length());

    if (ret != 0 || outLen == 0) {
      Serial.println("{\"ok\":false,\"message\":\"Base64 decode failed.\"}");
      return;
    }

    size_t written = Update.write(decodeBuf, outLen);
    if (written == outLen) {
      otaBytesWritten += written;
      Serial.println("{\"ok\":true,\"written\":" + String(written) + ",\"total\":" + String(otaBytesWritten) + "}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Flash write failed.\"}");
    }
    return;
  }

  if (path == "/api/ota/end") {
    if (Update.end(true)) {
      Serial.println("{\"ok\":true,\"message\":\"Firmware flashed successfully. Rebooting...\"}");
      Serial.flush();

      // Display completion notice
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(16, 20);
      display.println("UPDATE COMPLETE!");
      display.setCursor(28, 38);
      display.println("Rebooting...");
      display.display();

      delay(700);
      ESP.restart();
      return;
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Update failed: " + String(Update.errorString()) + "\"}");
      return;
    }
  }
  // ----------------------------------------

  if (path == "/api/setup/complete") {
    int doneVal = 1;
    jsonInt(body, "setup_done", doneVal);
    setup_done = (doneVal != 0);
    inSetupMode = false;
    prefs.begin("config", false); 
    prefs.putBool("setup_done", setup_done); 
    prefs.end();
    screenDirty = true;
    showToast("Setup Saved", 1000);
    emitSetupStatus();
    Serial.println("{\"ok\":true,\"message\":\"Setup saved to Pad.\"}");
    return;
  }

  if (path == "/api/setup/enter") {
    int doneVal = 0;
    jsonInt(body, "setup_done", doneVal);
    setup_done = (doneVal != 0);
    inSetupMode = true;
    prefs.begin("config", false); 
    prefs.putBool("setup_done", setup_done); 
    prefs.end();
    screenDirty = true;
    emitSetupStatus();
    Serial.println("{\"ok\":true,\"message\":\"Entered setup mode.\"}");
    return;
  }
  
  if (path == "/api/ble/switch") {
    int slot = 0;
    if (jsonInt(body, "slot", slot) && slot >= 1 && slot <= MAX_BLE_SLOTS) {
      Serial.println("{\"ok\":true,\"message\":\"Switching BLE host...\"}");
      switchBleDevice(slot - 1);
      return;
    }
    Serial.println("{\"ok\":false,\"message\":\"Invalid host slot.\"}");
    return;
  }

  if (path == "/api/customizer/save") {
    String profile, slot, action;
    if (jsonString(body, "profile", profile, 6) && validProfile(profile) &&
        jsonString(body, "slot", slot, 8) && slotIndex(slot) >= 0 &&
        jsonString(body, "action", action, 128) && isSupportedAction(action)) {
      saveBinding(profile, slotIndex(slot), action);
      Serial.println("{\"ok\":true,\"message\":\"Assignment saved.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid assignment.\"}");
    }
    return;
  }

  if (path == "/api/profile/save") {
    String profile, values[11];
    if (readProfilePayload(body, profile, values)) {
      String* bindings = profileBindings(profile);
      for (int i = 0; i < 11; ++i) bindings[i] = values[i];
      saveProfile(profile);
      Serial.println("{\"ok\":true,\"message\":\"Profile saved to device.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid profile payload.\"}");
    }
    return;
  }

  if (path == "/api/profile/rename") {
    String profile, name;
    if (jsonString(body, "profile", profile, 6) && validProfile(profile) &&
        jsonString(body, "name", name, 20) && validAlias(name)) {
      profileName(profile) = name;
      saveProfile(profile);
      Serial.println("{\"ok\":true,\"message\":\"Profile renamed.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid profile name.\"}");
    }
    return;
  }

  if (path == "/api/profile/copy") {
    String source, target;
    if (jsonString(body, "source", source, 6) && jsonString(body, "target", target, 6) &&
        validProfile(source) && validProfile(target) && source != target) {
      String* from = profileBindings(source);
      String* to = profileBindings(target);
      for (int i = 0; i < 11; ++i) to[i] = from[i];
      saveProfile(target);
      Serial.println("{\"ok\":true,\"message\":\"Assignments copied.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid copy parameters.\"}");
    }
    return;
  }

  if (path == "/api/config/export") {
    String json = "{\"schema\":\"studio-pad-config\",\"version\":2,\"tv_name\":" + jsonQuote(tv_name);
    for (int h = 0; h < 3; ++h) {
      String pfx = "h" + String(h + 1) + "_";
      json += ",\"" + pfx + "name\":" + jsonQuote(dev_names[h]);
      for (int i = 0; i < 11; ++i) {
        json += ",\"" + pfx + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[h][i]);
      }
    }
    for (int i = 0; i < 11; ++i) {
      json += ",\"tv_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(tv_bindings[i]);
    }
    json += ",\"mac_name\":" + jsonQuote(dev_names[currentBleSlot]);
    for (int i = 0; i < 11; ++i) {
      json += ",\"mac_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[currentBleSlot][i]);
    }
    json += "}";
    Serial.println(json);
    return;
  }

  if (path == "/api/config/import") {
    String schema;
    int version = 0;
    if (jsonString(body, "schema", schema, 32) && schema == "studio-pad-config" && jsonInt(body, "version", version)) {
      String impTvName;
      if (jsonString(body, "tv_name", impTvName, 20) && validAlias(impTvName)) tv_name = impTvName;
      for (int i = 0; i < 11; ++i) {
        String tvKey = String("tv_") + SLOT_IDS[i];
        if (jsonHasKey(body, tvKey.c_str())) jsonString(body, tvKey.c_str(), tv_bindings[i], 128);
      }
      saveProfile("TV");

      for (int h = 0; h < 3; ++h) {
        String pfx = "h" + String(h + 1) + "_";
        String hName;
        if (jsonString(body, (pfx + "name").c_str(), hName, 20) && validAlias(hName)) dev_names[h] = hName;
        for (int i = 0; i < 11; ++i) {
          String key = pfx + SLOT_IDS[i];
          if (jsonHasKey(body, key.c_str())) jsonString(body, key.c_str(), dev_bindings[h][i], 512);
        }
        String hTag = String("HOST") + String(h + 1);
        saveProfile(hTag);
      }
      Serial.println("{\"ok\":true,\"message\":\"Configuration restored to the device.\"}");
      return;
    }
    Serial.println("{\"ok\":false,\"message\":\"Incompatible backup configuration.\"}");
    return;
  }

  if (path == "/api/wifi") {
    String ssid, pass;
    if (jsonString(body, "ssid", ssid, 32) && jsonString(body, "pass", pass, 63)) {
      stored_ssid = ssid; stored_pass = pass;
      prefs.begin("config", false); prefs.putString("ssid", stored_ssid); prefs.putString("pass", stored_pass); prefs.end();
      WiFi.mode(WIFI_AP_STA); WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
      Serial.println("{\"ok\":true,\"message\":\"Wi-Fi credentials saved! Connecting...\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid Wi-Fi credentials.\"}");
    }
    return;
  }

  if (path == "/api/token/save") {
    String host, token;
    int port = 7345;
    jsonString(body, "tv_ip", host, 63);
    jsonInt(body, "tv_port", port);
    jsonString(body, "token", token, 128);
    if (host.length() > 0) stored_tv_ip = host;
    if (port > 0) stored_tv_port = port;
    if (token.length() > 0) { stored_tv_auth = token; stored_tv_auth.trim(); }
    prefs.begin("config", false); prefs.putString("tv_ip", stored_tv_ip); prefs.putInt("tv_port", stored_tv_port); prefs.putString("tv_auth", stored_tv_auth); prefs.end();
    Serial.println("{\"ok\":true,\"message\":\"TV settings saved.\"}");
    return;
  }

  if (path == "/api/tv/test") {
    if (WiFi.status() != WL_CONNECTED || !tvIsConfigured()) {
      Serial.println("{\"ok\":false,\"message\":\"Connect Wi-Fi and configure TV first.\"}");
      return;
    }
    String response = sendVizioRequestRaw("GET", "/state/device/power_mode", "");
    if (response.indexOf("200 OK") != -1 || response.indexOf("\"ITEMS\"") != -1) {
      Serial.println("{\"ok\":true,\"message\":\"TV answered successfully!\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"TV did not respond.\"}");
    }
    return;
  }

  if (path == "/api/pair/start") {
    String host; int port = DEFAULT_TV_PORT;
    jsonString(body, "tv_ip", host, 63); jsonInt(body, "tv_port", port);
    if (validHost(host) && port >= 1 && port <= 65535) {
      stored_tv_ip = host; stored_tv_port = port;
      String response = sendVizioRequestRaw("PUT", "/pairing/start", "{\"DEVICE_ID\":\"esp32_macropad\",\"DEVICE_NAME\":\"Studio Pad\"}", stored_tv_port);
      int tokenPos = response.indexOf("\"PAIRING_REQ_TOKEN\":");
      if (tokenPos >= 0) {
        pairingReqToken = response.substring(tokenPos + 20).toInt();
        prefs.begin("config", false); prefs.putString("tv_ip", stored_tv_ip); prefs.putInt("tv_port", stored_tv_port); prefs.end();
        Serial.println("{\"ok\":true,\"message\":\"PIN requested. Look at the TV screen.\"}");
        return;
      }
    }
    Serial.println("{\"ok\":false,\"message\":\"Could not start pairing with TV.\"}");
    return;
  }

  if (path == "/api/pair/submit") {
    String pin;
    if (jsonString(body, "pin", pin, 8) && validPin(pin) && pairingReqToken != 0) {
      char payload[256];
      snprintf(payload, sizeof(payload), "{\"DEVICE_ID\":\"esp32_macropad\",\"CHALLENGE_TYPE\":1,\"RESPONSE_VALUE\":\"%s\",\"PAIRING_REQ_TOKEN\":%d}", pin.c_str(), pairingReqToken);
      String response = sendVizioRequestRaw("PUT", "/pairing/pair", payload);
      int tokenPos = response.indexOf("\"AUTH_TOKEN\":\"");
      if (tokenPos >= 0) {
        int start = tokenPos + 14; int end = response.indexOf('"', start);
        if (end > start) {
          stored_tv_auth = response.substring(start, end); stored_tv_auth.trim(); pairingReqToken = 0;
          prefs.begin("config", false); prefs.putString("tv_auth", stored_tv_auth); prefs.end();
          Serial.println("{\"ok\":true,\"message\":\"TV pairing completed and stored.\"}");
          return;
        }
      }
    }
    Serial.println("{\"ok\":false,\"message\":\"Invalid PIN or pairing rejected.\"}");
    return;
  }

  if (path == "/api/tv/forget") {
    String confirm;
    if (jsonString(body, "confirm", confirm, 8) && confirm == "FORGET") {
      tvClient.stop(); pairingReqToken = 0; stored_tv_ip = ""; stored_tv_port = DEFAULT_TV_PORT; stored_tv_auth = "";
      prefs.begin("config", false); prefs.remove("tv_ip"); prefs.remove("tv_port"); prefs.remove("tv_auth"); prefs.end();
      Serial.println("{\"ok\":true,\"message\":\"TV configuration removed.\"}");
      return;
    }
    Serial.println("{\"ok\":false,\"message\":\"Confirmation required to forget TV.\"}");
    return;
  }

  if (path == "/api/device/reset-settings") {
    String confirm;
    if (jsonString(body, "confirm", confirm, 8) && confirm == "RESET") {
      resetProfile("HOST1"); resetProfile("HOST2"); resetProfile("HOST3"); resetProfile("TV");
      Serial.println("{\"ok\":true,\"message\":\"Profiles restored to blank defaults.\"}");
      return;
    }
    Serial.println("{\"ok\":false,\"message\":\"Confirmation required to reset profiles.\"}");
    return;
  }

  if (path == "/api/test/action") {
    String action;
    if (jsonString(body, "action", action, 128) && isSupportedAction(action)) {
      executeAction(action);
      Serial.println("{\"ok\":true,\"message\":\"Action dispatched.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Unsupported action.\"}");
    }
    return;
  }

  if (path == "/api/test/key") {
    int cs = 0, cd = 0;
    if (jsonInt(body, "codeset", cs) && jsonInt(body, "code", cd)) {
      dispatchKey(cs, cd);
      Serial.println("{\"ok\":true,\"message\":\"TV key queued.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid key codes.\"}");
    }
    return;
  }

  if (path == "/api/test/app") {
    String action, appId, msg = "";
    int ns = 5;
    if (jsonString(body, "appId", appId, 8)) {
      jsonInt(body, "nameSpace", ns);
      jsonString(body, "message", msg, 64);
      dispatchApp(appId.c_str(), ns, msg.c_str());
      Serial.println("{\"ok\":true,\"message\":\"TV app queued.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid app parameters.\"}");
    }
    return;
  }

  if (path == "/api/profile/activate") {
    String profile;
    if (jsonString(body, "profile", profile, 6) && validProfile(profile)) {
      setActiveMode(profile == "TV" ? MODE_TV : MODE_BLE);
      Serial.println("{\"ok\":true,\"message\":\"Profile activated.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid profile.\"}");
    }
    return;
  }

  if (path == "/api/events/set") {
    int enabled = 0;
    if (jsonInt(body, "enabled", enabled)) {
      events_enabled = enabled != 0;
      Serial.println(events_enabled ? "{\"ok\":true,\"message\":\"Live events on.\"}" : "{\"ok\":true,\"message\":\"Live events off.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Send enabled as 1 or 0.\"}");
    }
    return;
  }

  if (path == "/api/auth/set") {
    String token;
    if (jsonString(body, "token", token, 64) && (token.length() == 0 || token.length() >= 8)) {
      api_token = token;
      prefs.begin("config", false); prefs.putString("api_token", api_token); prefs.end();
      Serial.println(token.length() ? "{\"ok\":true,\"message\":\"HTTP API token saved.\"}" : "{\"ok\":true,\"message\":\"HTTP API disabled.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Token must be 8-64 characters, or empty to disable HTTP.\"}");
    }
    return;
  }

  Serial.println("{\"ok\":true,\"message\":\"Command received.\"}");
}

bool isDeviceConfigured() {
  if (setup_done) return true;
  if (stored_ssid.length() > 0) return true;
  for (int h = 0; h < 3; ++h) {
    for (int i = 0; i < 11; ++i) {
      if (dev_bindings[h][i].length() > 0) return true;
    }
  }
  return false;
}

void emitSetupStatus() {
  if (inSetupMode) {
    // Matches the web app's contract triggers
    Serial.println("{\"event\":\"setup\",\"setup_mode\":true,\"mode\":\"setup\"}");
  } else {
    Serial.println("{\"event\":\"setup\",\"setup_mode\":false,\"mode\":\"normal\"}");
  }
}

void startSetupPortal() {
  inSetupMode = !inSetupMode;
  screenDirty = true;
  emitSetupStatus();
}

void renderScreen() {
  display.clearDisplay();
  if (inSetupMode) {
    display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK); display.setTextSize(1); display.setCursor(18, 2); display.print("USB SETUP MODE");
    display.setTextColor(SSD1306_WHITE); display.setCursor(0, 18);
    display.println("1. Connect USB cable");
    display.println("2. Open Web App");
    display.println("3. Click 'Connect USB'");
    display.setCursor(0, 54);
    display.println("(Press Mode to exit)");
    display.display(); screenDirty = false; return;
  }

  if (currentMode == MODE_BLE) {
    display.drawBitmap(0, 0, bitmap_mode_mac, 128, 64, SSD1306_WHITE);
    // Upper-right badge: shows [D1], [D2], or [D3] for active device
    display.fillRect(104, 1, 23, 9, SSD1306_BLACK);
    display.drawRect(104, 1, 23, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(108, 2);
    display.print("D"); display.print(currentBleSlot + 1);
  } else {
    display.drawBitmap(0, 0, bitmap_mode_tv, 128, 64, SSD1306_WHITE);
  }

  if (millis() < toastExpiry) {
    int width = min(116, (int)strlen(toastMessage) * 6 + 12); int x = (128 - width) / 2;
    display.fillRect(x, 46, width, 15, SSD1306_BLACK); display.drawRect(x, 46, width, 15, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(x + 6, 50); display.print(toastMessage);
  }
  display.display(); screenDirty = false;
}

void handleEncoder(int direction) {
  int slot = (direction > 0) ? 9 : 10;
  emitEvent(String("{\"event\":\"turn\",\"slot\":\"") + SLOT_IDS[slot] + "\"}");
  String action = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][slot] : tv_bindings[slot]);
  executeAction(action);
}

void handleButtonPress(uint8_t pin) {
  if (pin == PIN_BTN_MODE) {
    if (inSetupMode) { inSetupMode = false; screenDirty = true; return; }
    emitEvent("{\"event\":\"press\",\"slot\":\"mode\"}");
    setActiveMode(currentMode == MODE_BLE ? MODE_TV : MODE_BLE);
    return;
  }
  int index = -1;
  if (pin == PIN_BTN1) index = 0;
  else if (pin == PIN_BTN2) index = 1;
  else if (pin == PIN_BTN3) index = 2;
  else if (pin == PIN_BTN4) index = 3;
  else if (pin == PIN_BTN5) index = 4;
  else if (pin == PIN_BTN6) index = 5;
  else if (pin == PIN_BTN7) index = 6;
  else if (pin == PIN_BTN8) index = 7;
  else if (pin == PIN_ENC_SW) index = 8;
  if (index >= 0) {
    emitEvent(String("{\"event\":\"press\",\"slot\":\"") + SLOT_IDS[index] + "\"}");
    String action = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][index] : tv_bindings[index]);
    executeAction(action);
  }
}

void loadConfiguration() {
  prefs.begin("config", true);
  stored_ssid = prefs.getString("ssid", ""); 
  stored_pass = prefs.getString("pass", "");
  stored_tv_ip = prefs.getString("tv_ip", DEFAULT_TV_IP); 
  stored_tv_port = prefs.getInt("tv_port", DEFAULT_TV_PORT); 
  stored_tv_auth = prefs.getString("tv_auth", DEFAULT_TV_AUTH); 
  api_token = prefs.getString("api_token", ""); 
  config_rev = prefs.getUInt("rev", 0); 
  currentMode = prefs.getUChar("mode", 0) == 1 ? MODE_TV : MODE_BLE;
  currentBleSlot = prefs.getUChar("ble_slot", 0);
  if (currentBleSlot >= MAX_BLE_SLOTS) currentBleSlot = 0;
  setup_done = prefs.getBool("setup_done", false);
  prefs.end();
  stored_tv_auth.trim();

  prefs.begin("matrix", true);
  // TV Profile
  tv_name = prefs.getString("t_name", "TV");
  for (int i = 0; i < 11; ++i) {
    tv_bindings[i] = prefs.getString((String("t_") + SLOT_IDS[i]).c_str(), DEFAULT_TV_BINDINGS[i]);
    if (!isSupportedAction(tv_bindings[i])) tv_bindings[i] = DEFAULT_TV_BINDINGS[i];
  }
  if (!validAlias(tv_name)) tv_name = "TV";

  // Load 3 Independent Device Profiles
  const char* const defNames[3] = { "Device 1", "Device 2", "Device 3" };
  for (int h = 0; h < 3; ++h) {
    String pfx = String("d") + String(h + 1) + "_";
    String legacyPfx = String("h") + String(h + 1) + "_";
    dev_names[h] = prefs.getString((pfx + "name").c_str(), prefs.getString((legacyPfx + "name").c_str(), defNames[h]));
    if (!validAlias(dev_names[h])) dev_names[h] = defNames[h];

    for (int i = 0; i < 11; ++i) {
      String key = pfx + SLOT_IDS[i];
      String legacyKey = legacyPfx + SLOT_IDS[i];
      dev_bindings[h][i] = prefs.getString(key.c_str(), prefs.getString(legacyKey.c_str(), DEFAULT_BLANK_BINDINGS[i]));
      if (!isSupportedAction(dev_bindings[h][i])) dev_bindings[h][i] = DEFAULT_BLANK_BINDINGS[i];
    }
  }
  prefs.end();

  if (stored_tv_port < 1 || stored_tv_port > 65535) stored_tv_port = DEFAULT_TV_PORT;
}

// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200); delay(100);
  pinMode(PIN_LED_BT, OUTPUT); pinMode(PIN_LED_WIFI, OUTPUT);
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) pinMode(buttons[i].pin, INPUT_PULLUP);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP); pinMode(PIN_ENC_DT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrEncoder, CHANGE);
  Wire.begin(OLED_SDA, OLED_SCL); Wire.setClock(400000); display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay(); display.display();

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

  // Set Virtual MAC and Device Name for active slot
  setSlotMacAddress(currentBleSlot);
  bleDeviceName = "Studio Pad " + String(currentBleSlot + 1);
  bleKeyboard.setName(bleDeviceName.c_str());

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

  // Web Serial listener for index.html / web app over USB
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > MAX_REQUEST_BODY) {
      Serial.println("{\"ok\":false,\"message\":\"Request too large.\"}");
    } else if (line.length() > 0 && line.startsWith("{")) {
      processSerialPacket(line);
    }
  }

  const bool wifi = WiFi.status() == WL_CONNECTED;
  digitalWrite(PIN_LED_BT, bleKeyboard.isConnected() ? HIGH : LOW); digitalWrite(PIN_LED_WIFI, wifi ? HIGH : LOW);
  static bool lastBle = false, lastWifi = false;
  const bool ble = bleKeyboard.isConnected();
  if (ble != lastBle || wifi != lastWifi) {
    lastBle = ble; lastWifi = wifi;
    emitEvent(String("{\"event\":\"connection\",\"ble\":") + (ble ? "true" : "false") + ",\"wifi\":" + (wifi ? "true" : "false") + "}");
  }
  if (wifi && !mdnsStarted) mdnsStarted = MDNS.begin("macropad");

  if (encoderDelta != 0) {
    noInterrupts(); int delta = encoderDelta; encoderDelta = 0; interrupts();

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
        if (reading == LOW) modeBtnPressStart = now;  // short press fires on release
        else if (modeBtnPressStart > 0) { modeBtnPressStart = 0; handleButtonPress(PIN_BTN_MODE); }
      } else if (reading == LOW) {
        handleButtonPress(buttons[i].pin);
      }
    }
    buttons[i].lastState = reading;
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
  delay(1);
}