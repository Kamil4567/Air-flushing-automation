#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <hardware/watchdog.h>

#include "UIBitmaps.h"
#include "StatusBitmaps.h"
#include "SettingsBitmaps.h"
#include "ConfigStore.h"
#include "Ui.h"
#include "Procedure.h"

namespace {

constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kSdaPin = 4;
constexpr uint8_t kSclPin = 5;
constexpr uint8_t kEncoderClockPin = 3;
constexpr uint8_t kEncoderDataPin = 6;
constexpr uint8_t kEncoderButtonPin = 7;
constexpr uint8_t kStartButtonPin = 1;
constexpr uint8_t kStopButtonPin = 2;
constexpr int16_t kScreenWidth = 128;
constexpr int16_t kScreenHeight = 64;
constexpr uint32_t kButtonDebounceMs = 25;
constexpr uint32_t kMinimumRedrawIntervalMs = 16;

Adafruit_SSD1306 display(kScreenWidth, kScreenHeight, &Wire, -1);

int cursorPosition = 0;
int selectedOption = -1;
int8_t encoderAccumulator = 0;
uint8_t previousEncoderState = 0;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;
uint32_t lastScreenUpdate = 0;
bool screenDirty = true;
bool displayReady = false;
bool hasDrawnScreen = false;
StatusPhase statusPhase = STATUS_EMPTY;
uint8_t statusCurrentCycle = 1;
uint8_t statusAllCycles = 0;
uint32_t statusElapsedSeconds = 0;
uint32_t statusMaximumSeconds = 0;
uint8_t settingsPosition = 0;
uint8_t boolEditSelection = 0;
uint8_t confirmSelection = 0;
uint32_t numberEditValue = 1;

struct DebouncedButton {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  uint32_t changedAt;
};

DebouncedButton startButton = {kStartButtonPin, HIGH, HIGH, 0};
DebouncedButton stopButton = {kStopButtonPin, HIGH, HIGH, 0};

const int8_t kEncoderTransitions[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};

uint8_t readEncoderState() {
  return (static_cast<uint8_t>(digitalRead(kEncoderClockPin)) << 1) |
         static_cast<uint8_t>(digitalRead(kEncoderDataPin));
}

void markScreenDirty() {
  screenDirty = true;
}

const uint8_t *homeBackground() {
  if (selectedOption == 0) {
    return HOME_SELECTED_0;
  }
  if (selectedOption == 1) {
    return HOME_SELECTED_1;
  }

  switch (cursorPosition) {
    case 1:
      return HOME_CURSOR_1;
    case 2:
      return HOME_CURSOR_2;
    default:
      return HOME_CURSOR_0;
  }
}

void drawDigit(uint8_t digit, int16_t x, int16_t y, bool inverted) {
  const uint16_t foreground = inverted ? SSD1306_BLACK : SSD1306_WHITE;
  const uint16_t background = inverted ? SSD1306_WHITE : SSD1306_BLACK;
  display.drawBitmap(x, y, FONT_DIGITS[digit], 8, 12, foreground, background);
}

void drawHomeScreen() {
  display.drawBitmap(0, 0, homeBackground(), kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);

  const bool cyclesInverted = selectedOption == 0;
  drawDigit(config.cycles / 10, 16, 5, cyclesInverted);
  drawDigit(config.cycles % 10, 26, 5, cyclesInverted);

  const bool fillTimeInverted = selectedOption == 1;
  drawDigit(config.fill_time / 100, 16, 31, fillTimeInverted);
  drawDigit((config.fill_time / 10) % 10, 26, 31, fillTimeInverted);
  drawDigit(config.fill_time % 10, 40, 31, fillTimeInverted);
}

const uint8_t *statusBackground() {
  switch (statusPhase) {
    case STATUS_FILL:
      return STATUS_FILL_BITMAP;
    case STATUS_CALIBRATION:
      return STATUS_CALIBRATION_BITMAP;
    default:
      return STATUS_EMPTY_BITMAP;
  }
}

void drawTimeSeparator(int16_t x, int16_t y) {
  display.drawPixel(x, y + 3, SSD1306_WHITE);
  display.drawPixel(x, y + 8, SSD1306_WHITE);
}

void drawTimeSlash(int16_t x, int16_t y) {
  display.drawLine(x, y + 10, x + 5, y + 2, SSD1306_WHITE);
}

void drawTimeDigits(uint32_t seconds, int16_t firstX, int16_t y,
                    bool includeHours) {
  if (includeHours) {
    const uint32_t hours = seconds / 3600;
    const uint32_t minutes = (seconds / 60) % 60;
    const uint32_t remainingSeconds = seconds % 60;
    drawDigit(hours % 10, firstX, y, false);
    drawTimeSeparator(firstX + 9, y);
    drawDigit(minutes / 10, firstX + 13, y, false);
    drawDigit(minutes % 10, firstX + 23, y, false);
    drawTimeSeparator(firstX + 32, y);
    drawDigit(remainingSeconds / 10, firstX + 36, y, false);
    drawDigit(remainingSeconds % 10, firstX + 46, y, false);
    return;
  }

  const uint32_t minutes = seconds / 60;
  const uint32_t remainingSeconds = seconds % 60;
  drawDigit((minutes / 10) % 10, firstX, y, false);
  drawDigit(minutes % 10, firstX + 10, y, false);
  drawDigit(remainingSeconds / 10, firstX + 23, y, false);
  drawDigit(remainingSeconds % 10, firstX + 33, y, false);
}

void drawStatusScreen() {
  display.drawBitmap(0, 0, statusBackground(), kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);

  drawDigit(statusCurrentCycle / 10, 41, 9, false);
  drawDigit(statusCurrentCycle % 10, 51, 9, false);
  drawDigit(statusAllCycles / 10, 69, 9, false);
  drawDigit(statusAllCycles % 10, 79, 9, false);

  const bool includeHours = statusMaximumSeconds >= 3600;
  if (includeHours) {
    display.fillRect(16, 35, 112, 14, SSD1306_BLACK);
    drawTimeDigits(statusElapsedSeconds, 10, 36, true);
    drawTimeSlash(66, 36);
    drawTimeDigits(statusMaximumSeconds, 73, 36, true);
  } else {
    drawTimeDigits(statusElapsedSeconds, 18, 36, false);
    drawTimeDigits(statusMaximumSeconds, 69, 36, false);
  }

  constexpr int16_t kProgressX = 3;
  constexpr int16_t kProgressY = 54;
  constexpr int16_t kProgressWidth = 122;
  constexpr int16_t kProgressHeight = 7;
  display.fillRect(kProgressX, kProgressY, kProgressWidth, kProgressHeight,
                   SSD1306_BLACK);

  if (statusMaximumSeconds > 0) {
    const uint32_t boundedElapsed =
        statusElapsedSeconds < statusMaximumSeconds
            ? statusElapsedSeconds
            : statusMaximumSeconds;
    const int16_t filledWidth = static_cast<int16_t>(
        (static_cast<uint64_t>(boundedElapsed) * kProgressWidth) /
        statusMaximumSeconds);
    display.fillRect(kProgressX, kProgressY, filledWidth, kProgressHeight,
                     SSD1306_WHITE);
  }
}

const uint8_t *settingsMenuBackground() {
  switch (settingsPosition) {
    case 1: return SETTINGS_PAGE_1;
    case 2: return SETTINGS_PAGE_2;
    case 3: return SETTINGS_PAGE_3;
    case 4: return SETTINGS_PAGE_4;
    case 5: return SETTINGS_PAGE_5;
    case 6: return SETTINGS_PAGE_6;
    case 7: return SETTINGS_PAGE_7;
    default: return SETTINGS_PAGE_0;
  }
}

const uint8_t *boolEditBackground() {
  if (settingsPosition == 1) {
    return boolEditSelection == 0 ? BOOL_BOOT_NO : BOOL_BOOT_YES;
  }
  return boolEditSelection == 0 ? BOOL_BEFORE_NO : BOOL_BEFORE_YES;
}

const uint8_t *confirmBackground() {
  if (settingsPosition == 6) {
    return confirmSelection == 0 ? CONFIRM_RESTORE_NO : CONFIRM_RESTORE_YES;
  }
  return confirmSelection == 0 ? CONFIRM_REBOOT_NO : CONFIRM_REBOOT_YES;
}

void drawNumberEditScreen() {
  const bool editingMaximumEmpty = settingsPosition == 4;
  display.drawBitmap(0, 0,
                     editingMaximumEmpty ? NUMBER_MAX_EMPTY
                                         : NUMBER_CALIBRATION,
                     kScreenWidth, kScreenHeight, SSD1306_WHITE);

  if (editingMaximumEmpty) {
    drawDigit((numberEditValue / 100) % 10, 50, 37, false);
    drawDigit((numberEditValue / 10) % 10, 60, 37, false);
    drawDigit(numberEditValue % 10, 70, 37, false);
  } else {
    drawDigit((numberEditValue / 10) % 10, 55, 37, false);
    drawDigit(numberEditValue % 10, 65, 37, false);
  }
}

bool pollDebouncedButton(DebouncedButton &button, uint32_t now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }

  if (now - button.changedAt < kButtonDebounceMs ||
      reading == button.stableState) {
    return false;
  }

  button.stableState = reading;
  return button.stableState == LOW;
}

void returnToSettings() {
  currentScreen = SCREEN_SETTINGS;
  screenDirty = true;
}

void selectSettingsItem() {
  switch (settingsPosition) {
    case 0:
      returnHomeScreen();
      return;
    case 1:
      boolEditSelection = config.calibration_at_boot ? 1 : 0;
      currentScreen = SCREEN_SETTINGS_BOOL_EDIT;
      break;
    case 2:
      boolEditSelection = config.calibration_before_procedure ? 1 : 0;
      currentScreen = SCREEN_SETTINGS_BOOL_EDIT;
      break;
    case 3:
      procedureStartCalibrationOnly();
      return;
    case 4:
      numberEditValue = config.max_empty_time;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case 5:
      numberEditValue = config.calibration_time;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case 6:
    case 7:
      confirmSelection = 0;
      currentScreen = SCREEN_SETTINGS_CONFIRM;
      break;
  }
  markScreenDirty();
}

enum EncoderRotationResult {
  ENCODER_ACTION_IGNORED,
  ENCODER_ACTION_CHANGED,
  ENCODER_ACTION_LIMIT
};

