#include "common.h"

// -----------------------------------------------------------------------------
// Display: OLED rendering, toast, dim/off/light-sleep power management
// -----------------------------------------------------------------------------

void showToast(const char* msg, unsigned long durationMs = 1000) {
  strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
  toastMessage[sizeof(toastMessage) - 1] = '\0';
  toastExpiry = millis() + durationMs;
  screenDirty = true;
}

void setActiveMode(PadMode mode) {
  currentMode = mode;
  prefs.begin("config", false);
  prefs.putUChar("mode", mode == MODE_TV ? 1 : 0);
  prefs.end();
  String toast = (mode == MODE_BLE ? dev_names[currentBleSlot] : "Profile: TV");
  showToast(toast.c_str(), 800);
  screenDirty = true;
  emitEvent(String("{\"event\":\"profile\",\"active\":\"") + (mode == MODE_BLE ? "MAC" : "TV") + "\"}");
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

  // Apply display power state
  if (g_displayState == DISPLAY_OFF) {
    screenDirty = false;
    return;  // display is off, nothing to draw
  }
  if (g_displayState == DISPLAY_DIM) {
    display.setContrast(40);  // dim
  } else {
    display.setContrast(255);  // full brightness
  }

  if (currentMode == MODE_BLE) {
    display.drawBitmap(0, 0, bitmap_mode_mac, 128, 64, SSD1306_WHITE);
    // Bottom-left host indicators: [1] [2] [3], active one highlighted
    for (int h = 0; h < 3; ++h) {
      int x = 2 + h * 24;
      bool active = (h == currentBleSlot);
      if (active) {
        display.fillRect(x, 32, 18, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.drawRect(x, 32, 18, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_WHITE);
      }
      display.setTextSize(1);
      display.setCursor(x + 4, 34);
      display.print(h + 1);
    }
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
