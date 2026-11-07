#pragma once

// -----------------------------------------------------------------------------
// Common shared declarations for ESPMacro firmware (Step 4 file split)
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Update.h>
#include "mbedtls/base64.h"

#define USE_NIMBLE
#include <NimBLEDevice.h>
#include <BleKeyboard.h>

#include "config.h"
#include "bitmaps.h"

// -----------------------------------------------------------------------------
// Firmware versioning & defaults
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
extern const char* const SLOT_IDS[11];
extern const char* const DEFAULT_BLANK_BINDINGS[11];
extern const char* const DEFAULT_TV_BINDINGS[11];
extern const char* const SUPPORTED_ACTIONS[];
extern const size_t SUPPORTED_ACTION_COUNT;

enum PadMode { MODE_BLE, MODE_TV };

enum DisplayState { DISPLAY_ON, DISPLAY_DIM, DISPLAY_OFF };

// -----------------------------------------------------------------------------
// Shared globals (defined in esp32_macropad.ino)
// -----------------------------------------------------------------------------
extern String stored_ssid;
extern String stored_pass;
extern String stored_tv_ip;
extern String stored_tv_auth;
extern String api_token;
extern uint32_t config_rev;
extern bool events_enabled;
extern int stored_tv_port;
extern int pairingReqToken;

extern String dev_names[3];
extern String dev_bindings[3][11];
extern String tv_name;
extern String tv_bindings[11];

extern PadMode currentMode;
extern uint8_t currentBleSlot;
extern const char* bleDeviceName;

extern bool inSetupMode;
extern bool setup_done;
extern bool mdnsStarted;

extern size_t otaTotalSize;
extern size_t otaBytesWritten;
extern String setupSsid;
extern String setupPass;

extern Preferences prefs;
extern WebServer server;
extern Adafruit_SSD1306 display;
extern BleKeyboard bleKeyboard;
extern WiFiClientSecure tvClient;
extern unsigned long lastTvKeepAlive;

extern QueueHandle_t vizioQueue;
extern SemaphoreHandle_t tvMutex;

extern char toastMessage[24];
extern unsigned long toastExpiry;
extern bool screenDirty;
extern const unsigned long DEBOUNCE_TIME;
extern unsigned long modeBtnPressStart;

// -----------------------------------------------------------------------------
// Step 3: per-host bond tracking
// -----------------------------------------------------------------------------
extern char hostBondAddr[3][18];
extern bool hostBondKnown[3];
extern unsigned long hostBtnPressStart[3];

// -----------------------------------------------------------------------------
// Step 5: display dim / off / light sleep
// -----------------------------------------------------------------------------
extern DisplayState g_displayState;
extern unsigned long g_lastActivity;
extern unsigned long g_dimMs;
extern unsigned long g_offMs;
extern bool g_sleeping;

// -----------------------------------------------------------------------------
// Encoder state table (defined in esp32_macropad.ino)
// -----------------------------------------------------------------------------
extern volatile int encoderDelta;
extern volatile uint8_t encState;
extern const uint8_t ttable[7][4];

// -----------------------------------------------------------------------------
// Button array (defined in esp32_macropad.ino)
// -----------------------------------------------------------------------------
struct Button {
  uint8_t pin;
  bool currentState;
  bool lastState;
  unsigned long lastDebounceTime;
};
extern Button buttons[];

// -----------------------------------------------------------------------------
// Vizio command types (defined in tv.cpp)
// -----------------------------------------------------------------------------
enum CommandType { CMD_KEY, CMD_APP };
struct VizioCommand {
  CommandType type;
  int codeset;
  int code;
  char appId[8];
  int nameSpace;
  char message[64];
};

// -----------------------------------------------------------------------------
// Function declarations - bluetooth.cpp
// -----------------------------------------------------------------------------
void bleDropAllLinks();
void switchBleDevice(uint8_t newSlot);
int buttonToHostIndex(uint8_t pin);
void saveHostBondAddr(uint8_t hostIndex, const NimBLEAddress& addr);
void clearHostBond(uint8_t hostIndex);
bool isBleAction(const String& action);
bool isTvAction(const String& action);
void sendBleMedia(const MediaKeyReport key);
void sendBleKey(uint8_t key);
void pressModifier(const String& mod);
void pressKey(const String& k);
bool executeShortcut(const String& act);

// -----------------------------------------------------------------------------
// Function declarations - display.cpp
// -----------------------------------------------------------------------------
void renderScreen();
void showToast(const char* msg, unsigned long durationMs = 1000);
void setActiveMode(PadMode mode);

// -----------------------------------------------------------------------------
// Function declarations - macros.cpp
// -----------------------------------------------------------------------------
bool executeAction(const String& act);
bool executeTvKey(const String& act);
bool executeTvApp(const String& act);
bool executeText(const String& act);
void handleEncoder(int direction);
void handleButtonPress(uint8_t pin);
void loadConfiguration();
bool validProfile(const String& profile);
bool validAlias(const String& value);
bool validHost(const String& value);
bool validSsid(const String& value);
bool validPass(const String& value);
bool validPin(const String& value);
bool isSupportedAction(const String& action);
int slotIndex(const String& slot);
int profileToHostIndex(const String& profile);
String& profileName(const String& profile);
String* profileBindings(const String& profile);
const char* profilePrefix(const String& profile);
String deviceId();
void emitEvent(const String& json);
void bumpRev();
void saveProfile(const String& profile);
void saveBinding(const String& profile, int index, const String& action);
void resetProfile(const String& profile);
bool tvIsConfigured();
String profileJson(const String& id);
String buildStatusJson();
void sendStatus();
bool readProfilePayload(const String& body, String& profile, String values[11]);
void handleSaveBinding();
void handleSaveProfile();
void handleRenameProfile();
void handleCopyProfile();
void handleExportConfig();
void handleImportConfig();
bool jsonString(const String& body, const char* key, String& out, size_t maxLength);
bool jsonInt(const String& body, const char* key, int& out);
String jsonQuote(const String& value);
void addCorsHeaders();
void sendJson(int status, const String& payload);
void sendError(int status, const char* code, const char* message);
bool requireAuth();
bool requestBody(String& body);
bool jsonHasKey(const String& body, const char* key);

// -----------------------------------------------------------------------------
// Function declarations - tv.cpp
// -----------------------------------------------------------------------------
String sendVizioRequestUnlocked(const char* method, const String& path, const String& payload, int portOverride = 0);
String sendVizioRequestRaw(const char* method, const String& path, const String& payload, int portOverride = 0);
void vizioWorkerTask(void* param);
bool dispatchKey(int codeset, int code);
bool dispatchApp(const char* appId, int nameSpace, const char* message);
void handleSaveWifi();
void handleSaveTvConfig();
void handleTvTest();
void handlePairStart();
void handlePairSubmit();
void handleForgetTv();
void handleResetDeviceSettings();
void handleTestAction();
void handleTestKey();
void handleTestApp();
void handleActivateProfile();
void setupWebServer();
void processSerialPacket(const String& body);
bool isDeviceConfigured();
void emitSetupStatus();
void startSetupPortal();

// -----------------------------------------------------------------------------
// Function declarations - main .ino
// -----------------------------------------------------------------------------
void setup();
void loop();
void IRAM_ATTR isrEncoder();

// -----------------------------------------------------------------------------
// JSON helpers (defined in macros.cpp)
// -----------------------------------------------------------------------------
const size_t MAX_REQUEST_BODY = 4096;
