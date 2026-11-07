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
#include <esp_ota_ops.h>
#include <vector>
#include <nvs.h>

// -----------------------------------------------------------------------------
// Lightweight NimBLE Engine
// -----------------------------------------------------------------------------
#define USE_NIMBLE
#include <NimBLEDevice.h>
#include <BleKeyboard.h>

#include "config.h"
#include "bitmaps.h"
#include "bertopad_logo.h"

// Firmware Versioning & Defaults
// -----------------------------------------------------------------------------
#ifndef API_VERSION
#define API_VERSION 2
#endif
// The .ino owns the version: ignore any stale FIRMWARE_VERSION left in config.h
#ifdef FIRMWARE_VERSION
#undef FIRMWARE_VERSION
#endif
#define FIRMWARE_VERSION "4.10.0"
#ifndef SETUP_AP_PREFIX
#define SETUP_AP_PREFIX "BertoPad"
#endif

#define MAX_BLE_SLOTS 3
#define BASE_SLOT_COUNT 11   // original controls: b1..b8, dial, enc_cw, enc_ccw
#define SLOT_COUNT 27        // + push_cw/push_ccw (dial chords) + dial2/e2_cw/e2_ccw (second encoder) + b1h..b8h (key hold) + g_shake/g_down/g_up (motion)
#define KEY_HOLD_MS 500
#ifndef BLE_MANAGER_ENABLED
#define BLE_MANAGER_ENABLED 1   // 0 = build without pairing / disconnect / forget management (set in config.h)
#endif      // hold a key this long to fire its hold action

// Second rotary encoder: ENCODER2_ENABLED and PIN_ENC2_CLK / PIN_ENC2_DT / PIN_ENC2_SW come from config.h.
// Fail the build instead of silently compiling without the encoder if config.h is out of date.
#if !defined(ENCODER2_ENABLED) || !defined(PIN_ENC2_CLK) || !defined(PIN_ENC2_DT) || !defined(PIN_ENC2_SW)
#error "config.h must define ENCODER2_ENABLED and PIN_ENC2_CLK / PIN_ENC2_DT / PIN_ENC2_SW"
#endif

enum SlotIndex { SLOT_DIAL = 8, SLOT_ENC_CW = 9, SLOT_ENC_CCW = 10, SLOT_PUSH_CW = 11, SLOT_PUSH_CCW = 12,
                 SLOT_DIAL2 = 13, SLOT_E2_CW = 14, SLOT_E2_CCW = 15, SLOT_HOLD0 = 16,
                 SLOT_G_SHAKE = 24, SLOT_G_DOWN = 25, SLOT_G_UP = 26 };
#define MAX_REPEAT_DEPTH 2   // REPEAT blocks may nest this deep
#define MAX_REPEAT_COUNT 250
#define MAX_MACRO_STEPS 5000 // hard ceiling on executed steps per macro run
#define MAX_ACTION_LEN 512   // max bytes in one binding / multi-step macro (HTTP, USB and backup import)
#define MAX_DELAY_MS 10000   // longest single DELAY:<ms> step
#define OTA_IDLE_TIMEOUT_MS 30000UL // abort a stalled USB stream flash and reboot

// -----------------------------------------------------------------------------
// Hardware Slot Definitions (11 Slots per Device Profile)
// -----------------------------------------------------------------------------
const char* const SLOT_IDS[SLOT_COUNT] = { 
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "dial", "enc_cw", "enc_ccw",
  "push_cw", "push_ccw",
  "dial2", "e2_cw", "e2_ccw",
  "b1h", "b2h", "b3h", "b4h", "b5h", "b6h", "b7h", "b8h",
  "g_shake", "g_down", "g_up"
};

// All Bluetooth slots start blank out of the box until mapped on the website
const char* const DEFAULT_BLANK_BINDINGS[SLOT_COUNT] = {
  "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "",
  "", "", ""
};

// Default TV Controls (Wi-Fi Vizio)
const char* const DEFAULT_TV_BINDINGS[SLOT_COUNT] = {
  "TV:POWER", "APP:YOUTUBE", "APP:DISNEY", "APP:TUBI",
  "APP:NETFLIX", "APP:PRIME", "TV:INFO", "TV:HOME",
  "TV:MUTE", "TV:VOL_UP", "TV:VOL_DOWN",
  "", "", "", "", "",
  "", "", "", "", "", "", "", "",
  "", "", ""
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
String dev_bindings[3][SLOT_COUNT];
String tv_name = "TV";
String tv_bindings[SLOT_COUNT];

PadMode currentMode = MODE_BLE;
uint8_t currentBleSlot = 0; // 0 = Device 1, 1 = Device 2, 2 = Device 3
String bleDeviceName = "BertoPad 1";

bool inSetupMode = false;
bool setup_done = false;
bool mdnsStarted = false;

// OTA Stream Flash Tracking
size_t otaTotalSize = 0;
size_t otaBytesWritten = 0;
bool otaActive = false;               // true between /api/ota/begin and /api/ota/end
bool inputLockout = false;            // swallow button events after a macro abort until everything is released
bool encPressArmed = false;           // dial click fires on release, unless it became a push-turn chord
bool encChordUsed = false;
bool encoderAccelEnabled = true;      // velocity-scaled volume / channel steps
bool tv_enabled = true;               // false = TV mode fully off (Mode key, TV profile, TV/APP actions)
unsigned long lastEncAccelTime = 0;
unsigned long lastEnc2AccelTime = 0;
bool keyHoldPending[8] = {false};     // key is down and has a hold action: tap fires on release, hold after KEY_HOLD_MS
bool keyHoldFired[8] = {false};
unsigned long keyPressAt[8] = {0};
uint16_t dispDimSec = 60;             // OLED dims after this many idle seconds (0 = never)
uint16_t dispOffSec = 300;            // OLED turns off after this many idle seconds (0 = never)
uint8_t dispContrast = 255;           // 0-255
unsigned long lastActivity = 0;
bool dispDimmed = false, dispOff = false;

// GY-521 / MPU-6050 motion logic. Pure math (no hardware access) so it can be tested on a PC.
struct MotionDetector {
  float rx = 0, ry = 0, rz = 1;      // unit vector of the pad lying face-up at rest (set by calibration)
  uint8_t sens = 3;                  // 1 (least sensitive) .. 5 (most sensitive)
  float lastJerk = 0;                // g change since the previous sample (used for pick-up wake)
  float orient = 1.0f;               // smoothed alignment with the rest vector (+1 face-up, -1 face-down)
  bool down = false;                 // currently face-down
  bool havePrev = false;
  float pgx = 0, pgy = 0, pgz = 1;
  uint32_t belowSince = 0, aboveSince = 0, windowStart = 0, cooldownUntil = 0, lastPeak = 0;
  uint8_t peaks = 0;
  enum { EV_SHAKE = 1, EV_DOWN = 2, EV_UP = 4 };
  float wakeThreshold() const { static const float t[5] = {0.45f, 0.30f, 0.20f, 0.14f, 0.10f}; return t[(sens < 1 ? 1 : sens > 5 ? 5 : sens) - 1]; }
  float shakeThreshold() const { static const float t[5] = {1.6f, 1.3f, 1.0f, 0.8f, 0.6f}; return t[(sens < 1 ? 1 : sens > 5 ? 5 : sens) - 1]; }
  // Feed one accelerometer sample (in g). Returns a bit mask of EV_* events.
  int update(float gx, float gy, float gz, uint32_t now) {
    int ev = 0;
    if (!havePrev) { pgx = gx; pgy = gy; pgz = gz; havePrev = true; }
    float dx = gx - pgx, dy = gy - pgy, dz = gz - pgz;
    lastJerk = sqrtf(dx * dx + dy * dy + dz * dz);
    pgx = gx; pgy = gy; pgz = gz;
    // Shake: 4 sharp impulses within 800 ms, then a 1.5 s cooldown
    if (now >= cooldownUntil && lastJerk > shakeThreshold() && now - lastPeak >= 60) {
      lastPeak = now;
      if (peaks == 0 || now - windowStart > 800) { windowStart = now; peaks = 0; }
      if (++peaks >= 4) { ev |= EV_SHAKE; peaks = 0; cooldownUntil = now + 1500; }
    }
    // Face-down / face-up, with hold times so a quick tilt never counts
    orient = orient * 0.8f + (gx * rx + gy * ry + gz * rz) * 0.2f;
    if (!down) {
      if (orient < -0.8f) {
        if (belowSince == 0) belowSince = now ? now : 1;
        if (now - belowSince >= 800) { down = true; ev |= EV_DOWN; aboveSince = 0; }
      } else belowSince = 0;
    } else {
      if (orient > 0.7f) {
        if (aboveSince == 0) aboveSince = now ? now : 1;
        if (now - aboveSince >= 400) { down = false; ev |= EV_UP; belowSince = 0; }
      } else aboveSince = 0;
    }
    return ev;
  }
};
MotionDetector motionDet;
bool motionPresent = false, motionEnabled = true, motionWake = true;
uint8_t motionSens = 3, mpuAddr = 0x68, motionWho = 0, motionFails = 0;
uint32_t motionLastPoll = 0;
int pendingBleSlot = -1;            // host being previewed while Mode is held and the dial is turned
uint8_t otaLastPct = 255;           // last OTA progress percentage drawn on the OLED
uint32_t blePairUntil = 0, bleConnectedAt = 0, bleRestartAt = 0;   // pairing window end, link age, delayed reboot
bool bleHoldOff = false;            // advertising paused by the app
bool bleTrusted = false;            // the connected host is a bonded (paired) one
bool bleWasConnected = false, bleRecordedConn = false;
bool bleStrict = true;              // only bonded hosts may type; new hosts only while pairing mode is on
String bootReason = "";
String lastAbnormalReset = "";
uint32_t abnormalResets = 0;
unsigned long otaLastActivity = 0;
String setupSsid = "";
String setupPass = "";

Preferences prefs;
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BleKeyboard bleKeyboard("BertoPad 1", "BertoPad", 100);
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

#if ENCODER2_ENABLED
volatile int encoderDelta2 = 0;
volatile uint8_t encState2 = 0;

void IRAM_ATTR isrEncoder2() {
  uint8_t pinstate = (digitalRead(PIN_ENC2_CLK) << 1) | digitalRead(PIN_ENC2_DT);
  encState2 = ttable[encState2 & 0x0F][pinstate];
  uint8_t result = encState2 & 0x30;
  if (result == 0x10) encoderDelta2++;
  else if (result == 0x20) encoderDelta2--;
}
#endif

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
#if ENCODER2_ENABLED
  , { PIN_ENC2_SW, HIGH, HIGH, 0 }
#endif
};

