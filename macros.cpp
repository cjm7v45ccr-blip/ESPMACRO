#include "common.h"

// -----------------------------------------------------------------------------
// Macros: action execution, profile management, JSON helpers
// -----------------------------------------------------------------------------

String jsonQuote(const String& value) {
  String result = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\\' || c == '"') { result += '\\'; result += c; }
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if ((uint8_t)c >= 0x20) result += c;
  }
  result += "\"";
  return result;
}

void addCorsHeaders() {
  // No wildcard origin — API is protected by X-Pad-Token authentication
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
  if (pos >= (int)body.length() || body[pos] != '"') return false;
  ++pos;
  String value = "";
  bool escaped = false;
  for (; pos < (int)body.length(); ++pos) {
    char c = body[pos];
    if (escaped) {
      if (c != '\\' && c != '"' && c != '/' && c != 'n' && c != 'r' && c != 't') return false;
      if (c == 'n') value += '\n';
      else if (c == 'r') value += '\r';
      else if (c == 't') value += '\t';
      else value += c;
      escaped = false;
      continue;
    }
    if (c == '\\') { escaped = true; continue; }
    if (c == '"') { out = value; return value.length() <= maxLength; }
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
  bool quoted = pos < (int)body.length() && body[pos] == '"';
  if (quoted) ++pos;
  int start = pos;
  while (pos < (int)body.length() && isdigit((unsigned char)body[pos])) ++pos;
  if (start == pos || (quoted && (pos >= (int)body.length() || body[pos] != '"'))) return false;
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
  prefs.begin("config", false);
  prefs.putUInt("rev", config_rev);
  prefs.end();
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

  json += "\"profiles\":{\"active\":" + jsonQuote(currentMode == MODE_BLE ? (String("HOST") + String(currentBleSlot + 1)) : "TV") +
          ",\"items\":[" + profileJson("HOST1") + "," + profileJson("HOST2") + "," + profileJson("HOST3") + "," + profileJson("TV") + "]},";

  json += "\"ble_slot\":" + String(currentBleSlot + 1) + ",\"wifi_connected\":" + String(wifi ? "true" : "false") + ",\"ip\":" + jsonQuote(wifi ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"ssid\":" + jsonQuote(stored_ssid) + ",\"tv_ip\":" + jsonQuote(stored_tv_ip) + ",\"tv_port\":" + String(stored_tv_port) + ",\"has_auth\":" + String(stored_tv_auth.length() > 0 ? "true" : "false") + ",";

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
  sendJson(200, "{\"ok\":true,\"message\":\"Configuration restored to the device.\"}");
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
  // Load per-host bond addresses
  for (int h = 0; h < 3; ++h) {
    String key = String("bond_s") + h;
    String addr = prefs.getString(key.c_str(), "");
    if (addr.length() > 0 && addr.length() < 18) {
      snprintf(hostBondAddr[h], sizeof(hostBondAddr[h]), "%s", addr.c_str());
      hostBondKnown[h] = true;
    }
  }
  // Load dim / off times (defaults: dim 60 s, off 300 s)
  g_dimMs = prefs.getULong("dim_ms", 60000);
  g_offMs = prefs.getULong("off_ms", 300000);
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
