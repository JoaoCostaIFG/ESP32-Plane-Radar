#pragma once

#include <cstddef>

namespace services::settings {

constexpr size_t kOtaPasswordMaxLen = 32;
constexpr int kTextScaleMinPercent = 80;
constexpr int kTextScaleMaxPercent = 130;
constexpr int kTextScaleDefaultPercent = 110;
constexpr int kNightBrightnessMinPercent = 10;
constexpr int kNightBrightnessMaxPercent = 100;
constexpr int kNightBrightnessDefaultPercent = 40;

/** Load persistent display and OTA settings from NVS. */
void init();

bool footerEnabled();
bool weatherEnabled();
bool temperatureFahrenheit();
bool use24HourClock();
int textScalePercent();
bool nightDimEnabled();
int nightBrightnessPercent();
const char* otaPassword();

/**
 * Store web-portal values. An empty OTA password keeps the current password so
 * the portal never needs to echo the stored secret into its HTML.
 */
void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
                    const char* fahrenheit_checkbox,
                    const char* clock24_checkbox,
                    const char* text_scale_percent_value,
                    const char* ota_password_value,
                    const char* night_dim_checkbox = nullptr,
                    const char* night_brightness_percent_value = nullptr);

/** Restore defaults during a full BOOT-button reset. */
void clear();

}  // namespace services::settings