// -----------------------------------------------------------------------------
// Logitech M720-Style Virtual MAC & Multi-Host Switching
// -----------------------------------------------------------------------------
void setSlotMacAddress(uint8_t slot) {
  uint8_t gen = 0;
  prefs.begin("config", true);
  gen = prefs.getUChar((String("ble_gen") + String(slot)).c_str(), 0);
  prefs.end();
  if (slot == 0 && gen == 0) return; // Slot 0 uses the default factory eFuse MAC until it is forgotten
  uint64_t efuse = ESP.getEfuseMac();
  uint8_t baseMac[6];
  baseMac[0] = (uint8_t)(efuse);
  baseMac[1] = (uint8_t)(efuse >> 8);
  baseMac[2] = (uint8_t)(efuse >> 16);
  baseMac[3] = (uint8_t)(efuse >> 24);
  baseMac[4] = (uint8_t)((efuse >> 32) + gen * 3);          // forgetting a slot gives it a fresh Bluetooth identity
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
const size_t MAX_REQUEST_BODY = 8192; // a full profile of long macros can exceed 4 KB

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

// Reads a boolean sent as true/false or 1/0
bool jsonFlag(const String& body, const char* key, bool& out) {
  int v = 0;
  if (jsonInt(body, key, v)) { out = (v != 0); return true; }
  String needle = "\"" + String(key) + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return false;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return false;
  int pos = colon + 1;
  while (pos < (int)body.length() && isspace((unsigned char)body[pos])) ++pos;
  if (body.startsWith("true", pos))  { out = true;  return true; }
  if (body.startsWith("false", pos)) { out = false; return true; }
  return false;
}

// Trims one "||" macro segment. TEXT: steps keep their trailing spaces / newlines
// (only leading whitespace before the TEXT: keyword is removed).
String trimMacroStep(const String& raw) {
  unsigned int s = 0;
  while (s < raw.length() && isspace((unsigned char)raw[s])) ++s;
  String step = raw.substring(s);
  if (!step.startsWith("TEXT:")) step.trim();
  return step;
}

// DELAY:<ms>, digits only, 0..MAX_DELAY_MS
bool parseDelayMs(const String& action, int& msOut) {
  if (!action.startsWith("DELAY:")) return false;
  String num = action.substring(6);
  num.trim();
  if (num.length() < 1 || num.length() > 5) return false;
  for (size_t i = 0; i < num.length(); ++i) if (!isdigit((unsigned char)num[i])) return false;
  int ms = num.toInt();
  if (ms < 0 || ms > MAX_DELAY_MS) return false;
  msOut = ms;
  return true;
}

// REPEAT:<n> opens a loop block (1..MAX_REPEAT_COUNT) that is closed by END, e.g.
//   REPEAT:3||MEDIA:NEXT||DELAY:200||END
bool parseRepeat(const String& s, int& countOut) {
  if (!s.startsWith("REPEAT:")) return false;
  String num = s.substring(7);
  num.trim();
  if (num.length() < 1 || num.length() > 3) return false;
  for (size_t i = 0; i < num.length(); ++i) if (!isdigit((unsigned char)num[i])) return false;
  int n = num.toInt();
  if (n < 1 || n > MAX_REPEAT_COUNT) return false;
  countOut = n;
  return true;
}

void splitMacro(const String& act, std::vector<String>& out) {
  int start = 0;
  while (start < (int)act.length()) {
    int idx = act.indexOf("||", start);
    out.push_back(trimMacroStep(idx >= 0 ? act.substring(start, idx) : act.substring(start)));
    if (idx < 0) break;
    start = idx + 2;
  }
}

bool isSupportedAction(const String& action) {
  if (action == "" || action == "NONE") return true; // Unassigned is valid
  if (action.length() < 3 || action.length() > MAX_ACTION_LEN) return false;

  // Validate multi-step macros (REPEAT/END blocks must be balanced)
  if (action.indexOf("||") >= 0) {
    std::vector<String> steps;
    splitMacro(action, steps);
    int depth = 0, n = 0;
    for (size_t k = 0; k < steps.size(); ++k) {
      const String& sub = steps[k];
      if (parseRepeat(sub, n)) { if (++depth > MAX_REPEAT_DEPTH) return false; continue; }
      if (sub == "END") { if (depth == 0) return false; --depth; continue; }
      if (!isSupportedAction(sub)) return false;
    }
    return depth == 0;
  }

  if (action.startsWith("DELAY:")) { int ms = 0; return parseDelayMs(action, ms); }
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
  for (int i = 0; i < SLOT_COUNT; ++i) if (slot == SLOT_IDS[i]) return i;
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
  if (events_enabled && !otaActive) Serial.println(json);
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
  for (int i = 0; i < SLOT_COUNT; ++i) prefs.putString((String(profilePrefix(profile)) + SLOT_IDS[i]).c_str(), bindings[i]);
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
  for (int i = 0; i < SLOT_COUNT; ++i) bindings[i] = defaults[i];
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
const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    default:                return "unknown";
  }
}

// Remembers why the pad rebooted. Crashes, watchdog resets and brown-outs are counted in NVS.
void recordBootReason() {
  esp_reset_reason_t r = esp_reset_reason();
  bootReason = resetReasonName(r);
  bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
  prefs.begin("diag", false);
  abnormalResets = prefs.getUInt("abn_cnt", 0);
  lastAbnormalReset = prefs.getString("abn_last", "");
  if (abnormal) {
    abnormalResets++;
    lastAbnormalReset = bootReason;
    prefs.putUInt("abn_cnt", abnormalResets);
    prefs.putString("abn_last", lastAbnormalReset);
  }
  prefs.end();
}

void clearDiagnosticsLog() {
  prefs.begin("diag", false); prefs.clear(); prefs.end();
  abnormalResets = 0; lastAbnormalReset = "";
}

void setEncoderAccel(bool on) {
  encoderAccelEnabled = on;
  prefs.begin("config", false); prefs.putBool("enc_accel", on); prefs.end();
  emitEvent(String("{\"event\":\"encoder\",\"accel\":") + (on ? "true" : "false") + "}");
}

void showToast(const char* msg, unsigned long durationMs = 1000) {
  strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
  toastMessage[sizeof(toastMessage) - 1] = '\0';
  toastExpiry = millis() + durationMs;
  screenDirty = true;
  wakeDisplay();
}

// ---- OLED care: contrast, auto-dim and auto-off (protects the panel from burn-in) ----
void applyDisplayContrast() {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(dispContrast);
}

void wakeDisplay() {
  lastActivity = millis();
  if (dispOff) { display.ssd1306_command(SSD1306_DISPLAYON); dispOff = false; screenDirty = true; }
  if (dispDimmed) { display.dim(false); applyDisplayContrast(); dispDimmed = false; }
}

void updateDisplayPower() {
  if (inSetupMode) { lastActivity = millis(); return; }
  if (dispDimSec == 0 && dispOffSec == 0) return;
  unsigned long idle = millis() - lastActivity;
  if (!dispOff && dispOffSec > 0 && idle >= (unsigned long)dispOffSec * 1000UL) {
    display.ssd1306_command(SSD1306_DISPLAYOFF); dispOff = true; return;
  }
  if (!dispDimmed && !dispOff && dispDimSec > 0 && idle >= (unsigned long)dispDimSec * 1000UL) {
    display.dim(true); dispDimmed = true;
  }
}

bool applyDisplaySettings(const String& body) {
  int dim = dispDimSec, off = dispOffSec, con = dispContrast;
  if (jsonHasKey(body, "dim")      && (!jsonInt(body, "dim", dim)           || dim < 0 || dim > 36000)) return false;
  if (jsonHasKey(body, "off")      && (!jsonInt(body, "off", off)           || off < 0 || off > 36000)) return false;
  if (jsonHasKey(body, "contrast") && (!jsonInt(body, "contrast", con)      || con < 0 || con > 255))   return false;
  dispDimSec = (uint16_t)dim; dispOffSec = (uint16_t)off; dispContrast = (uint8_t)con;
  prefs.begin("config", false);
  prefs.putUShort("disp_dim", dispDimSec); prefs.putUShort("disp_off", dispOffSec); prefs.putUChar("disp_con", dispContrast);
  prefs.end();
  applyDisplayContrast();
  wakeDisplay();
  return true;
}

// ---- Status LEDs: PWM brightness, follow the OLED sleep state, breathe while waiting for Bluetooth ----
#define LED_BRIGHT_MAX 160
#define LED_BRIGHT_DIM 25
void ledWrite(uint8_t which, uint8_t level) {
  uint8_t duty = (uint8_t)(((uint16_t)level * level) / 255);   // gamma curve for an even-looking fade
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(which == 0 ? PIN_LED_BT : PIN_LED_WIFI, duty);
#else
  ledcWrite(which, duty);
#endif
}

void initStatusLeds() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LED_BT, 5000, 8); ledcAttach(PIN_LED_WIFI, 5000, 8);
#else
  ledcSetup(0, 5000, 8); ledcAttachPin(PIN_LED_BT, 0);
  ledcSetup(1, 5000, 8); ledcAttachPin(PIN_LED_WIFI, 1);
#endif
  ledWrite(0, 0); ledWrite(1, 0);
}

void updateStatusLeds(bool ble, bool wifi) {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 20) return;
  last = now;
  if (dispOff) { ledWrite(0, 0); ledWrite(1, 0); return; }
  const uint8_t maxB = dispDimmed ? LED_BRIGHT_DIM : LED_BRIGHT_MAX;
  if (blePairUntil) ledWrite(0, ((now / 125) % 2) ? maxB : 0);   // fast blink = pairing mode
  else if (bleHoldOff) ledWrite(0, 0);
  else if (ble) ledWrite(0, maxB);
  else if (currentMode == MODE_BLE) ledWrite(0, (uint8_t)((sinf(now * 0.004f) + 1.0f) * 0.5f * maxB));   // waiting for the host
  else ledWrite(0, 0);
  ledWrite(1, wifi ? maxB : 0);
}

