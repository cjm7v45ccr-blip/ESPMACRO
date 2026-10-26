#pragma once

// Studio MacroPad hardware configuration for ESP32-WROOM-32.
// Keep credentials out of this file. Configure Wi-Fi and Vizio pairing over
// USB Web Serial (hold Mode for 3 s to enter setup mode); both are stored in
// ESP32 Preferences.

#define FIRMWARE_VERSION "2.1.0"
#define API_VERSION 2

// Hardware pins — 8 action buttons, with Button 9 as the Mode button
#define PIN_BTN1        26
#define PIN_BTN2        27
#define PIN_BTN3        14
#define PIN_BTN4        16
#define PIN_BTN5        17
#define PIN_BTN6        5
#define PIN_BTN7        13
#define PIN_BTN8        4
#define PIN_BTN_MODE    23

#define PIN_ENC_CLK     33
#define PIN_ENC_DT      32
#define PIN_ENC_SW      25

#define PIN_LED_BT      18
#define PIN_LED_WIFI    19

#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_ADDR       0x3C
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

// Reserved: the firmware does not start a Wi-Fi setup access point. Setup is
// USB-only (the OLED shows setup instructions while setup mode is active).
#define SETUP_AP_PREFIX "StudioPad"

// Vizio defaults intentionally contain no user credential or assumed LAN host.
#define DEFAULT_TV_IP   ""
#define DEFAULT_TV_PORT 7345
#define DEFAULT_TV_AUTH ""