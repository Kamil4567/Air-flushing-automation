#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <hardware/watchdog.h>

#include "UIBitmaps.h"
#include "HomeV2Bitmaps.h"
#include "TimeCalibrationBitmaps.h"
#include "StatusBitmaps.h"
#include "SettingsBitmaps.h"
#include "SettingsV2Bitmaps.h"
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
constexpr uint8_t kFillButtonPin = 17;
constexpr uint8_t kEmptyButtonPin = 16;
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
uint8_t confirmSelection = 0;
uint32_t numberEditValue = 1;
uint8_t timeCalibrationStep = 0;
uint32_t fillCalibrationElapsedMs = 0;
uint32_t emptyCalibrationElapsedMs = 0;
uint32_t fillCalibrationStartedAt = 0;
uint32_t emptyCalibrationStartedAt = 0;
bool fillCalibrationButtonActive = false;
bool emptyCalibrationButtonActive = false;
uint16_t displayedCalibrationTenths = UINT16_MAX;
uint8_t homeManualOutputMode = 0;
bool homeManualLockout = false;

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

// Odczytuje stan dwóch kanałów enkodera jako dwubitową wartość.
// Argumenty: brak. Zwraca: aktualny stan CLK i DT; nie zmienia stanu UI.
uint8_t readEncoderState() {
  return (static_cast<uint8_t>(digitalRead(kEncoderClockPin)) << 1) |
         static_cast<uint8_t>(digitalRead(kEncoderDataPin));
}

// Oznacza ekran jako wymagający ponownego narysowania.
// Argumenty: brak. Zwraca: brak; ustawia flagę screenDirty.
void markScreenDirty() {
  screenDirty = true;
}

uint16_t currentCalibrationDisplayTenths(uint32_t now);

// Wybiera bitmapę ekranu głównego zgodnie z zaznaczonym polem lub kursorem.
// Argumenty: brak. Zwraca: wskaźnik do bitmapy; nie zmienia stanu UI.
const uint8_t *homeBackground() {
  if (selectedOption == 0) {
    return HOME_V2_SELECTED_0;
  }
  if (selectedOption == 1) {
    return HOME_V2_SELECTED_1;
  }
  if (selectedOption == 2) {
    return HOME_V2_SELECTED_2;
  }

  switch (cursorPosition) {
    case 1:
      return HOME_V2_CURSOR_1;
    case 2:
      return HOME_V2_CURSOR_2;
    case 3:
      return HOME_V2_CURSOR_3;
    default:
      return HOME_V2_CURSOR_0;
  }
}

// Rysuje na OLED jedną cyfrę w podanej pozycji i opcjonalnie odwraca kolory.
// Argumenty: cyfra (0–9), pozycja x/y i flaga inwersji. Zwraca: brak;
// modyfikuje bufor wyświetlacza.
void drawDigit(uint8_t digit, int16_t x, int16_t y, bool inverted) {
  const uint16_t foreground = inverted ? SSD1306_BLACK : SSD1306_WHITE;
  const uint16_t background = inverted ? SSD1306_WHITE : SSD1306_BLACK;
  display.drawBitmap(x, y, FONT_DIGITS[digit], 8, 12, foreground, background);
}

// Rysuje ekran główny z konfiguracją oraz zaznaczeniem aktywnego pola.
// Argumenty: brak. Zwraca: brak; odczytuje config i modyfikuje bufor OLED.
void drawHomeScreen() {
  display.drawBitmap(0, 0, homeBackground(), kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);

  const bool cyclesInverted = selectedOption == 0;
  drawDigit(config.cycles / 10, 16, 2, cyclesInverted);
  drawDigit(config.cycles % 10, 26, 2, cyclesInverted);

  const bool fillTimeInverted = selectedOption == 1;
  drawDigit(config.fill_time / 100, 16, 17, fillTimeInverted);
  drawDigit((config.fill_time / 10) % 10, 26, 17, fillTimeInverted);
  drawDigit(config.fill_time % 10, 40, 17, fillTimeInverted);

  const bool emptyTimeInverted = selectedOption == 2;
  drawDigit(config.empty_time / 100, 16, 34, emptyTimeInverted);
  drawDigit((config.empty_time / 10) % 10, 26, 34, emptyTimeInverted);
  drawDigit(config.empty_time % 10, 40, 34, emptyTimeInverted);
}