// ---- Bluetooth management: pairing mode, disconnect, forget one profile's pairing ----
#if BLE_MANAGER_ENABLED
static bool bleListHas(const String& list, const String& addr) {
  int start = 0;
  while (start <= (int)list.length()) {
    int c = list.indexOf(',', start);
    String item = (c < 0) ? list.substring(start) : list.substring(start, c);
    if (item.length() > 0 && item.equalsIgnoreCase(addr)) return true;
    if (c < 0) break;
    start = c + 1;
  }
  return false;
}

static String bleBondKey(uint8_t slot) { return String("bond_s") + String(slot); }

String bleReadBonds(uint8_t slot) {
  prefs.begin("config", true);
  String v = prefs.getString(bleBondKey(slot).c_str(), "");
  prefs.end();
  return v;
}

int bleBondCount(uint8_t slot) {
  String v = bleReadBonds(slot);
  if (v.length() == 0) return 0;
  int n = 1;
  for (unsigned i = 0; i < v.length(); ++i) if (v[i] == ',') n++;
  return n;
}

// Remembers which profile a host paired with, so "forget" can remove only that profile's pairing.
// Returns true when the host was not known to any profile before.
bool bleRecordPeer(uint8_t slot, const String& addr) {
  bool known = false;
  for (uint8_t i = 0; i < MAX_BLE_SLOTS; ++i) if (bleListHas(bleReadBonds(i), addr)) known = true;
  String mine = bleReadBonds(slot);
  if (!bleListHas(mine, addr)) {
    mine = (mine.length() > 0) ? (mine + "," + addr) : addr;
    while (mine.length() > 3 * 17 + 2) mine = mine.substring(mine.indexOf(',') + 1);   // keep the newest 3 hosts
    prefs.begin("config", false);
    prefs.putString(bleBondKey(slot).c_str(), mine);
    prefs.end();
  }
  return !known;
}

static void bleDropAllLinks() {
  NimBLEServer* srv = NimBLEDevice::getServer();
  if (!srv) return;
  std::vector<uint16_t> peers = srv->getPeerDevices();
  for (size_t i = 0; i < peers.size(); ++i) srv->disconnect(peers[i]);
}

bool bleLinkUsable() { return bleKeyboard.isConnected() && (!bleStrict || bleTrusted); }

String bleStatusFields() {
  String s = String(",\"connected\":") + (bleKeyboard.isConnected() ? "true" : "false") +
             ",\"trusted\":" + (bleTrusted ? "true" : "false") +
             ",\"pairing\":" + (blePairUntil ? "true" : "false") +
             ",\"pair_seconds\":" + String(((int32_t)(blePairUntil - millis()) > 0 && blePairUntil) ? (uint32_t)((blePairUntil - millis()) / 1000) : 0) +
             ",\"paused\":" + (bleHoldOff ? "true" : "false") +
             ",\"strict\":" + (bleStrict ? "true" : "false") + ",\"paired\":[";
  for (uint8_t i = 0; i < MAX_BLE_SLOTS; ++i) s += String(i ? "," : "") + String(bleBondCount(i));
  return s + "]";
}

// cmd: disconnect | connect | pair | forget | settings
bool bleCommand(const String& cmd, const String& body, String& msg) {
  if (cmd == "disconnect") {
    bool stayOff = false;
    jsonFlag(body, "stay_off", stayOff);
    bool had = bleKeyboard.isConnected();
    bleHoldOff = stayOff;
    if (stayOff) blePairUntil = 0;
    bleDropAllLinks();
    if (stayOff) NimBLEDevice::getAdvertising()->stop();
    showToast(stayOff ? "BLE Paused" : "BLE Disconnected", 900);
    msg = had ? "Disconnected from the host." : "No host was connected.";
    if (stayOff) msg += " Bluetooth stays hidden until you press Connect or start pairing.";
    emitEvent(String("{\"event\":\"ble\",\"state\":\"") + (stayOff ? "paused" : "disconnected") + "\"}");
    return true;
  }
  if (cmd == "connect") {
    bleHoldOff = false;
    NimBLEDevice::startAdvertising();
    showToast("BLE Visible", 900);
    msg = "Bluetooth is visible again. Paired hosts reconnect on their own.";
    emitEvent("{\"event\":\"ble\",\"state\":\"visible\"}");
    return true;
  }
  if (cmd == "pair") {
    int sec = 120;
    if (jsonHasKey(body, "seconds") && !jsonInt(body, "seconds", sec)) { msg = "seconds must be a number."; return false; }
    if (sec < 0 || sec > 600) { msg = "seconds must be 0-600."; return false; }
    if (sec == 0) {
      blePairUntil = 0;
      msg = "Pairing mode is off.";
      emitEvent("{\"event\":\"ble\",\"state\":\"pairing_off\"}");
    } else {
      bleHoldOff = false;
      blePairUntil = millis() + (uint32_t)sec * 1000UL;
      if (blePairUntil == 0) blePairUntil = 1;
      NimBLEDevice::startAdvertising();
      showToast("Pairing...", 1200);
      msg = String("Pairing mode is on for ") + String(sec) + " s. On the computer, add \"" + bleDeviceName + "\" in Bluetooth settings.";
      emitEvent(String("{\"event\":\"ble\",\"state\":\"pairing\",\"seconds\":") + String(sec) + "}");
    }
    return true;
  }
  if (cmd == "settings") {
    bool v = bleStrict;
    if (!jsonHasKey(body, "strict") || !jsonFlag(body, "strict", v)) { msg = "Send strict as true or false."; return false; }
    bleStrict = v;
    prefs.begin("config", false); prefs.putBool("ble_strict", bleStrict); prefs.end();
    msg = bleStrict ? "Only paired hosts can connect outside pairing mode." : "Any host may connect (strict mode off).";
    return true;
  }
  if (cmd == "forget") {
    String confirm;
    if (!jsonString(body, "confirm", confirm, 8) || confirm != "FORGET") { msg = "Confirmation required to forget this pairing."; return false; }
    int slot = currentBleSlot + 1;
    if (jsonHasKey(body, "slot") && (!jsonInt(body, "slot", slot) || slot < 1 || slot > MAX_BLE_SLOTS)) { msg = "Slot must be 1, 2 or 3."; return false; }
    uint8_t sIdx = (uint8_t)(slot - 1);
    bool active = (sIdx == currentBleSlot);
    if (active) bleDropAllLinks();
    String rec = bleReadBonds(sIdx);
    int removed = 0;
    for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; --i) {
      NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
      if (bleListHas(rec, String(a.toString().c_str()))) { if (NimBLEDevice::deleteBond(a)) removed++; }
    }
    prefs.begin("config", false);
    String genKey = String("ble_gen") + String(sIdx);
    prefs.putUChar(genKey.c_str(), prefs.getUChar(genKey.c_str(), 0) + 1);   // new Bluetooth identity for this profile
    prefs.remove(bleBondKey(sIdx).c_str());
    if (active) prefs.putBool("pair_boot", true);                            // open pairing right after the restart
    prefs.end();
    bool alsoBindings = false;
    jsonFlag(body, "bindings", alsoBindings);
    if (alsoBindings) resetProfile(String("HOST") + String(slot));
    msg = String("Forgot the pairing for Device ") + String(slot) + " (" + String(removed) + " host" + (removed == 1 ? "" : "s") +
          " removed). Remove the old \"BertoPad " + String(slot) + "\" from that computer's Bluetooth list, then pair again.";
    if (active) { msg += " The pad restarts now."; bleRestartAt = millis() + 600; if (bleRestartAt == 0) bleRestartAt = 1; }
    emitEvent(String("{\"event\":\"ble\",\"state\":\"forgot\",\"slot\":") + String(slot) + "}");
    return true;
  }
  msg = "Unknown Bluetooth command.";
  return false;
}

// Runs every 250 ms: pairing window, advertising pause, recording which profile each host belongs to,
// and dropping unknown hosts that connect outside pairing mode.
void bleManagerTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  if (bleRestartAt && (int32_t)(now - bleRestartAt) >= 0) ESP.restart();
  if (blePairUntil && (int32_t)(now - blePairUntil) >= 0) {
    blePairUntil = 0;
    showToast("Pairing off", 900);
    emitEvent("{\"event\":\"ble\",\"state\":\"pairing_off\"}");
  }
  if (bleHoldOff && !blePairUntil) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv && adv->isAdvertising()) adv->stop();   // the keyboard library restarts advertising after a disconnect
  }
  bool connected = bleKeyboard.isConnected();
  if (connected && !bleWasConnected) { bleConnectedAt = now; bleRecordedConn = false; }
  bleWasConnected = connected;
  if (!connected) { bleTrusted = false; return; }

  NimBLEServer* srv = NimBLEDevice::getServer();
  if (!srv || srv->getConnectedCount() == 0) { bleTrusted = true; return; }   // cannot inspect the link: fail open
  bool anyBonded = false;
  for (size_t i = 0; i < srv->getConnectedCount(); ++i) {
    auto info = srv->getPeerInfo(i);
    if (NimBLEDevice::isBonded(info.getIdAddress())) {
      anyBonded = true;
      if (!bleRecordedConn) {
        bool isNew = bleRecordPeer(currentBleSlot, String(info.getIdAddress().toString().c_str()));
        bleRecordedConn = true;
        if (isNew && blePairUntil) {
          blePairUntil = 0;
          showToast("Paired!", 1200);
          emitEvent("{\"event\":\"ble\",\"state\":\"paired\"}");
        }
      }
    } else if (bleStrict && !blePairUntil && now - bleConnectedAt > 4000) {
      srv->disconnect(info.getConnHandle());     // unknown host outside pairing mode
      emitEvent("{\"event\":\"ble\",\"state\":\"rejected\"}");
    }
  }
  bleTrusted = anyBonded;
}
#else
bool bleLinkUsable() { return bleKeyboard.isConnected(); }
String bleStatusFields() { return ""; }
bool bleCommand(const String&, const String&, String& msg) { msg = "Bluetooth management is not built into this firmware."; return false; }
void bleManagerTick() {}
#endif

// ---- GY-521 (MPU-6050) over the same I2C bus as the OLED (SDA 21 / SCL 22), accelerometer only ----
bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(mpuAddr); Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuBegin() {
  const uint8_t addrs[2] = {0x68, 0x69};              // 0x69 when AD0 is tied high
  for (uint8_t i = 0; i < 2; ++i) {
    mpuAddr = addrs[i];
    Wire.beginTransmission(mpuAddr);
    if (Wire.endTransmission() != 0) continue;
    if (!mpuWrite(0x6B, 0x01)) continue;               // wake from sleep, PLL clock
    delay(10);
    mpuWrite(0x6C, 0x07);                              // gyro axes in standby (accelerometer only)
    mpuWrite(0x1A, 0x03);                              // 44 Hz low-pass filter
    mpuWrite(0x1C, 0x08);                              // +-4 g range (8192 LSB per g)
    Wire.beginTransmission(mpuAddr); Wire.write(0x75); Wire.endTransmission(false);
    Wire.requestFrom((int)mpuAddr, 1);
    motionWho = Wire.available() ? Wire.read() : 0;
    return true;
  }
  return false;
}

