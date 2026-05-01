#pragma once

// ── Board selection ───────────────────────────────────────────────
// Uncomment the board you are using. Only one should be active.
#define BOARD_S3_ZERO   // Waveshare ESP32-S3-Zero  (4MB flash, Quad PSRAM)
// #define BOARD_S3_GEEK   // Waveshare ESP32-S3-GEEK  (16MB flash, OPI PSRAM)

// ── Board-specific pin definitions ────────────────────────────────
#if defined(BOARD_S3_GEEK)
  // S3-GEEK: SD is on a dedicated SPI bus, fixed pins
  #define USE_SD
  #define SD_CS   34
  #define SD_SCK  36
  #define SD_MISO 37
  #define SD_MOSI 35
  #define RGB_LED_PIN 25
  // S3-GEEK uses FATFS partition scheme
  #define USE_FFAT

#elif defined(BOARD_S3_ZERO)
  // S3-Zero: no onboard SD — wire a breakout to any free GPIOs
  // Free GPIOs: 1-18, 38-42  (33-37 reserved for PSRAM)
  #define SD_CS    10
  #define SD_SCK   12
  #define SD_MISO  13
  #define SD_MOSI  11
  #define RGB_LED_PIN 21
  // NOTE: Using USB MSC (usbon/usboff) requires these Arduino IDE settings:
  //   USB CDC On Boot → Disabled
  //   USB Mode        → USB-OTG
  // This disables Serial monitor. OTA via WiFi still works.

#else
  // #error "No board selected — uncomment one in config.h"
#endif

// ── Access Point ──────────────────────────────────────────────────
// Factory defaults. These are only used if no config.json exists yet.
// Change them via the admin panel config page after first boot.
#define AP_SSID  "ESP32-Admin"
#define AP_PASS  "admin1234"        // Min 8 chars. "" for open AP.

// ── Station (optional) ────────────────────────────────────────────
// Leave blank to skip STA on first boot.
// Easier to configure via the admin panel config page.
#define STA_SSID ""
#define STA_PASS ""
