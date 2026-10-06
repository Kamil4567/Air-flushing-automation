#ifndef UI_H
#define UI_H

#include <stdint.h>

#define SCREEN_HOME 0
#define SCREEN_SETTINGS 1
#define SCREEN_STATUS 2
#define SCREEN_SETTINGS_BOOL_EDIT 3
#define SCREEN_SETTINGS_NUMBER_EDIT 4
#define SCREEN_SETTINGS_CONFIRM 5

enum StatusPhase : uint8_t {
  STATUS_EMPTY,
  STATUS_FILL,
  STATUS_CALIBRATION
};

struct Config {
  uint8_t cycles;
  // Stored in tenths of a second: 125 means 12.5 seconds.
  uint16_t fill_time;
  bool calibration_at_boot;
  bool calibration_before_procedure;
  uint32_t max_empty_time;
  uint32_t actuation_time;
  uint32_t calibration_time;
  uint16_t pump_current_baseline_tenths_ma;
  bool pump_current_baseline_valid;
};

extern Config config;
extern const Config defaultConfig;
extern int currentScreen;

void uiBegin();
void processInput();
void updateScreen();
void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds);
void updateStatusScreen(StatusPhase phase, uint8_t currentCycle,
                        uint8_t allCycles, uint32_t elapsedSeconds,
                        uint32_t maximumSeconds);
void returnHomeScreen();
void restoreDefaultConfig();

#endif