static int16_t mpuReadS16() { uint8_t hi = Wire.read(); uint8_t lo = Wire.read(); return (int16_t)((hi << 8) | lo); }

bool mpuRead(int16_t& ax, int16_t& ay, int16_t& az, int16_t& temp) {
  Wire.beginTransmission(mpuAddr); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)mpuAddr, 8) != 8) return false;
  ax = mpuReadS16(); ay = mpuReadS16(); az = mpuReadS16(); temp = mpuReadS16();
  return true;
}

void motionBegin() { motionPresent = mpuBegin(); }

void fireGesture(const char* type, int slot, bool wake) {
  emitEvent(String("{\"event\":\"gesture\",\"type\":\"") + type + "\"}");
  if (wake) wakeDisplay();
  const String& a = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][slot] : tv_bindings[slot]);
  if (a.length() > 0 && a != "NONE") executeAction(a);
}

void motionUpdate() {
  if (!motionPresent || !motionEnabled) return;
  uint32_t now = millis();
  if (now - motionLastPoll < 20) return;               // 50 Hz
  motionLastPoll = now;
  int16_t ax, ay, az, t;
  if (!mpuRead(ax, ay, az, t)) { if (++motionFails >= 50) motionPresent = false; return; } // sensor unplugged: stop polling
  motionFails = 0;
  const float k = 1.0f / 8192.0f;
  int ev = motionDet.update(ax * k, ay * k, az * k, now);
  // Pick-up wake only while the screen is dim/off, so desk vibration can't keep it lit forever
  if (motionWake && (dispDimmed || dispOff) && motionDet.lastJerk > motionDet.wakeThreshold()) wakeDisplay();
  if (ev & MotionDetector::EV_SHAKE) fireGesture("shake", SLOT_G_SHAKE, true);
  if (ev & MotionDetector::EV_DOWN)  fireGesture("down", SLOT_G_DOWN, false);
  if (ev & MotionDetector::EV_UP)    fireGesture("up", SLOT_G_UP, false);
}

// Saves "pad lying flat, face-up" as the reference for the face-down / face-up gestures.
bool motionCalibrate(String& msg) {
  if (!motionPresent) { msg = "No motion sensor detected. Check the GY-521 wiring."; return false; }
  float sx = 0, sy = 0, sz = 0, mn[3] = {9, 9, 9}, mx[3] = {-9, -9, -9};
  for (int i = 0; i < 20; ++i) {
    int16_t ax, ay, az, t;
    if (!mpuRead(ax, ay, az, t)) { msg = "Could not read the motion sensor."; return false; }
    float v[3] = {ax / 8192.0f, ay / 8192.0f, az / 8192.0f};
    sx += v[0]; sy += v[1]; sz += v[2];
    for (int a = 0; a < 3; ++a) { if (v[a] < mn[a]) mn[a] = v[a]; if (v[a] > mx[a]) mx[a] = v[a]; }
    delay(20);
  }
  for (int a = 0; a < 3; ++a) if (mx[a] - mn[a] > 0.15f) { msg = "Keep the pad still while calibrating."; return false; }
  sx /= 20; sy /= 20; sz /= 20;
  float n = sqrtf(sx * sx + sy * sy + sz * sz);
  if (n < 0.5f || n > 1.5f) { msg = "Unexpected sensor reading. Check the wiring and power."; return false; }
  motionDet.rx = sx / n; motionDet.ry = sy / n; motionDet.rz = sz / n;
  motionDet.orient = 1.0f; motionDet.down = false; motionDet.belowSince = 0; motionDet.aboveSince = 0;
  prefs.begin("config", false);
  prefs.putShort("mot_rx", (int16_t)lroundf(motionDet.rx * 1000)); prefs.putShort("mot_ry", (int16_t)lroundf(motionDet.ry * 1000)); prefs.putShort("mot_rz", (int16_t)lroundf(motionDet.rz * 1000));
  prefs.end();
  msg = "Current position saved as flat, face-up.";
  return true;
}

bool applyMotionSettings(const String& body) {
  bool en = motionEnabled, wk = motionWake; int sens = motionSens;
  if (jsonHasKey(body, "enabled")     && !jsonFlag(body, "enabled", en)) return false;
  if (jsonHasKey(body, "wake")        && !jsonFlag(body, "wake", wk)) return false;
  if (jsonHasKey(body, "sensitivity") && (!jsonInt(body, "sensitivity", sens) || sens < 1 || sens > 5)) return false;
  motionEnabled = en; motionWake = wk; motionSens = (uint8_t)sens; motionDet.sens = motionSens;
  prefs.begin("config", false);
  prefs.putBool("mot_en", motionEnabled); prefs.putBool("mot_wake", motionWake); prefs.putUChar("mot_sens", motionSens);
  prefs.end();
  return true;
}

String motionReadJson() {
  int16_t ax, ay, az, t;
  if (!motionPresent || !mpuRead(ax, ay, az, t)) return "{\"ok\":false,\"message\":\"No motion sensor detected. Check the GY-521 wiring.\"}";
  const float mg = 1000.0f / 8192.0f;
  return String("{\"ok\":true,\"ax\":") + String((int)(ax * mg)) + ",\"ay\":" + String((int)(ay * mg)) + ",\"az\":" + String((int)(az * mg)) +
         ",\"temp_c\":" + String(t / 340.0f + 36.53f, 1) + ",\"face_up\":" + String(motionDet.orient, 2) + "}";
}

String nvsStatsJson() {
  nvs_stats_t st;
  if (nvs_get_stats(NULL, &st) == ESP_OK)
    return String("\"nvs_used\":") + String((unsigned)st.used_entries) + ",\"nvs_free\":" + String((unsigned)st.free_entries);
  return "\"nvs_used\":0,\"nvs_free\":0";
}

void setActiveMode(PadMode mode) {
  if (mode == MODE_TV && !tv_enabled) { showToast("TV Mode Off", 800); return; }
  currentMode = mode;
  prefs.begin("config", false); prefs.putUChar("mode", mode == MODE_TV ? 1 : 0); prefs.end();
  String toast = (mode == MODE_BLE ? dev_names[currentBleSlot] : "Profile: TV");
  showToast(toast.c_str(), 800);
  screenDirty = true;
  emitEvent(String("{\"event\":\"profile\",\"active\":\"") + (mode == MODE_BLE ? "MAC" : "TV") + "\"}");
}

const char* tvUnavailableMsg() {
  return tv_enabled ? "Connect Wi-Fi and configure the TV before testing this action."
                    : "TV mode is turned off on the pad.";
}

// Turns TV mode on/off. Turning it off while the TV profile is active falls back to the current host.
// TV bindings, Wi-Fi and pairing data are kept, so switching it back on restores everything.
void setTvEnabled(bool on) {
  tv_enabled = on;
  prefs.begin("config", false);
  prefs.putBool("tv_enabled", on);
  bool fellBack = (!on && currentMode == MODE_TV);
  if (fellBack) { currentMode = MODE_BLE; prefs.putUChar("mode", 0); }
  prefs.end();
  showToast(on ? "TV Mode On" : "TV Mode Off", 800);
  screenDirty = true;
  emitEvent(String("{\"event\":\"tv\",\"enabled\":") + (on ? "true" : "false") + "}");
  if (fellBack) emitEvent("{\"event\":\"profile\",\"active\":\"MAC\"}");
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
  if (!tv_enabled) return false;
  VizioCommand cmd = {};
  cmd.type = CMD_KEY; cmd.codeset = codeset; cmd.code = code;
  return xQueueSend(vizioQueue, &cmd, 0) == pdTRUE;
}