// Rysuje ekran aktualnego kroku kalibracji czasu oraz wartość pomiaru.
// Argumenty: brak. Zwraca: brak; odczytuje stan kalibracji i modyfikuje bufor OLED.
void drawTimeCalibrationScreen() {
  const uint8_t *bitmap = TIME_CALIBRATION_STEP_1;
  if (timeCalibrationStep == 2) {
    bitmap = TIME_CALIBRATION_STEP_2;
  } else if (timeCalibrationStep == 3) {
    bitmap = TIME_CALIBRATION_STEP_3;
  }
  display.drawBitmap(0, 0, bitmap, kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);

  if (timeCalibrationStep == 2 || timeCalibrationStep == 3) {
    const uint16_t tenths = currentCalibrationDisplayTenths(millis());
    drawDigit(tenths / 100, 88, 26, true);
    drawDigit((tenths / 10) % 10, 98, 26, true);
    drawDigit(tenths % 10, 112, 26, true);
  }
}

// Wybiera bitmapę tła statusu dla bieżącej fazy procedury.
// Argumenty: brak. Zwraca: wskaźnik do bitmapy; nie zmienia stanu UI.
const uint8_t *statusBackground() {
  switch (statusPhase) {
    case STATUS_FILL:
      return STATUS_FILL_BITMAP;
    default:
      return STATUS_EMPTY_BITMAP;
  }
}

// Rysuje dwukropek w podanej pozycji na ekranie statusu.
// Argumenty: współrzędne x/y. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTimeSeparator(int16_t x, int16_t y) {
  display.drawPixel(x, y + 3, SSD1306_WHITE);
  display.drawPixel(x, y + 8, SSD1306_WHITE);
}

// Rysuje ukośnik oddzielający czasy na ekranie statusu.
// Argumenty: współrzędne x/y. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTimeSlash(int16_t x, int16_t y) {
  display.drawLine(x, y + 10, x + 5, y + 2, SSD1306_WHITE);
}

// Rysuje czas w formacie MM:SS albo H:MM:SS.
// Argumenty: czas w sekundach, początek x, y i wybór formatu godzinowego.
// Zwraca: brak; modyfikuje bufor wyświetlacza.
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

// Rysuje ekran statusu: fazę, numer cyklu, czasy i pasek postępu.
// Argumenty: brak. Zwraca: brak; odczytuje stan statusu i modyfikuje bufor OLED.
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

// Rysuje tekst ustawień i opcjonalne znaki diakrytyczne za pomocą masek bitowych.
// Argumenty: tekst, pozycja, kolory oraz opcjonalne maski akcentów, kropek i ukośników.
// Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawSettingsLine(const char *text, int16_t x, int16_t y,
                      uint16_t foreground, uint16_t background,
                      uint32_t acuteMask = 0, uint32_t dotMask = 0,
                      uint32_t slashMask = 0) {
  display.setTextSize(1);
  display.setTextColor(foreground, background);
  display.setCursor(x, y);
  display.print(text);

  for (uint8_t index = 0; index < 20; ++index) {
    const int16_t markX = x + index * 6;
    if (acuteMask & (1UL << index)) {
      display.drawLine(markX + 2, y - 2, markX + 3, y - 1, foreground);
    }
    if (dotMask & (1UL << index)) {
      display.drawPixel(markX + 2, y - 2, foreground);
    }
    if (slashMask & (1UL << index)) {
      display.drawLine(markX + 1, y + 1, markX + 3, y - 1, foreground);
    }
  }
}

