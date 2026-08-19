#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini / XIAO ESP32-C3, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
// Defaults are for the ESP32-C3 Super Mini. The Seeed XIAO ESP32-C3 env in
// platformio.ini overrides these via -DCONFIG_DISPLAY_PIN_* build flags
// (MOSI=D10, SCLK=D8, DC=D1, CS=D4, RST=D3; D9/MISO unused; BOOT button).
#ifndef CONFIG_DISPLAY_PIN_RST
#define CONFIG_DISPLAY_PIN_RST 0
#endif
#ifndef CONFIG_DISPLAY_PIN_CS
#define CONFIG_DISPLAY_PIN_CS 1
#endif
#ifndef CONFIG_DISPLAY_PIN_DC
#define CONFIG_DISPLAY_PIN_DC 10
#endif
#ifndef CONFIG_DISPLAY_PIN_MOSI
#define CONFIG_DISPLAY_PIN_MOSI 3
#endif
#ifndef CONFIG_DISPLAY_PIN_SCLK
#define CONFIG_DISPLAY_PIN_SCLK 4
#endif

constexpr gpio_num_t kDisplayPinRst = static_cast<gpio_num_t>(CONFIG_DISPLAY_PIN_RST);
constexpr gpio_num_t kDisplayPinCs = static_cast<gpio_num_t>(CONFIG_DISPLAY_PIN_CS);
constexpr gpio_num_t kDisplayPinDc = static_cast<gpio_num_t>(CONFIG_DISPLAY_PIN_DC);
constexpr gpio_num_t kDisplayPinMosi = static_cast<gpio_num_t>(CONFIG_DISPLAY_PIN_MOSI);
constexpr gpio_num_t kDisplayPinSclk = static_cast<gpio_num_t>(CONFIG_DISPLAY_PIN_SCLK);

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- Flight enrichment (origin/destination and detailed aircraft type) ---
constexpr char kFlightDataApiBase[] = "https://api.adsbdb.com/v0/";
/** One lookup at a time; successful results remain cached for six hours. */
constexpr unsigned long kFlightLookupMinIntervalMs = 750UL;
constexpr unsigned long kFlightLookupTimeoutMs = 5000UL;
constexpr unsigned long kFlightLookupFailureBackoffMs = 30000UL;
constexpr unsigned long kFlightCacheSuccessMs = 6UL * 60UL * 60UL * 1000UL;
constexpr unsigned long kFlightCacheMissMs = 10UL * 60UL * 1000UL;

// --- Weather and local time ---
constexpr char kWeatherApiBase[] = "https://api.open-meteo.com/v1/forecast";
constexpr unsigned long kWeatherFetchIntervalMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kWeatherRequestTimeoutMs = 6000UL;

// --- User-facing defaults ---
constexpr char kOtaUsername[] = "admin";
/** Change this in the web settings before exposing the device to other users. */
constexpr char kDefaultOtaPassword[] = "plane-radar";

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

// --- Night dimming (software palette scaling; BLK line is hardwired) ---
/** Local-hour range treated as night: [start, 24) and [0, end). */
constexpr int kNightDimStartHour = 22;
constexpr int kNightDimEndHour = 7;

}  // namespace config