bool dispatchApp(const char* appId, int nameSpace, const char* message) {
  if (!tv_enabled) return false;
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

// -----------------------------------------------------------------------------
// Macro runtime: abort-on-keypress, scoped state, step tracing
// -----------------------------------------------------------------------------
static const size_t kButtonCount = sizeof(buttons) / sizeof(buttons[0]);
static bool macroActive = false;
static bool macroAborted = false;
static bool macroSnapDown[16];
static unsigned long macroLowSince[16];

void macroBegin() {
  macroActive = true;
  macroAborted = false;
  for (size_t i = 0; i < kButtonCount; ++i) {
    macroSnapDown[i] = (digitalRead(buttons[i].pin) == LOW); // held at start (e.g. the trigger key) never aborts
    macroLowSince[i] = 0;
  }
}

// A NEW press on any pad button (key, Mode or dial) while a macro runs stops it.
bool macroPollAbort() {
  if (!macroActive) return false;
  if (macroAborted) return true;
  unsigned long now = millis();
  for (size_t i = 0; i < kButtonCount; ++i) {
    if (digitalRead(buttons[i].pin) != LOW) { macroSnapDown[i] = false; macroLowSince[i] = 0; continue; }
    if (macroSnapDown[i]) continue;
    if (macroLowSince[i] == 0) macroLowSince[i] = now;
    else if (now - macroLowSince[i] >= 15) { macroAborted = true; return true; } // 15 ms debounce
  }
  return false;
}

void macroEnd() {
  if (macroAborted) {
    bleKeyboard.releaseAll();      // never leave Ctrl/Shift/Alt/Cmd stuck
    inputLockout = true;           // the aborting press must not trigger anything else
    showToast("Macro stopped", 900);
    emitEvent("{\"event\":\"macro\",\"state\":\"aborted\"}");
  }
  macroActive = false;
}

// Opens a macro context for the outermost action only (nested calls reuse it).
struct MacroScope {
  bool owner;
  MacroScope() : owner(!macroActive) { if (owner) macroBegin(); }
  ~MacroScope() { if (owner) macroEnd(); }
};

// delay() that notices an abort press within ~5 ms
void interruptibleDelay(unsigned long ms) {
  if (!macroActive) { delay(ms); return; }
  unsigned long end = millis() + ms;
  while (!macroAborted) {
    if (macroPollAbort()) break;
    long rem = (long)(end - millis());
    if (rem <= 0) break;
    delay(rem > 5 ? (unsigned long)5 : (unsigned long)rem);
  }
}

String actionKind(const String& s) {   // "TEXT:secret" -> "TEXT" (never leaks typed content)
  int c = s.indexOf(':');
  return (c > 0) ? s.substring(0, c) : s;
}

bool executeText(const String& act) {
  MacroScope scope;
  for (size_t i = 5; i < act.length(); ++i) {
    if (macroPollAbort()) break;
    char c = act[i];
    if (c == '\r') continue;
    bleKeyboard.write((uint8_t)c);
  }
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
  if (otaActive) return false; // radios and key handling are stopped during stream flashing
  // If button is unassigned/blank, do nothing safely
  if (act == "" || act == "NONE") return true; 

  // --- Handle Multi-Step Macros (action1||action2||action3, with REPEAT:n ... END blocks) ---
  if (act.indexOf("||") >= 0) {
    std::vector<String> steps;
    splitMacro(act, steps);
    MacroScope scope;
    struct Frame { size_t start; int remaining; } stack[MAX_REPEAT_DEPTH];
    int depth = 0, executed = 0, n = 0;
    const size_t total = steps.size();
    if (events_enabled) emitEvent(String("{\"event\":\"macro\",\"state\":\"start\",\"steps\":") + String((unsigned)total) + "}");
    size_t i = 0;
    while (i < total && !macroAborted) {
      if (macroPollAbort()) break;
      const String& s = steps[i];
      if (parseRepeat(s, n)) {
        if (depth < MAX_REPEAT_DEPTH) { stack[depth].start = i + 1; stack[depth].remaining = n; ++depth; }
        ++i; continue;
      }
      if (s == "END") {
        if (depth > 0) {
          if (--stack[depth - 1].remaining > 0) { i = stack[depth - 1].start; continue; }
          --depth;
        }
        ++i; continue;
      }
      if (s.length() > 0) {
        if (++executed > MAX_MACRO_STEPS) break;
        if (events_enabled) emitEvent(String("{\"event\":\"macro\",\"state\":\"step\",\"step\":") + String((unsigned)(i + 1)) +
                                      ",\"of\":" + String((unsigned)total) + ",\"kind\":" + jsonQuote(actionKind(s)) + "}");
        executeAction(s);
        interruptibleDelay(50); // Pause between macro steps
      }
      ++i;
    }
    if (events_enabled && !macroAborted) emitEvent("{\"event\":\"macro\",\"state\":\"end\"}");
    return true;
  }
  // ------------------------------------------------------------

  // Support for custom delay step if specified by web app (e.g., DELAY:200)
  if (act.startsWith("DELAY:")) {
    int ms = 0;
    if (!parseDelayMs(act, ms)) return false;
    MacroScope scope;
    if (ms > 0) interruptibleDelay(ms);
    return true;
  }

  // Host switching actions
  if (act == "BLE:HOST1") { switchBleDevice(0); return true; }
  else if (act == "BLE:HOST2") { switchBleDevice(1); return true; }
  else if (act == "BLE:HOST3") { switchBleDevice(2); return true; }
  else if (act == "BLE:NEXT_HOST") { switchBleDevice((currentBleSlot + 1) % MAX_BLE_SLOTS); return true; }

  if (isBleAction(act) && !bleLinkUsable()) { showToast("BLE Disconnected", 700); return false; }
  if (isTvAction(act) && !tv_enabled) { showToast("TV Mode Off", 700); return false; }
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
  for (int i = 0; i < SLOT_COUNT; ++i) {
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
                ",\"api_version\":" + String(API_VERSION) + ",\"rev\":" + String(config_rev) +
                ",\"capabilities\":[\"delay\",\"repeat\",\"macro_abort\",\"macro_trace\",\"push_turn\",\"encoder_accel\",\"diagnostics\",\"tv_toggle\"" + String(ENCODER2_ENABLED ? ",\"encoder2\"" : "") + ",\"key_hold\",\"display_power\",\"nvs_stats\"" + String(motionPresent ? ",\"motion\"" : "") + String(BLE_MANAGER_ENABLED ? ",\"ble_manage\"" : "") + "],";
  json += "\"device\":{\"id\":" + jsonQuote(deviceId()) + ",\"name\":\"BertoPad\",\"firmware\":\"" FIRMWARE_VERSION "\",\"connection_type\":" + jsonQuote(wifi ? "USB + Wi-Fi" : "USB") + "},";
  json += "\"connection\":{\"wifi_connected\":" + String(wifi ? "true" : "false") + ",\"ip\":" + jsonQuote(wifi ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + ",\"ble_connected\":" + String(bleKeyboard.isConnected() ? "true" : "false") + "},";
  json += "\"ble\":{\"active_host\":" + String(currentBleSlot + 1) + ",\"max_hosts\":" + String(MAX_BLE_SLOTS) + ",\"device_name\":" + jsonQuote(bleDeviceName) + bleStatusFields() + "},";
  json += "\"diagnostics\":{\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"http_api_enabled\":" + String(api_token.length() > 0 ? "true" : "false") + ",\"events_enabled\":" + String(events_enabled ? "true" : "false") +
          ",\"boot_reason\":" + jsonQuote(bootReason) + ",\"abnormal_resets\":" + String(abnormalResets) +
          ",\"last_abnormal_reset\":" + jsonQuote(lastAbnormalReset) + ",\"min_free_heap\":" + String(ESP.getMinFreeHeap()) +
          "," + nvsStatsJson() + ",\"uptime_s\":" + String((unsigned long)(millis() / 1000)) + "},\"encoder\":{\"accel\":" + String(encoderAccelEnabled ? "true" : "false") +
          "},\"display\":{\"dim_s\":" + String(dispDimSec) + ",\"off_s\":" + String(dispOffSec) + ",\"contrast\":" + String(dispContrast) +
          "},\"motion\":{\"present\":" + String(motionPresent ? "true" : "false") + ",\"enabled\":" + String(motionEnabled ? "true" : "false") +
          ",\"wake\":" + String(motionWake ? "true" : "false") + ",\"sensitivity\":" + String(motionSens) + ",\"who\":" + String(motionWho) + "},";
  json += "\"tv\":{\"enabled\":" + String(tv_enabled ? "true" : "false") + ",\"configured\":" + String(tvIsConfigured() ? "true" : "false") + ",\"paired\":" + String(stored_tv_auth.length() > 0 ? "true" : "false") + ",\"host\":" + jsonQuote(stored_tv_ip) + ",\"port\":" + String(stored_tv_port) + "},";
  
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

bool readProfilePayload(const String& body, String& profile, String values[SLOT_COUNT]) {
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile)) return false;
  String* current = profileBindings(profile);
  for (int i = 0; i < SLOT_COUNT; ++i) {
    // push_cw / push_ccw are optional so older web-app builds don't wipe them
    if (i >= BASE_SLOT_COUNT && !jsonHasKey(body, SLOT_IDS[i])) { values[i] = current[i]; continue; }
    if (!jsonString(body, SLOT_IDS[i], values[i], MAX_ACTION_LEN) || !isSupportedAction(values[i])) return false;
  }
  return true;
}

void handleSaveBinding() {
  String body, profile, slot, action;
  if (!requestBody(body)) return;
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile) || 
      !jsonString(body, "slot", slot, 8) || slotIndex(slot) < 0 || 
      !jsonString(body, "action", action, MAX_ACTION_LEN) || !isSupportedAction(action)) {
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
  String body, profile, values[SLOT_COUNT];
  if (!requestBody(body)) return;
  if (!readProfilePayload(body, profile, values)) {
    sendError(422, "invalid_profile", "Submit all supported controls with valid actions.");
    return;
  }
  String* bindings = profileBindings(profile);
  for (int i = 0; i < SLOT_COUNT; ++i) bindings[i] = values[i];
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
  for (int i = 0; i < SLOT_COUNT; ++i) to[i] = from[i];
  saveProfile(target);
  sendJson(200, "{\"ok\":true,\"message\":\"Assignments copied to the target profile.\"}");
}

String buildExportJson() {
  String json = "{\"schema\":\"studio-pad-config\",\"version\":2,\"tv_name\":" + jsonQuote(tv_name);
  for (int h = 0; h < 3; ++h) {
    String pfx = "h" + String(h + 1) + "_";
    json += ",\"" + pfx + "name\":" + jsonQuote(dev_names[h]);
    for (int i = 0; i < SLOT_COUNT; ++i) {
      json += ",\"" + pfx + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[h][i]);
    }
  }
  for (int i = 0; i < SLOT_COUNT; ++i) {
    json += ",\"tv_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(tv_bindings[i]);
  }
  json += ",\"mac_name\":" + jsonQuote(dev_names[currentBleSlot]);
  for (int i = 0; i < SLOT_COUNT; ++i) {
    json += ",\"mac_" + String(SLOT_IDS[i]) + "\":" + jsonQuote(dev_bindings[currentBleSlot][i]);
  }
  json += "}";
  return json;
}

// Reads one binding from a backup. Missing keys are ignored; oversized or unsupported
// values are skipped (the current assignment is kept) instead of being stored and then
// wiped by loadConfiguration() on the next boot.
void importBinding(const String& body, const String& key, String& dest, int& skipped) {
  if (!jsonHasKey(body, key.c_str())) return;
  String value;
  if (jsonString(body, key.c_str(), value, MAX_ACTION_LEN) && isSupportedAction(value)) dest = value;
  else skipped++;
}

bool applyConfigImport(const String& body, int& skipped) {
  String schema;
  int version = 0;
  skipped = 0;
  if (!jsonString(body, "schema", schema, 32) || schema != "studio-pad-config" || !jsonInt(body, "version", version)) return false;

  // Restore TV
  String impTvName;
  if (jsonString(body, "tv_name", impTvName, 20) && validAlias(impTvName)) tv_name = impTvName;
  for (int i = 0; i < SLOT_COUNT; ++i) importBinding(body, String("tv_") + SLOT_IDS[i], tv_bindings[i], skipped);
  saveProfile("TV");

  // Restore 3 Devices
  for (int h = 0; h < 3; ++h) {
    String pfx = "h" + String(h + 1) + "_";
    String hName;
    if (jsonString(body, (pfx + "name").c_str(), hName, 20) && validAlias(hName)) dev_names[h] = hName;
    for (int i = 0; i < SLOT_COUNT; ++i) importBinding(body, pfx + SLOT_IDS[i], dev_bindings[h][i], skipped);
    saveProfile(String("HOST") + String(h + 1));
  }
  return true;
}