// Rysuje stronę menu ustawień wybraną przez settingsPosition.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawSettingsMenu() {
  static const uint8_t *const pages[] = {
      SETTINGS_V2_PAGE_0, SETTINGS_V2_PAGE_1, SETTINGS_V2_PAGE_2,
      SETTINGS_V2_PAGE_3, SETTINGS_V2_PAGE_4,
  };
  display.drawBitmap(0, 0, pages[settingsPosition], kScreenWidth,
                     kScreenHeight, SSD1306_WHITE);
}

// Zwraca bitmapę aktualnej odpowiedzi w dialogu resetu.
// Argumenty: brak. Zwraca: wskaźnik do bitmapy „nie” lub „tak”.
const uint8_t *confirmBackground() {
  return confirmSelection == 0 ? CONFIRM_REBOOT_NO : CONFIRM_REBOOT_YES;
}

// Rysuje ekran edycji limitu czasu napełniania albo opróżniania.
// Argumenty: brak. Zwraca: brak; odczytuje stan edycji i modyfikuje bufor OLED.
void drawNumberEditScreen() {
  display.drawBitmap(0, 0,
                     NUMBER_MAX_EMPTY,
                     kScreenWidth, kScreenHeight, SSD1306_WHITE);

  if (settingsPosition == 2) {
    display.fillRect(0, 0, kScreenWidth, 25, SSD1306_WHITE);
    drawSettingsLine("MAKSYMALNY CZAS", 19, 4, SSD1306_BLACK,
                     SSD1306_WHITE);
    drawSettingsLine("NAPELNIANIA", 31, 12, SSD1306_BLACK, SSD1306_WHITE,
                     0, 0, 1UL << 4);
  }

  drawDigit((numberEditValue / 100) % 10, 50, 37, false);
  drawDigit((numberEditValue / 10) % 10, 60, 37, false);
  drawDigit(numberEditValue % 10, 70, 37, false);
}

// Odczytuje przycisk z eliminacją drgań styków i wykrywa zbocze wciśnięcia.
// Argumenty: stan przycisku do aktualizacji i bieżący czas w ms.
// Zwraca: true tylko przy potwierdzonym wciśnięciu; modyfikuje przekazany stan.
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

// Wraca do menu ustawień i wymusza odświeżenie ekranu.
// Argumenty: brak. Zwraca: brak; zmienia currentScreen i screenDirty.
void returnToSettings() {
  currentScreen = SCREEN_SETTINGS;
  screenDirty = true;
}

