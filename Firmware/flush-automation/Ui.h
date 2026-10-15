#ifndef UI_H
#define UI_H

#include <stdint.h>

#define SCREEN_HOME 0
#define SCREEN_SETTINGS 1
#define SCREEN_STATUS 2
#define SCREEN_SETTINGS_NUMBER_EDIT 3
#define SCREEN_SETTINGS_CONFIRM 4
#define SCREEN_TIME_CALIBRATION 5

enum StatusPhase : uint8_t {
  STATUS_EMPTY,
  STATUS_FILL
};

struct Config {
  uint8_t cycles;
  // Fill/empty values are tenths of a second; their limits are seconds.
  uint16_t fill_time;
  uint16_t max_fill_time;
  uint16_t empty_time;
  uint16_t max_empty_time;
  uint32_t actuation_time;
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
void timeCalibrationStart();
void timeCalibrationStop();
bool timeCalibrationNextStep();
void timeCalibrationUpdate();

#endif