void handleExportConfig() {
  if (!requireAuth()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=bertopad-backup.json");
  sendJson(200, buildExportJson());
}

void handleImportConfig() {
  String body;
  if (!requestBody(body)) return;
  int skipped = 0;
  if (!applyConfigImport(body, skipped)) {
    sendError(422, "incompatible_backup", "This is not a compatible BertoPad configuration backup."); return;
  }
  String msg = "Configuration restored to the device.";
  if (skipped > 0) msg += " " + String(skipped) + " unsupported assignment(s) were skipped.";
  sendJson(200, "{\"ok\":true,\"message\":" + jsonQuote(msg) + "}");
}

void handleSaveWifi() {
  String body, ssid, pass;
  if (!requestBody(body)) return;
  if (!jsonString(body, "ssid", ssid, 32) || !jsonString(body, "pass", pass, 63) || !validSsid(ssid) || !validPass(pass)) {
    sendError(422, "invalid_wifi", "Enter a Wi-Fi network name up to 32 characters and a password up to 63 characters."); return;
  }
  stored_ssid = ssid; stored_pass = pass;
  prefs.begin("config", false); prefs.putString("ssid", stored_ssid); prefs.putString("pass", stored_pass); prefs.end();
  WiFi.mode(WIFI_STA); WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
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
  if (!requireAuth()) return;
  if (!tv_enabled) { sendError(409, "tv_disabled", "TV mode is turned off on the pad."); return; }
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
  String response = sendVizioRequestRaw("PUT", "/pairing/start", "{\"DEVICE_ID\":\"esp32_macropad\",\"DEVICE_NAME\":\"BertoPad\"}", stored_tv_port);
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
  sendJson(200, "{\"ok\":true,\"message\":\"TV host and pairing token removed from BertoPad.\"}");
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
  if (!jsonString(body, "action", action, MAX_ACTION_LEN) || !isSupportedAction(action)) { sendError(422, "unsupported_action", "That action is not available on this firmware."); return; }
  if (!executeAction(action)) { sendError(409, "action_unavailable", isBleAction(action) ? "Connect Bluetooth before testing this action." : tvUnavailableMsg()); return; }
  sendJson(202, "{\"ok\":true,\"message\":\"Action accepted by BertoPad.\"}");
}

void handleTestKey() {
  String body; int codeset = 0, code = 0;
  if (!requestBody(body)) return;
  if (!tv_enabled) { sendError(409, "tv_disabled", "TV mode is turned off on the pad."); return; }
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
  if (!executeAction(action)) { sendError(409, "action_unavailable", tvUnavailableMsg()); return; }
  sendJson(202, "{\"ok\":true,\"message\":\"Streaming app launch queued.\"}");
}

void handleActivateProfile() {
  String body, profile;
  if (!requestBody(body)) return;
  if (!jsonString(body, "profile", profile, 6) || !validProfile(profile)) { sendError(422, "invalid_profile", "Use MAC or TV."); return; }
  if (profile == "TV" && !tv_enabled) { sendError(409, "tv_disabled", "TV mode is turned off on the pad."); return; }
  setActiveMode(profile == "TV" ? MODE_TV : MODE_BLE);
  sendJson(200, "{\"ok\":true,\"message\":\"Profile activated.\"}");
}

void handleBleHttp(const char* cmd) {
  String body;
  if (!requestBody(body)) return;
  String msg;
  bool ok = bleCommand(cmd, body, msg);
  sendJson(ok ? 200 : 422, String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":" + jsonQuote(msg) + "}");
}

void handleMotionSettings() {
  String body;
  if (!requestBody(body)) return;
  if (!applyMotionSettings(body)) { sendError(422, "invalid_motion", "enabled and wake must be true/false, sensitivity 1-5."); return; }
  sendJson(200, "{\"ok\":true,\"message\":\"Motion settings saved.\"}");
}

void handleMotionCalibrate() {
  if (!requireAuth()) return;
  String msg; bool ok = motionCalibrate(msg);
  sendJson(ok ? 200 : 409, String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":" + jsonQuote(msg) + "}");
}

void handleMotionRead() {
  if (!requireAuth()) return;
  sendJson(200, motionReadJson());
}

void handleDisplaySettings() {
  String body;
  if (!requestBody(body)) return;
  if (!applyDisplaySettings(body)) { sendError(422, "invalid_display", "dim and off must be 0-36000 seconds, contrast 0-255."); return; }
  sendJson(200, "{\"ok\":true,\"message\":\"Display settings saved.\"}");
}

void handleTvEnable() {
  String body; bool on = true;
  if (!requestBody(body)) return;
  if (!jsonFlag(body, "enabled", on)) { sendError(422, "invalid_tv_enable", "Send enabled as true or false."); return; }
  setTvEnabled(on);
  sendJson(200, String("{\"ok\":true,\"enabled\":") + (tv_enabled ? "true" : "false") + ",\"message\":" +
                jsonQuote(tv_enabled ? "TV mode enabled." : "TV mode disabled.") + "}");
}

void handleEncoderSettings() {
  String body; int accel = 1;
  if (!requestBody(body)) return;
  if (!jsonInt(body, "accel", accel)) { sendError(422, "invalid_encoder", "Send accel as 1 or 0."); return; }
  setEncoderAccel(accel != 0);
  sendJson(200, "{\"ok\":true,\"message\":\"Encoder settings saved.\"}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/plain", "BertoPad: Web portal available or use USB Web Serial."); });
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
  server.on("/api/encoder/set", HTTP_POST, handleEncoderSettings);
  server.on("/api/tv/enable", HTTP_POST, handleTvEnable);
  server.on("/api/display/set", HTTP_POST, handleDisplaySettings);
  server.on("/api/motion/set", HTTP_POST, handleMotionSettings);
  server.on("/api/motion/calibrate", HTTP_POST, handleMotionCalibrate);
  server.on("/api/motion/read", HTTP_GET, handleMotionRead);
  server.on("/api/ble/disconnect", HTTP_POST, []() { handleBleHttp("disconnect"); });
  server.on("/api/ble/connect",    HTTP_POST, []() { handleBleHttp("connect"); });
  server.on("/api/ble/pair",       HTTP_POST, []() { handleBleHttp("pair"); });
  server.on("/api/ble/forget",     HTTP_POST, []() { handleBleHttp("forget"); });
  server.on("/api/ble/settings",   HTTP_POST, []() { handleBleHttp("settings"); });

  server.on("/api/setup/complete", HTTP_POST, []() {
    if (!requireAuth()) return;
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
    if (!requireAuth()) return;
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
    otaLastPct = 255;

    // Restarting a transfer: drop any half-finished update first
    if (otaActive) { Update.abort(); otaActive = false; }

    // Check the partition layout BEFORE shutting the radios down. "Huge APP / No OTA"
    // layouts have no spare app slot; those boards must use the esptool-js USB flasher.
    const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
      Serial.println("{\"ok\":false,\"message\":\"No OTA partition in this board's layout. Use the direct USB flasher instead.\"}");
      return;
    }
    if (size > 0 && (size_t)size > target->size) {
      Serial.println("{\"ok\":false,\"message\":\"Firmware image is larger than the OTA partition.\"}");
      return;
    }

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

    // Quiet everything that can touch flash/cache while sectors are being erased:
    // buttons/encoder/web server are frozen by loop(), Wi-Fi and BLE are shut down.
    otaActive = true;
    otaLastActivity = millis();
    server.stop();
    tvClient.stop();
    WiFi.disconnect(true);
    NimBLEDevice::deinit(true);
    delay(100);

    if (Update.begin(otaTotalSize, U_FLASH)) {
      Serial.println("{\"ok\":true,\"message\":\"OTA begin ready.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Update.begin failed: " + String(Update.errorString()) + ". Rebooting to restore Bluetooth and Wi-Fi.\"}");
      Serial.flush();
      delay(300);
      ESP.restart();
    }
    return;
  }

  if (path == "/api/ota/chunk") {
    if (!otaActive) {
      Serial.println("{\"ok\":false,\"message\":\"OTA not started. Send /api/ota/begin first.\"}");
      return;
    }
    otaLastActivity = millis();

    String b64 = "";
    // Accepts either "data" or "chunk" keys
    if (!jsonString(body, "data", b64, 3500)) {
      jsonString(body, "chunk", b64, 3500);
    }

    if (b64.length() == 0) {
      Serial.println("{\"ok\":false,\"message\":\"Missing chunk data.\"}");
      return;
    }

    // Decode base64 stream directly to buffer (3500 base64 chars -> 2625 bytes max)
    static uint8_t decodeBuf[2640];
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
      if (otaTotalSize > 0 && otaTotalSize != UPDATE_SIZE_UNKNOWN) {
        uint8_t pct = (uint8_t)(((uint64_t)otaBytesWritten * 100ULL) / otaTotalSize);
        if (pct != otaLastPct && (pct % 4 == 0 || pct >= 100)) {
          otaLastPct = pct;
          display.fillRect(14, 50, 100, 8, SSD1306_BLACK);
          display.drawRect(14, 50, 100, 8, SSD1306_WHITE);
          display.fillRect(16, 52, (pct * 96) / 100, 4, SSD1306_WHITE);
          display.display();
        }
      }
      Serial.println("{\"ok\":true,\"written\":" + String(written) + ",\"total\":" + String(otaBytesWritten) + "}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Flash write failed: " + String(Update.errorString()) + "\"}");
    }
    return;
  }

  if (path == "/api/ota/end") {
    if (!otaActive) {
      Serial.println("{\"ok\":false,\"message\":\"OTA not started.\"}");
      return;
    }
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
      Serial.println("{\"ok\":false,\"message\":\"Update failed: " + String(Update.errorString()) + ". Rebooting to restore Bluetooth and Wi-Fi.\"}");
      Serial.flush();
      delay(300);
      ESP.restart();
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
        jsonString(body, "action", action, MAX_ACTION_LEN) && isSupportedAction(action)) {
      saveBinding(profile, slotIndex(slot), action);
      Serial.println("{\"ok\":true,\"message\":\"Assignment saved.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid assignment.\"}");
    }
    return;
  }

  if (path == "/api/profile/save") {
    String profile, values[SLOT_COUNT];
    if (readProfilePayload(body, profile, values)) {
      String* bindings = profileBindings(profile);
      for (int i = 0; i < SLOT_COUNT; ++i) bindings[i] = values[i];
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
      for (int i = 0; i < SLOT_COUNT; ++i) to[i] = from[i];
      saveProfile(target);
      Serial.println("{\"ok\":true,\"message\":\"Assignments copied.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid copy parameters.\"}");
    }
    return;
  }

  if (path == "/api/config/export") {
    Serial.println(buildExportJson());
    return;
  }

  if (path == "/api/config/import") {
    int skipped = 0;
    if (applyConfigImport(body, skipped)) {
      Serial.println(skipped > 0 ? "{\"ok\":true,\"message\":\"Configuration restored (some unsupported assignments were skipped).\"}"
                                 : "{\"ok\":true,\"message\":\"Configuration restored to the device.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Incompatible backup configuration.\"}");
    }
    return;
  }

  if (path == "/api/wifi") {
    String ssid, pass;
    if (jsonString(body, "ssid", ssid, 32) && jsonString(body, "pass", pass, 63)) {
      stored_ssid = ssid; stored_pass = pass;
      prefs.begin("config", false); prefs.putString("ssid", stored_ssid); prefs.putString("pass", stored_pass); prefs.end();
      WiFi.mode(WIFI_STA); WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
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
    if (!tv_enabled) {
      Serial.println("{\"ok\":false,\"message\":\"TV mode is turned off on the pad.\"}");
      return;
    }
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
      String response = sendVizioRequestRaw("PUT", "/pairing/start", "{\"DEVICE_ID\":\"esp32_macropad\",\"DEVICE_NAME\":\"BertoPad\"}", stored_tv_port);
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
    if (jsonString(body, "action", action, MAX_ACTION_LEN) && isSupportedAction(action)) {
      if (executeAction(action)) {
        Serial.println("{\"ok\":true,\"message\":\"Action dispatched.\"}");
      } else {
        if (isBleAction(action)) Serial.println("{\"ok\":false,\"message\":\"Connect Bluetooth before testing this action.\"}");
        else Serial.println(String("{\"ok\":false,\"message\":") + jsonQuote(tvUnavailableMsg()) + "}");
      }
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
    if (jsonString(body, "profile", profile, 6) && validProfile(profile) && profile == "TV" && !tv_enabled) {
      Serial.println("{\"ok\":false,\"message\":\"TV mode is turned off on the pad.\"}");
    } else if (jsonString(body, "profile", profile, 6) && validProfile(profile)) {
      setActiveMode(profile == "TV" ? MODE_TV : MODE_BLE);
      Serial.println("{\"ok\":true,\"message\":\"Profile activated.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Invalid profile.\"}");
    }
    return;
  }

  {
    static const char* const bleCmds[5] = {"disconnect", "connect", "pair", "forget", "settings"};
    for (size_t k = 0; k < 5; ++k) {
      if (path == (String("/api/ble/") + bleCmds[k])) {
        String msg;
        bool ok = bleCommand(bleCmds[k], body, msg);
        Serial.println(String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":" + jsonQuote(msg) + "}");
        return;
      }
    }
  }

  if (path == "/api/motion/set") {
    if (applyMotionSettings(body)) Serial.println("{\"ok\":true,\"message\":\"Motion settings saved.\"}");
    else Serial.println("{\"ok\":false,\"message\":\"enabled and wake must be true/false, sensitivity 1-5.\"}");
    return;
  }

  if (path == "/api/motion/calibrate") {
    String msg; bool ok = motionCalibrate(msg);
    Serial.println(String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":" + jsonQuote(msg) + "}");
    return;
  }

  if (path == "/api/motion/read") {
    Serial.println(motionReadJson());
    return;
  }

  if (path == "/api/display/set") {
    if (applyDisplaySettings(body)) Serial.println("{\"ok\":true,\"message\":\"Display settings saved.\"}");
    else Serial.println("{\"ok\":false,\"message\":\"dim and off must be 0-36000 seconds, contrast 0-255.\"}");
    return;
  }

  if (path == "/api/tv/enable") {
    bool on = true;
    if (jsonFlag(body, "enabled", on)) {
      setTvEnabled(on);
      Serial.println(String("{\"ok\":true,\"enabled\":") + (tv_enabled ? "true" : "false") +
                     ",\"message\":" + jsonQuote(tv_enabled ? "TV mode enabled." : "TV mode disabled.") + "}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Send enabled as true or false.\"}");
    }
    return;
  }

  if (path == "/api/encoder/set") {
    int accel = 1;
    if (jsonInt(body, "accel", accel)) {
      setEncoderAccel(accel != 0);
      Serial.println("{\"ok\":true,\"message\":\"Encoder settings saved.\"}");
    } else {
      Serial.println("{\"ok\":false,\"message\":\"Send accel as 1 or 0.\"}");
    }
    return;
  }

  if (path == "/api/diagnostics/clear") {
    clearDiagnosticsLog();
    Serial.println("{\"ok\":true,\"message\":\"Reset log cleared.\"}");
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
    for (int i = 0; i < SLOT_COUNT; ++i) {
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

  if (pendingBleSlot >= 0) {   // host picker shown while Mode is held
    display.fillRect(0, 10, 128, 54, SSD1306_BLACK); display.drawRect(0, 10, 128, 54, SSD1306_WHITE);
    for (int i = 0; i < MAX_BLE_SLOTS; ++i) {
      int x = 6 + i * 40;
      if (i == pendingBleSlot) display.fillRect(x, 15, 36, 20, SSD1306_WHITE); else display.drawRect(x, 15, 36, 20, SSD1306_WHITE);
      display.setTextColor(i == pendingBleSlot ? SSD1306_BLACK : SSD1306_WHITE); display.setTextSize(2);
      display.setCursor(x + 6, 18); display.print("D"); display.print(i + 1);
    }
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    String nm = dev_names[pendingBleSlot]; if (nm.length() > 20) nm = nm.substring(0, 20);
    display.setCursor((128 - (int)nm.length() * 6) / 2, 40); display.print(nm);
    display.setCursor(13, 52); display.print("Release to switch");
    display.display(); screenDirty = false; return;
  }

  if (millis() < toastExpiry) {
    int width = min(116, (int)strlen(toastMessage) * 6 + 12); int x = (128 - width) / 2;
    display.fillRect(x, 46, width, 15, SSD1306_BLACK); display.drawRect(x, 46, width, 15, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1); display.setCursor(x + 6, 50); display.print(toastMessage);
  }
  display.display(); screenDirty = false;
}

void fireEncoderSlot(int slot) {
  emitEvent(String("{\"event\":\"turn\",\"slot\":\"") + SLOT_IDS[slot] + "\"}");
  String action = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][slot] : tv_bindings[slot]);
  executeAction(action);
}

const char* buttonSlotName(size_t i) {
  uint8_t pin = buttons[i].pin;
  if (pin == PIN_BTN_MODE) return "mode";
  if (pin == PIN_ENC_SW) return SLOT_IDS[SLOT_DIAL];
  if (pin == PIN_ENC2_SW) return SLOT_IDS[SLOT_DIAL2];
  return i < 8 ? SLOT_IDS[i] : "";
}

bool keyHasHold(int k) {
  const String& h = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][SLOT_HOLD0 + k] : tv_bindings[SLOT_HOLD0 + k]);
  return h.length() > 0 && h != "NONE";
}