// Uruchamia akcję przypisaną do aktualnej pozycji menu ustawień.
// Argumenty: brak. Zwraca: brak; zmienia ekran lub rozpoczyna kalibrację.
void selectSettingsItem() {
  switch (settingsPosition) {
    case 0:
      returnHomeScreen();
      return;
    case 1:
      timeCalibrationStart();
      return;
    case 2:
      numberEditValue = config.max_fill_time;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case 3:
      numberEditValue = config.max_empty_time;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case 4:
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

// Stosuje obrót enkodera do aktywnego menu, pola lub wartości konfiguracji.
// Argumenty: kierunek obrotu (+1/-1). Zwraca: ignorowanie, zmianę albo osiągnięcie
// limitu; może modyfikować kursor, wybór, numberEditValue lub config.
EncoderRotationResult handleRotation(int8_t direction) {
  if (currentScreen == SCREEN_STATUS ||
      currentScreen == SCREEN_TIME_CALIBRATION) {
    return ENCODER_ACTION_IGNORED;
  }

  if (currentScreen == SCREEN_SETTINGS) {
    const int nextPosition = static_cast<int>(settingsPosition) + direction;
    if (nextPosition < 0 || nextPosition > 4) {
      return ENCODER_ACTION_LIMIT;
    }
    settingsPosition = static_cast<uint8_t>(nextPosition);
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    const uint32_t maximum = settingsPosition == 2 ? 100 : 999;
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
    const uint32_t maximum =
        static_cast<uint32_t>(config.max_fill_time) * 10;
    const uint32_t boundedMaximum = maximum < 999 ? maximum : 999;
    if (nextValue < 1 || nextValue > boundedMaximum) {
      return ENCODER_ACTION_LIMIT;
    }
    config.fill_time = static_cast<uint16_t>(nextValue);
  } else if (selectedOption == 2) {
    const int32_t nextValue = static_cast<int32_t>(config.empty_time) + direction;
    const uint32_t maximum =
        static_cast<uint32_t>(config.max_empty_time) * 10;
    const uint32_t boundedMaximum = maximum < 999 ? maximum : 999;
    if (nextValue < 1 || nextValue > boundedMaximum) {
      return ENCODER_ACTION_LIMIT;
    }
    config.empty_time = static_cast<uint16_t>(nextValue);
  } else {
    cursorPosition = (cursorPosition + direction + 4) % 4;
  }

  markScreenDirty();
  return ENCODER_ACTION_CHANGED;
}

// Obsługuje naciśnięcie enkodera zależnie od aktywnego ekranu.
// Argumenty: brak. Zwraca: czy naciśnięcie zostało obsłużone; może zmieniać ekran,
// konfigurację, rozpoczynać krok kalibracji lub zlecić reset.
bool handleButtonPress() {
  if (currentScreen == SCREEN_STATUS) {
    return false;
  }

  if (currentScreen == SCREEN_TIME_CALIBRATION) {
    return timeCalibrationNextStep();
  }

  if (currentScreen == SCREEN_SETTINGS) {
    selectSettingsItem();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    if (settingsPosition == 2) {
      config.max_fill_time = static_cast<uint16_t>(numberEditValue);
      const uint16_t maximum =
          static_cast<uint16_t>(config.max_fill_time * 10UL);
      if (config.fill_time > maximum) {
        config.fill_time = maximum > 999 ? 999 : maximum;
      }
    } else {
      config.max_empty_time = numberEditValue;
      const uint16_t maximum =
          static_cast<uint16_t>(config.max_empty_time * 10UL);
      if (config.empty_time > maximum) {
        config.empty_time = maximum > 999 ? 999 : maximum;
      }
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

    procedureRequestSoftwareReset();
    return false;
  }

  if (currentScreen == SCREEN_HOME) {
    if (selectedOption >= 0) {
      selectedOption = -1;
      configStoreRequestSave();
    } else if (cursorPosition == 3) {
      currentScreen = SCREEN_SETTINGS;
    } else {
      selectedOption = cursorPosition;
    }
    markScreenDirty();
    return true;
  }

  return false;
}

// Odczytuje kanały enkodera, dekoduje pełny krok i obsługuje jego sygnał dźwiękowy.
// Argumenty: brak. Zwraca: brak; aktualizuje akumulator przejść i stan UI.
void pollEncoder() {
  if (currentScreen == SCREEN_STATUS ||
      currentScreen == SCREEN_TIME_CALIBRATION) {
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

// Odczytuje przycisk enkodera z eliminacją drgań i wywołuje jego akcję.
// Argumenty: brak. Zwraca: brak; aktualizuje stan przycisku i może zmienić UI.
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

// Zeruje postęp i liczniki obu pomiarów kalibracji czasu.
// Argumenty: brak. Zwraca: brak; resetuje wewnętrzny stan kalibracji UI.
void resetTimeCalibrationState() {
  timeCalibrationStep = 0;
  fillCalibrationElapsedMs = 0;
  emptyCalibrationElapsedMs = 0;
  fillCalibrationStartedAt = 0;
  emptyCalibrationStartedAt = 0;
  fillCalibrationButtonActive = false;
  emptyCalibrationButtonActive = false;
  displayedCalibrationTenths = UINT16_MAX;
}

// Zaokrągla czas w ms do dziesiątych części sekundy i ogranicza go do 99,9 s.
// Argumenty: zmierzony czas w ms. Zwraca: czas w dziesiątych sekundy.
uint16_t measuredTimeTenths(uint32_t milliseconds) {
  const uint32_t tenths = (milliseconds + 50) / 100;
  return static_cast<uint16_t>(tenths > 999 ? 999 : tenths);
}

// Dopisuje zakończone przytrzymanie przycisku do czasu kalibracji i wyłącza wyjścia.
// Argumenty: bieżący czas w ms i wybór pomiaru napełniania/opróżniania.
// Zwraca: brak; aktualizuje właściwy licznik i stan przycisku kalibracyjnego.
void accumulateCalibrationHold(uint32_t now, bool filling) {
  bool &buttonActive =
      filling ? fillCalibrationButtonActive : emptyCalibrationButtonActive;
  uint32_t &elapsed =
      filling ? fillCalibrationElapsedMs : emptyCalibrationElapsedMs;
  uint32_t &startedAt =
      filling ? fillCalibrationStartedAt : emptyCalibrationStartedAt;
  if (buttonActive) {
    const uint64_t total = static_cast<uint64_t>(elapsed) + (now - startedAt);
    elapsed = total > 99900 ? 99900 : static_cast<uint32_t>(total);
    buttonActive = false;
  }
  procedureSetManualOutputs(false, false);
}

// Oblicza wyświetlaną wartość kalibracji, uwzględniając trwające przytrzymanie.
// Argumenty: bieżący czas w ms. Zwraca: czas w dziesiątych sekundy; nie zapisuje
// wyniku do konfiguracji.
uint16_t currentCalibrationDisplayTenths(uint32_t now) {
  const bool filling = timeCalibrationStep == 2;
  const uint32_t elapsed =
      filling ? fillCalibrationElapsedMs : emptyCalibrationElapsedMs;
  const bool buttonActive =
      filling ? fillCalibrationButtonActive : emptyCalibrationButtonActive;
  const uint32_t startedAt =
      filling ? fillCalibrationStartedAt : emptyCalibrationStartedAt;
  const uint64_t total =
      static_cast<uint64_t>(elapsed) +
      (buttonActive ? static_cast<uint32_t>(now - startedAt) : 0);
  const uint32_t boundedTotal = total > 99900 ? 99900 : total;
  return measuredTimeTenths(boundedTotal);
}

// Obsługuje ręczne przyciski Z1/Z2 na ekranie głównym i blokadę po zmianie ekranu.
// Argumenty: brak. Zwraca: brak; może zmieniać wyjścia procedury i tryb ręczny.
void updateHomeManualControls() {
  const bool fillPressed = digitalRead(kFillButtonPin) == LOW;
  const bool emptyPressed = digitalRead(kEmptyButtonPin) == LOW;

  if (currentScreen != SCREEN_HOME) {
    if (homeManualOutputMode != 0) {
      procedureSetManualOutputs(false, false);
      homeManualOutputMode = 0;
    }
    homeManualLockout = true;
    return;
  }

  if (homeManualLockout) {
    if (!fillPressed && !emptyPressed) {
      homeManualLockout = false;
    } else {
      return;
    }
  }

  const uint8_t requestedMode =
      fillPressed == emptyPressed ? 0 : (fillPressed ? 1 : 2);
  if (requestedMode == homeManualOutputMode) {
    return;
  }

  procedureSetManualOutputs(requestedMode == 1, requestedMode == 2);
  homeManualOutputMode = requestedMode;
}

}  // namespace

const Config defaultConfig = {1, 50, 100, 300, 30, 500, 0, false};
Config config = defaultConfig;
int currentScreen = SCREEN_HOME;

// Inicjalizuje I2C, wejścia użytkownika, stan debounce oraz wyświetlacz OLED.
// Argumenty: brak. Zwraca: brak; konfiguruje sprzęt i ustawia flagi UI.
void uiBegin() {
  Wire.setSDA(kSdaPin);
  Wire.setSCL(kSclPin);
  Wire.begin();

  pinMode(kEncoderClockPin, INPUT_PULLUP);
  pinMode(kEncoderDataPin, INPUT_PULLUP);
  pinMode(kEncoderButtonPin, INPUT_PULLUP);
  pinMode(kStartButtonPin, INPUT_PULLUP);
  pinMode(kStopButtonPin, INPUT_PULLUP);
  pinMode(kFillButtonPin, INPUT_PULLUP);
  pinMode(kEmptyButtonPin, INPUT_PULLUP);

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

// Wykonuje pojedynczy przebieg obsługi przycisków, enkodera i wejść ręcznych.
// Argumenty: brak. Zwraca: brak; może wywoływać sterowanie procedurą i zmieniać UI.
void processInput() {
  const uint32_t now = millis();
  const bool stopPressed = pollDebouncedButton(stopButton, now);
  const bool startPressed = pollDebouncedButton(startButton, now);

  if (stopPressed) {
    homeManualLockout = true;
    homeManualOutputMode = 0;
    procedureStop();
  } else if (currentScreen == SCREEN_TIME_CALIBRATION) {
    timeCalibrationUpdate();
  } else if (startPressed) {
    procedureStart();
  }

  if (!stopPressed) {
    pollEncoder();
    pollEncoderButton();
  }
  updateHomeManualControls();
}

// Przełącza UI na ekran statusu i inicjuje jego dane.
// Argumenty: faza, numer bieżącego cyklu, liczba cykli, czas upływu i maksimum (s).
// Zwraca: brak; zmienia ekran i dane statusu.
void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds) {
  currentScreen = SCREEN_STATUS;
  hasDrawnScreen = false;
  updateStatusScreen(phase, currentCycle, allCycles, elapsedSeconds,
                     maximumSeconds);
  screenDirty = true;
}

// Aktualizuje dane statusu i oznacza ekran do rysowania, jeśli coś się zmieniło.
// Argumenty: faza, numer bieżącego cyklu, liczba cykli, czas upływu i maksimum (s).
// Zwraca: brak; modyfikuje stan statusu i ewentualnie screenDirty.
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

// Wraca do ekranu głównego, zeruje wybór i kończy aktywny tryb kalibracji czasu.
// Argumenty: brak. Zwraca: brak; zmienia ekran, kursor i flagę odświeżenia.
void returnHomeScreen() {
  if (currentScreen == SCREEN_TIME_CALIBRATION) {
    procedureSetManualOutputs(false, false);
    resetTimeCalibrationState();
  }
  currentScreen = SCREEN_HOME;
  selectedOption = -1;
  cursorPosition = 0;
  screenDirty = true;
}

// Rozpoczyna kalibrację czasów od kroku pomiaru napełniania.
// Argumenty: brak. Zwraca: brak; zeruje pomiary, wyłącza wyjścia i zmienia ekran.
void timeCalibrationStart() {
  procedureSetManualOutputs(false, false);
  resetTimeCalibrationState();
  timeCalibrationStep = 1;
  currentScreen = SCREEN_TIME_CALIBRATION;
  markScreenDirty();
}

// Kończy kalibrację, wyłącza wyjścia i wraca do ekranu głównego.
// Argumenty: brak. Zwraca: brak; resetuje stan kalibracji i blokuje ręczne wyjścia.
void timeCalibrationStop() {
  procedureSetManualOutputs(false, false);
  resetTimeCalibrationState();
  homeManualLockout = true;
  currentScreen = SCREEN_HOME;
  selectedOption = -1;
  cursorPosition = 0;
  markScreenDirty();
}

// Zatwierdza bieżący pomiar albo przechodzi do następnego kroku kalibracji.
// Argumenty: brak. Zwraca: true, gdy krok został przyjęty; false dla pustego lub
// nieaktywnego pomiaru. Aktualizuje config, stan kalibracji i żądanie zapisu.
bool timeCalibrationNextStep() {
  const uint32_t now = millis();
  if (timeCalibrationStep == 1) {
    timeCalibrationStep = 2;
    fillCalibrationElapsedMs = 0;
    markScreenDirty();
    return true;
  }

  if (timeCalibrationStep == 2) {
    accumulateCalibrationHold(now, true);
    const uint16_t measuredTime =
        measuredTimeTenths(fillCalibrationElapsedMs);
    if (measuredTime == 0) {
      procedureEncoderErrorBeep();
      return false;
    }
    config.fill_time = measuredTime;
    const uint16_t requiredMaximum =
        static_cast<uint16_t>((measuredTime + 9) / 10);
    if (config.max_fill_time < requiredMaximum) {
      config.max_fill_time = requiredMaximum;
    }
    configStoreRequestSave();
    timeCalibrationStep = 3;
    emptyCalibrationElapsedMs = 0;
    markScreenDirty();
    return true;
  }

  if (timeCalibrationStep == 3) {
    accumulateCalibrationHold(now, false);
    const uint16_t measuredTime =
        measuredTimeTenths(emptyCalibrationElapsedMs);
    if (measuredTime == 0) {
      procedureEncoderErrorBeep();
      return false;
    }
    config.empty_time = measuredTime;
    const uint16_t requiredMaximum =
        static_cast<uint16_t>((measuredTime + 9) / 10);
    if (config.max_empty_time < requiredMaximum) {
      config.max_empty_time = requiredMaximum;
    }
    configStoreRequestSave();
    timeCalibrationStop();
    return true;
  }

  return false;
}

// Odczytuje przyciski kalibracji, steruje wyjściem i odświeża licznik pomiaru.
// Argumenty: brak. Zwraca: brak; aktualizuje czas, wyjścia i flagę rysowania.
void timeCalibrationUpdate() {
  const uint32_t now = millis();
  if (timeCalibrationStep == 2) {
    const bool pressed = digitalRead(kFillButtonPin) == LOW;
    if (pressed && !fillCalibrationButtonActive) {
      fillCalibrationButtonActive = true;
      fillCalibrationStartedAt = now;
      procedureSetManualOutputs(true, false);
    } else if (!pressed && fillCalibrationButtonActive) {
      accumulateCalibrationHold(now, true);
    }
  } else if (timeCalibrationStep == 3) {
    const bool pressed = digitalRead(kEmptyButtonPin) == LOW;
    if (pressed && !emptyCalibrationButtonActive) {
      emptyCalibrationButtonActive = true;
      emptyCalibrationStartedAt = now;
      procedureSetManualOutputs(false, true);
    } else if (!pressed && emptyCalibrationButtonActive) {
      accumulateCalibrationHold(now, false);
    }
  }

  if (timeCalibrationStep == 2 || timeCalibrationStep == 3) {
    const uint16_t tenths = currentCalibrationDisplayTenths(now);
    if (tenths != displayedCalibrationTenths) {
      displayedCalibrationTenths = tenths;
      markScreenDirty();
    }
  }
}

// Rysuje aktualny ekran tylko wtedy, gdy jest gotowy, zmieniony i minął limit odświeżania.
// Argumenty: brak. Zwraca: brak; aktualizuje bufor OLED i stan synchronizacji ekranu.
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
    drawSettingsMenu();
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    drawNumberEditScreen();
  } else if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    display.drawBitmap(0, 0, confirmBackground(), kScreenWidth, kScreenHeight,
                       SSD1306_WHITE);
  } else if (currentScreen == SCREEN_STATUS) {
    drawStatusScreen();
  } else if (currentScreen == SCREEN_TIME_CALIBRATION) {
    drawTimeCalibrationScreen();
  } else {
    drawHomeScreen();
  }
  display.display();

  screenDirty = false;
  hasDrawnScreen = true;
  lastScreenUpdate = now;
}