EncoderRotationResult handleRotation(int8_t direction) {
  if (currentScreen == SCREEN_STATUS) {
    return ENCODER_ACTION_IGNORED;
  }

  if (currentScreen == SCREEN_SETTINGS) {
    const int nextPosition = static_cast<int>(settingsPosition) + direction;
    if (nextPosition < 0 || nextPosition > 7) {
      return ENCODER_ACTION_LIMIT;
    }
    settingsPosition = static_cast<uint8_t>(nextPosition);
  } else if (currentScreen == SCREEN_SETTINGS_BOOL_EDIT) {
    const int nextSelection = static_cast<int>(boolEditSelection) + direction;
    if (nextSelection < 0 || nextSelection > 1) {
      return ENCODER_ACTION_LIMIT;
    }
    boolEditSelection = static_cast<uint8_t>(nextSelection);
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    const uint32_t maximum = settingsPosition == 4 ? 999 : 99;
    const int64_t nextValue = static_cast<int64_t>(numberEditValue) + direction;
    if (nextValue < 1 || nextValue > maximum) {
      return ENCODER_ACTION_LIMIT;
    }
    numberEditValue = static_cast<uint32_t>(nextValue);
  } else if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    const int nextSelection = static_cast<int>(confirmSelection) + direction;
    if (nextSelection < 0 || nextSelection > 1) {
      return ENCODER_ACTION_LIMIT;
    }
    confirmSelection = static_cast<uint8_t>(nextSelection);
  } else if (selectedOption == 0) {
    const int16_t nextValue = static_cast<int16_t>(config.cycles) + direction;
    if (nextValue < 1 || nextValue > 99) {
      return ENCODER_ACTION_LIMIT;
    }
    config.cycles = static_cast<uint8_t>(nextValue);
  } else if (selectedOption == 1) {
    const int32_t nextValue = static_cast<int32_t>(config.fill_time) + direction;
    if (nextValue < 1 || nextValue > 999) {
      return ENCODER_ACTION_LIMIT;
    }
    config.fill_time = static_cast<uint16_t>(nextValue);
  } else {
    cursorPosition = (cursorPosition + direction + 3) % 3;
  }

  markScreenDirty();
  return ENCODER_ACTION_CHANGED;
}

