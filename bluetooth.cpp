#include "common.h"

// -----------------------------------------------------------------------------
// Bluetooth: single-MAC instant host switch, bond tracking
// -----------------------------------------------------------------------------

// Drop every active BLE connection so the pad becomes connectable again.
void bleDropAllLinks() {
  NimBLEServer* pServer = NimBLEDevice::getServer();
  if (!pServer) return;
  std::vector<uint16_t> peers = pServer->getPeerDevices();
  for (uint16_t connHandle : peers) {
    pServer->disconnect(connHandle);
  }
}

// Instant, reboot-free host switch: keep the factory MAC forever, drop the
// current link, re-advertise under the single BertoPad name, and toast the new host.
void switchBleDevice(uint8_t newSlot) {
  if (newSlot >= MAX_BLE_SLOTS) return;

  if (newSlot == currentBleSlot) {
    char msg[24];
    snprintf(msg, sizeof(msg), "Host %d Active", newSlot + 1);
    showToast(msg, 700);
    return;
  }

  prefs.begin("config", false);
  prefs.putUChar("ble_slot", newSlot);
  prefs.end();

  currentBleSlot = newSlot;

  char msg[24];
  snprintf(msg, sizeof(msg), "Host %d", newSlot + 1);
  showToast(msg, 1000);

  // Disconnect the current host, then re-advertise so any host can connect.
  bleDropAllLinks();
  NimBLEDevice::startAdvertising();

  screenDirty = true;
}

// Map a host button pin to a host index (BTN1->0, BTN2->1, BTN3->2)
int buttonToHostIndex(uint8_t pin) {
  if (pin == PIN_BTN1) return 0;
  if (pin == PIN_BTN2) return 1;
  if (pin == PIN_BTN3) return 2;
  return -1;
}

// Save the bonded peer address for a host slot (persisted in Preferences).
void saveHostBondAddr(uint8_t hostIndex, const NimBLEAddress& addr) {
  snprintf(hostBondAddr[hostIndex], sizeof(hostBondAddr[hostIndex]), "%s", addr.toString().c_str());
  hostBondKnown[hostIndex] = true;
  prefs.begin("config", false);
  prefs.putString((String("bond_s") + hostIndex).c_str(), hostBondAddr[hostIndex]);
  prefs.end();
}

// Clear ONLY the bond for the given host slot, leaving other hosts' bonds intact.
void clearHostBond(uint8_t hostIndex) {
  if (!hostBondKnown[hostIndex]) return;
  NimBLEAddress addr(hostBondAddr[hostIndex]);
  NimBLEDevice::deleteBond(addr);
  hostBondAddr[hostIndex][0] = '\0';
  hostBondKnown[hostIndex] = false;
  prefs.begin("config", false);
  prefs.remove((String("bond_s") + hostIndex).c_str());
  prefs.end();
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

void sendBleMedia(const MediaKeyReport key) {
  bleKeyboard.press(key);
  delay(10);
  bleKeyboard.release(key);
}

void sendBleKey(uint8_t key) {
  bleKeyboard.press(key);
  delay(10);
  bleKeyboard.release(key);
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