void fireKeyHold(int k) {
  emitEvent(String("{\"event\":\"hold\",\"slot\":\"") + SLOT_IDS[SLOT_HOLD0 + k] + "\"}");
  executeAction(currentMode == MODE_BLE ? dev_bindings[currentBleSlot][SLOT_HOLD0 + k] : tv_bindings[SLOT_HOLD0 + k]);
}

// Only repeatable "step" actions accelerate; macros, text and app launches never multiply.
bool isAcceleratable(const String& a) {
  return a == "MEDIA:VOL_UP" || a == "MEDIA:VOL_DOWN" || a == "TV:VOL_UP" || a == "TV:VOL_DOWN" ||
         a == "TV:CH_UP" || a == "TV:CH_DOWN";
}

// Detents to emit for this batch. Slow turns stay 1:1; fast spins scale up to 4x (max 12).
int encoderSteps(int detents, const String& action, unsigned long now, unsigned long& lastTime) {
  int steps = min(detents, 8);
  if (encoderAccelEnabled && isAcceleratable(action)) {
    unsigned long dt = now - lastTime;
    if (dt < 1) dt = 1;
    unsigned long rate = ((unsigned long)detents * 1000UL) / dt;   // detents per second
    int mult = (rate >= 40) ? 4 : (rate >= 25) ? 3 : (rate >= 12) ? 2 : 1;
    steps = min(detents * mult, 12);
  }
  lastTime = now;
  return steps;
}

void handleButtonPress(uint8_t pin) {
  if (pin == PIN_BTN_MODE) {
    if (inSetupMode) { inSetupMode = false; screenDirty = true; return; }
    emitEvent("{\"event\":\"press\",\"slot\":\"mode\"}");
    if (!tv_enabled) { showToast(dev_names[currentBleSlot].c_str(), 800); return; } // only Bluetooth profiles exist
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
  else if (pin == PIN_ENC_SW) index = SLOT_DIAL;
  else if (pin == PIN_ENC2_SW) index = SLOT_DIAL2;
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
  encoderAccelEnabled = prefs.getBool("enc_accel", true);
  dispDimSec = prefs.getUShort("disp_dim", 60);
  dispOffSec = prefs.getUShort("disp_off", 300);
  dispContrast = prefs.getUChar("disp_con", 255);
  motionEnabled = prefs.getBool("mot_en", true);
  motionWake = prefs.getBool("mot_wake", true);
  motionSens = prefs.getUChar("mot_sens", 3);
  if (motionSens < 1 || motionSens > 5) motionSens = 3;
  motionDet.sens = motionSens;
  {
    float rx = prefs.getShort("mot_rx", 0) / 1000.0f, ry = prefs.getShort("mot_ry", 0) / 1000.0f, rz = prefs.getShort("mot_rz", 0) / 1000.0f;
    float n = sqrtf(rx * rx + ry * ry + rz * rz);
    if (n > 0.5f) { motionDet.rx = rx / n; motionDet.ry = ry / n; motionDet.rz = rz / n; }
  }
  tv_enabled = prefs.getBool("tv_enabled", true);
  if (!tv_enabled && currentMode == MODE_TV) currentMode = MODE_BLE;
  prefs.end();
  stored_tv_auth.trim();

  prefs.begin("matrix", true);
  // TV Profile
  tv_name = prefs.getString("t_name", "TV");
  for (int i = 0; i < SLOT_COUNT; ++i) {
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

    for (int i = 0; i < SLOT_COUNT; ++i) {
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
void showBootLogo() {
  display.clearDisplay();
  display.drawBitmap(6, 10, bertopad_logo44, BERTOPAD_LOGO_W, BERTOPAD_LOGO_H, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(56, 12); display.print("Berto");
  display.setCursor(56, 30); display.print("Pad");
  display.setTextSize(1);
  display.setCursor(56, 52); display.print("v" FIRMWARE_VERSION);
  display.display();
  delay(1200);
}

void setup() {
  Serial.setRxBufferSize(8192); // must precede begin(); OTA chunks and long macro saves arrive as single lines
  Serial.begin(115200);
  Serial.setTimeout(2000);
  recordBootReason();
  delay(100);
  initStatusLeds();
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) pinMode(buttons[i].pin, INPUT_PULLUP);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP); pinMode(PIN_ENC_DT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), isrEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), isrEncoder, CHANGE);
#if ENCODER2_ENABLED
  // GPIO34-39 are input-only and have no internal pull-ups, so CLK/DT there must be pulled up externally
  pinMode(PIN_ENC2_CLK, (PIN_ENC2_CLK >= 34 && PIN_ENC2_CLK <= 39) ? INPUT : INPUT_PULLUP);
  pinMode(PIN_ENC2_DT,  (PIN_ENC2_DT  >= 34 && PIN_ENC2_DT  <= 39) ? INPUT : INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_CLK), isrEncoder2, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_DT), isrEncoder2, CHANGE);