bool handleButtonPress() {
  if (currentScreen == SCREEN_STATUS) {
    return false;
  }

  if (currentScreen == SCREEN_SETTINGS) {
    selectSettingsItem();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_BOOL_EDIT) {
    const bool value = boolEditSelection != 0;
    if (settingsPosition == 1) {
      config.calibration_at_boot = value;
    } else {
      config.calibration_before_procedure = value;
    }
    configStoreRequestSave();
    returnToSettings();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    if (settingsPosition == 4) {
      config.max_empty_time = numberEditValue;
    } else {
      config.calibration_time = numberEditValue;
    }
    configStoreRequestSave();
    returnToSettings();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    if (confirmSelection == 0) {
      returnToSettings();
      return true;
    }

    if (settingsPosition == 6) {
      restoreDefaultConfig();
      returnToSettings();
      return true;
    }

    procedureRequestSoftwareReset();
    return false;
  }

  if (currentScreen == SCREEN_HOME) {
    if (selectedOption >= 0) {
      selectedOption = -1;
      configStoreRequestSave();
    } else if (cursorPosition == 2) {
      currentScreen = SCREEN_SETTINGS;
    } else {
      selectedOption = cursorPosition;
    }
    markScreenDirty();
    return true;
  }

  return false;
}

void pollEncoder() {
  if (currentScreen == SCREEN_STATUS) {
    previousEncoderState = readEncoderState();
    encoderAccumulator = 0;
    return;
  }

  const uint8_t currentState = readEncoderState();
  const uint8_t transition = (previousEncoderState << 2) | currentState;
  previousEncoderState = currentState;
  encoderAccumulator += kEncoderTransitions[transition];

  if (encoderAccumulator >= 4) {
    encoderAccumulator = 0;
    const EncoderRotationResult result = handleRotation(1);
    if (result == ENCODER_ACTION_CHANGED) {
      procedureEncoderTurnBeep();
    } else if (result == ENCODER_ACTION_LIMIT) {
      procedureEncoderErrorBeep();
    }
  } else if (encoderAccumulator <= -4) {
    encoderAccumulator = 0;
    const EncoderRotationResult result = handleRotation(-1);
    if (result == ENCODER_ACTION_CHANGED) {
      procedureEncoderTurnBeep();
    } else if (result == ENCODER_ACTION_LIMIT) {
      procedureEncoderErrorBeep();
    }
  }
}

void pollEncoderButton() {
  const uint32_t now = millis();
  const bool reading = digitalRead(kEncoderButtonPin);

  if (reading != lastButtonReading) {
    lastButtonReading = reading;
    buttonChangedAt = now;
  }

  if (now - buttonChangedAt < kButtonDebounceMs ||
      reading == stableButtonState) {
    return;
  }

  stableButtonState = reading;
  if (stableButtonState == LOW) {
    if (handleButtonPress()) {
      procedureEncoderPressBeep();
    }
  }
}

}  // namespace

