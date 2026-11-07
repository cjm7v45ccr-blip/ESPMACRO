#include "common.h"

// -----------------------------------------------------------------------------
// TV: Vizio HTTPS client, pairing, HTTP API handlers, Web Serial, setup web server
// -----------------------------------------------------------------------------

SemaphoreHandle_t tvMutex = nullptr;
QueueHandle_t vizioQueue;

String sendVizioRequestUnlocked(const char* method, const String& path, const String& payload, int portOverride = 0) {
  if (WiFi.status() != WL_CONNECTED || !validHost(stored_tv_ip)) return "";
  const int portToUse = portOverride > 0 ? portOverride : stored_tv_port;
  if (!tvClient.connected() || millis() - lastTvKeepAlive > 15000) {
    tvClient.stop();
    tvClient.useBuiltinCACertBundle();
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

void vizioWorkerTask(void* param) {
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
      Serial.println("{\"ok\":false,\"message\":\"Update failed: \" + String(Update.errorString()) + "\"}");
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
      Serial.println("{\"ok\":false,\"message\":\"Token must be 8–64 characters, or empty to disable HTTP.\"}");
    }
    return;
  }

  if (path == "/api/dim/config") {
    long dimMs = 0, offMs = 0;
    bool hasDim = jsonInt(body, "dim_ms", dimMs);
    bool hasOff = jsonInt(body, "off_ms", offMs);
    if (hasDim && dimMs >= 1000 && dimMs <= 600000)
      g_dimMs = (unsigned long)dimMs;
    if (hasOff && offMs > (long)g_dimMs && offMs <= 3600000)
      g_offMs = (unsigned long)offMs;
    prefs.begin("config", false);
    prefs.putULong("dim_ms", g_dimMs);
    prefs.putULong("off_ms", g_offMs);
    prefs.end();
    Serial.println("{\"ok\":true,\"dim_ms\":" + String(g_dimMs) + ",\"off_ms\":" + String(g_offMs) + "}");
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