#endif
  Wire.begin(OLED_SDA, OLED_SCL); Wire.setClock(400000); display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  showBootLogo();
  motionBegin();

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
  bleDeviceName = "BertoPad " + String(currentBleSlot + 1);
  bleKeyboard.setName(bleDeviceName.c_str());

  NimBLEDevice::setSecurityAuth(true, false, true); 
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); 
  bleKeyboard.begin();
#if BLE_MANAGER_ENABLED
  {
    prefs.begin("config", false);
    bleStrict = prefs.getBool("ble_strict", true);
    bool pairBoot = prefs.getBool("pair_boot", false);
    if (pairBoot) prefs.putBool("pair_boot", false);
    prefs.end();
    if (pairBoot) { blePairUntil = millis() + 120000UL; if (blePairUntil == 0) blePairUntil = 1; }   // reopened pairing after "forget"
  }
#endif

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
  applyDisplayContrast();
  showToast(readyMsg, 800);
}

void loop() {
  if (!otaActive) server.handleClient();

  // Web Serial listener for index.html / web app over USB
  // Non-blocking line reader: a partial line can never stall buttons, knobs or the display.
  static String serialRxBuf;
  static bool serialRxOverflow = false;
  static uint32_t serialRxLast = 0;
  if (serialRxBuf.length() > 0 && millis() - serialRxLast > 1500) serialRxBuf = "";   // drop an abandoned partial line
  while (Serial.available()) {
    char c = (char)Serial.read();
    serialRxLast = millis();
    if (c == '\n') {
      bool handle = false;
      if (serialRxOverflow) {
        Serial.println("{\"ok\":false,\"message\":\"Request too large.\"}");
      } else {
        int brace = serialRxBuf.indexOf('{');                // skip stray bytes in front of the JSON
        if (brace > 0) serialRxBuf.remove(0, brace);
        serialRxBuf.trim();
        handle = serialRxBuf.length() > 0 && serialRxBuf.startsWith("{");
      }
      String line = serialRxBuf;
      serialRxBuf = ""; serialRxOverflow = false;
      if (handle) { processSerialPacket(line); break; }     // one packet per loop pass keeps the pad responsive
    } else if (c != '\r' && !serialRxOverflow) {
      if (serialRxBuf.length() >= MAX_REQUEST_BODY) { serialRxOverflow = true; serialRxBuf = ""; }
      else serialRxBuf += c;
    }
  }

  // During a USB stream flash only the serial link is serviced. Buttons, encoder, display
  // and radios stay frozen, and a stalled transfer reboots the pad instead of leaving it stuck.
  if (otaActive) {
    if (millis() - otaLastActivity > OTA_IDLE_TIMEOUT_MS) { Update.abort(); ESP.restart(); }
    delay(1);
    return;
  }

  const bool wifi = WiFi.status() == WL_CONNECTED;
  updateStatusLeds(bleKeyboard.isConnected(), wifi);
  static bool lastBle = false, lastWifi = false;
  const bool ble = bleKeyboard.isConnected();
  if (ble != lastBle || wifi != lastWifi) {
    lastBle = ble; lastWifi = wifi;
    emitEvent(String("{\"event\":\"connection\",\"ble\":") + (ble ? "true" : "false") + ",\"wifi\":" + (wifi ? "true" : "false") + "}");
  }
  if (wifi && !mdnsStarted) mdnsStarted = MDNS.begin("macropad");

  if (encoderDelta != 0) {
    noInterrupts(); int delta = encoderDelta; encoderDelta = 0; interrupts();
    wakeDisplay();

    // Mode button held while rotating dial: cycle Device 1 -> 2 -> 3
    if (digitalRead(PIN_BTN_MODE) == LOW) {
      modeBtnPressStart = 0; // Prevent entering setup portal on release
      // Scrub through the hosts on the OLED; the switch (and reboot) happens when Mode is released
      if (pendingBleSlot < 0) pendingBleSlot = currentBleSlot;
      pendingBleSlot = (pendingBleSlot + (delta > 0 ? 1 : (MAX_BLE_SLOTS - 1))) % MAX_BLE_SLOTS;
      screenDirty = true;
    } else {
      const bool cw = delta > 0;
      int slot = cw ? 9 : 10;
      // Dial held down while turning: use the push_cw / push_ccw binding when one is set
      if (digitalRead(PIN_ENC_SW) == LOW) {
        int chordSlot = cw ? 11 : 12;
        const String& chordAction = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][chordSlot] : tv_bindings[chordSlot]);
        if (chordAction.length() > 0 && chordAction != "NONE") { slot = chordSlot; encChordUsed = true; }
      }
      const String& turnAction = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][slot] : tv_bindings[slot]);
      int steps = encoderSteps(abs(delta), turnAction, millis(), lastEncAccelTime);
      for (int s = 0; s < steps; ++s) fireEncoderSlot(slot);
    }
  }

#if ENCODER2_ENABLED
  if (encoderDelta2 != 0) {
    noInterrupts(); int delta2 = encoderDelta2; encoderDelta2 = 0; interrupts();
    wakeDisplay();
    const int slot2 = (delta2 > 0) ? SLOT_E2_CW : SLOT_E2_CCW;
    const String& turnAction2 = (currentMode == MODE_BLE ? dev_bindings[currentBleSlot][slot2] : tv_bindings[slot2]);
    int steps2 = encoderSteps(abs(delta2), turnAction2, millis(), lastEnc2AccelTime);
    for (int s = 0; s < steps2; ++s) fireEncoderSlot(slot2);
  }
#endif

  unsigned long now = millis();
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
    int reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastState) buttons[i].lastDebounceTime = now;
    if (now - buttons[i].lastDebounceTime > DEBOUNCE_TIME && reading != buttons[i].currentState) {
      buttons[i].currentState = reading;
      wakeDisplay();
      if (reading == HIGH && events_enabled) emitEvent(String("{\"event\":\"release\",\"slot\":\"") + buttonSlotName(i) + "\"}");
      if (buttons[i].pin == PIN_BTN_MODE) {
        if (reading == LOW) { if (!inputLockout) modeBtnPressStart = now; }  // short press fires on release
        else if (pendingBleSlot >= 0) { int target = pendingBleSlot; pendingBleSlot = -1; screenDirty = true; switchBleDevice(target); }
        else if (modeBtnPressStart > 0) { modeBtnPressStart = 0; handleButtonPress(PIN_BTN_MODE); }
      } else if (buttons[i].pin == PIN_ENC_SW) {
        // Dial click fires on release so "hold + turn" can act as a chord instead
        if (reading == LOW) {
          encPressArmed = !inputLockout;
#if BLE_MANAGER_ENABLED
          if (encPressArmed && digitalRead(PIN_BTN_MODE) == LOW) {   // Mode held + dial click: toggle pairing mode
            modeBtnPressStart = 0; encPressArmed = false; encChordUsed = true;
            String m; bleCommand("pair", blePairUntil ? "{\"seconds\":0}" : "{}", m);
          }
#endif
        }
        else { if (encPressArmed && !encChordUsed) handleButtonPress(PIN_ENC_SW); encPressArmed = false; encChordUsed = false; }
      } else if (i < 8) {
        // Action keys. With a hold action bound, the tap fires on release and the hold after KEY_HOLD_MS;
        // without one the key fires instantly on press, exactly as before.
        if (reading == LOW) {
          if (!inputLockout) {
            if (keyHasHold((int)i)) { keyHoldPending[i] = true; keyHoldFired[i] = false; keyPressAt[i] = now; }
            else handleButtonPress(buttons[i].pin);
          }
        } else {
          if (keyHoldPending[i] && !keyHoldFired[i]) handleButtonPress(buttons[i].pin);
          keyHoldPending[i] = false;
        }
      } else if (reading == LOW && !inputLockout) {
        handleButtonPress(buttons[i].pin);
      }
    }
    if (buttons[i].pin == PIN_ENC_SW && reading == HIGH && buttons[i].currentState == HIGH) encChordUsed = false;
    buttons[i].lastState = reading;
  }

  // After a macro abort, ignore the pad until every button has been released
  if (inputLockout) {
    bool anyDown = false;
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) if (buttons[i].currentState == LOW) anyDown = true;
    if (!anyDown) inputLockout = false;
  }

  for (int k = 0; k < 8; ++k) {
    if (inputLockout) { keyHoldPending[k] = false; continue; }
    if (keyHoldPending[k] && !keyHoldFired[k] && buttons[k].currentState == LOW && now - keyPressAt[k] >= KEY_HOLD_MS) {
      keyHoldFired[k] = true;
      fireKeyHold(k);
    }
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
  motionUpdate();
  bleManagerTick();
  updateDisplayPower();
  if (screenDirty && !dispOff) renderScreen();
  delay(1);
}