const Config defaultConfig = {1, 50, false, true, 30, 500, 10, 0, false};
Config config = defaultConfig;
int currentScreen = SCREEN_HOME;

void uiBegin() {
  Wire.setSDA(kSdaPin);
  Wire.setSCL(kSclPin);
  Wire.begin();

  pinMode(kEncoderClockPin, INPUT_PULLUP);
  pinMode(kEncoderDataPin, INPUT_PULLUP);
  pinMode(kEncoderButtonPin, INPUT_PULLUP);
  pinMode(kStartButtonPin, INPUT_PULLUP);
  pinMode(kStopButtonPin, INPUT_PULLUP);

  previousEncoderState = readEncoderState();
  lastButtonReading = digitalRead(kEncoderButtonPin);
  stableButtonState = lastButtonReading;
  buttonChangedAt = millis();
  startButton.lastReading = digitalRead(kStartButtonPin);
  startButton.stableState = startButton.lastReading;
  startButton.changedAt = millis();
  stopButton.lastReading = digitalRead(kStopButtonPin);
  stopButton.stableState = stopButton.lastReading;
  stopButton.changedAt = millis();

  displayReady = display.begin(SSD1306_SWITCHCAPVCC, kOledAddress);
  markScreenDirty();
}

void processInput() {
  const uint32_t now = millis();
  const bool stopPressed = pollDebouncedButton(stopButton, now);
  const bool startPressed = pollDebouncedButton(startButton, now);

  if (stopPressed) {
    procedureStop();
  } else if (startPressed) {
    procedureStart();
  }

  pollEncoder();
  pollEncoderButton();
}

void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds) {
  currentScreen = SCREEN_STATUS;
  hasDrawnScreen = false;
  updateStatusScreen(phase, currentCycle, allCycles, elapsedSeconds,
                     maximumSeconds);
  screenDirty = true;
}

void updateStatusScreen(StatusPhase phase, uint8_t currentCycle,
                        uint8_t allCycles, uint32_t elapsedSeconds,
                        uint32_t maximumSeconds) {
  if (statusPhase != phase || statusCurrentCycle != currentCycle ||
      statusAllCycles != allCycles ||
      statusElapsedSeconds != elapsedSeconds ||
      statusMaximumSeconds != maximumSeconds) {
    statusPhase = phase;
    statusCurrentCycle = currentCycle;
    statusAllCycles = allCycles;
    statusElapsedSeconds = elapsedSeconds;
    statusMaximumSeconds = maximumSeconds;
    screenDirty = true;
  }
}

void returnHomeScreen() {
  currentScreen = SCREEN_HOME;
  selectedOption = -1;
  cursorPosition = 0;
  screenDirty = true;
}

void restoreDefaultConfig() {
  config = defaultConfig;
  configStoreRequestSave();
}

void updateScreen() {
  if (!displayReady || !screenDirty) {
    return;
  }

  const uint32_t now = millis();
  if (hasDrawnScreen && now - lastScreenUpdate < kMinimumRedrawIntervalMs) {
    return;
  }

  display.clearDisplay();
  if (currentScreen == SCREEN_SETTINGS) {
    display.drawBitmap(0, 0, settingsMenuBackground(), kScreenWidth,
                       kScreenHeight,
                       SSD1306_WHITE);
  } else if (currentScreen == SCREEN_SETTINGS_BOOL_EDIT) {
    display.drawBitmap(0, 0, boolEditBackground(), kScreenWidth, kScreenHeight,
                       SSD1306_WHITE);
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    drawNumberEditScreen();
  } else if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    display.drawBitmap(0, 0, confirmBackground(), kScreenWidth, kScreenHeight,
                       SSD1306_WHITE);
  } else if (currentScreen == SCREEN_STATUS) {
    drawStatusScreen();
  } else {
    drawHomeScreen();
  }
  display.display();

  screenDirty = false;
  hasDrawnScreen = true;
  lastScreenUpdate = now;
}