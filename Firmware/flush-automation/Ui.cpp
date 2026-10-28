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
#include "Ina219.h"
#include "Lps28dfw.h"
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
constexpr uint8_t kTestPageManual = 0;
constexpr uint8_t kTestPageCutoff = 1;
constexpr uint8_t kTestPagePressure = 2;
constexpr uint8_t kTestPageCalibration = 3;
constexpr uint8_t kTestPageParams = 4;
constexpr uint8_t kTestPageCount = 5;
// Edycja parametrów odcięcia: przeglądanie stron, wybór pola, zmiana wartości.
constexpr uint8_t kParamModeBrowse = 0;
constexpr uint8_t kParamModeSelect = 1;
constexpr uint8_t kParamModeEdit = 2;
constexpr uint8_t kParamRowPressure = 0;
constexpr uint8_t kParamRowMethod = 1;
constexpr uint8_t kParamRowOpen = 2;
constexpr uint8_t kParamRowClosed = 3;
constexpr uint8_t kParamRowRise = 4;
constexpr uint8_t kParamRowFilterType = 5;
constexpr uint8_t kParamRowFilter = 6;
constexpr uint8_t kParamRowConfirm = 7;
constexpr uint8_t kParamRowCount = 8;
constexpr uint8_t kParamVisibleRows = 5;
constexpr int32_t kParamMaxTenths = 30000;
constexpr uint32_t kParamFastTurnMs = 80;

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
bool testModeActive = false;
uint8_t testPage = kTestPageManual;
uint8_t paramMode = kParamModeBrowse;
uint8_t paramCursor = 0;
int32_t paramEditValue = 0;
uint32_t lastParamTurnAt = 0;
PumpTestInfo testInfo = {};
uint8_t testInfoAddress = 0;

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

// Zapisuje wartość w dziesiątych częściach jako tekst "123.4" z opcjonalnym znakiem plus.
// Argumenty: bufor, rozmiar, wartość i flaga wymuszenia znaku. Zwraca: brak.
void formatTenths(char *out, size_t size, int32_t tenths, bool forceSign) {
  const int32_t magnitude = tenths < 0 ? -tenths : tenths;
  snprintf(out, size, "%s%ld.%ld",
           tenths < 0 ? "-" : (forceSign ? "+" : ""),
           static_cast<long>(magnitude / 10),
           static_cast<long>(magnitude % 10));
}

// Rysuje nagłówek strony testowej z tytułem i numerem strony.
// Argumenty: tytuł. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestHeader(const char *title) {
  display.fillRect(0, 0, kScreenWidth, 11, SSD1306_WHITE);
  drawSettingsLine(title, 2, 2, SSD1306_BLACK, SSD1306_WHITE);
  char page[8];
  snprintf(page, sizeof(page), "%u/%u", static_cast<unsigned>(testPage + 1),
           static_cast<unsigned>(kTestPageCount));
  drawSettingsLine(page, 104, 2, SSD1306_BLACK, SSD1306_WHITE);
}

// Rysuje wiersz tekstu strony testowej.
// Argumenty: pozycja y i tekst (maks. 21 znaków). Zwraca: brak.
void drawTestLine(int16_t y, const char *text) {
  drawSettingsLine(text, 1, y, SSD1306_WHITE, SSD1306_BLACK);
}

// Rysuje dolny pasek strony testowej, opcjonalnie w negatywie.
// Argumenty: tekst i flaga negatywu. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestFooter(const char *text, bool inverted) {
  if (inverted) {
    display.fillRect(0, 53, kScreenWidth, 11, SSD1306_WHITE);
    drawSettingsLine(text, 1, 55, SSD1306_BLACK, SSD1306_WHITE);
  } else {
    drawSettingsLine(text, 1, 55, SSD1306_WHITE, SSD1306_BLACK);
  }
}

// Tworzy wiersz "PRAD: x mA" albo opis błędu odczytu z INA219.
// Argumenty: bufor i rozmiar. Zwraca: brak.
void formatTestCurrentLine(char *out, size_t size) {
  if (testInfo.blanked) {
    snprintf(out, size, "PRAD: PAUZA (SILNIK)");
  } else if (!testInfo.hasSample) {
    snprintf(out, size, "PRAD: ...");
  } else if (!testInfo.sensorOk) {
    snprintf(out, size, "PRAD: BLAD I2C");
  } else {
    char value[12];
    formatTenths(value, sizeof(value), testInfo.currentTenthsMa, false);
    snprintf(out, size, "PRAD: %s mA", value);
  }
}

// Rysuje stronę 1 trybu testowego: duży odczyt prądu i stan sterowania ręcznego.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestManualPage() {
  drawTestHeader("TRYB TESTOWY");

  if (ina219_address() != 0) {
    char status[24];
    snprintf(status, sizeof(status), "INA219 0x%02X OK", ina219_address());
    drawSettingsLine(status, 18, 15, SSD1306_WHITE, SSD1306_BLACK);
  } else {
    drawSettingsLine("INA219: BRAK I2C", 12, 15, SSD1306_WHITE,
                     SSD1306_BLACK);
  }

  char pressureText[28];
  if (testInfo.hasPressure && testInfo.pressureOk) {
    char pressureValue[12];
    formatTenths(pressureValue, sizeof(pressureValue),
                 testInfo.pressureTenthsHpa, false);
    snprintf(pressureText, sizeof(pressureText), "LPS28: %s hPa", pressureValue);
  } else {
    snprintf(pressureText, sizeof(pressureText), "LPS28: BRAK I2C");
  }
  drawSettingsLine(pressureText, 12, 23, SSD1306_WHITE, SSD1306_BLACK);

  char text[16];
  if (testInfo.blanked) {
    snprintf(text, sizeof(text), "PAUZA");
  } else if (!testInfo.hasSample) {
    snprintf(text, sizeof(text), "...");
  } else if (!testInfo.sensorOk) {
    snprintf(text, sizeof(text), "BLAD I2C");
  } else {
    char value[12];
    formatTenths(value, sizeof(value), testInfo.currentTenthsMa, false);
    snprintf(text, sizeof(text), "%s mA", value);
  }
  const int16_t textWidth = static_cast<int16_t>(strlen(text)) * 12;
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setCursor((kScreenWidth - textWidth) / 2, 32);
  display.print(text);

  if (homeManualOutputMode == 1) {
    drawSettingsLine("NAPELNIANIE", 31, 54, SSD1306_WHITE, SSD1306_BLACK, 0, 0,
                     1UL << 4);
  } else if (homeManualOutputMode == 2) {
    drawSettingsLine("OPROZNIANIE", 31, 54, SSD1306_WHITE, SSD1306_BLACK,
                     1UL << 3, 1UL << 4);
  } else {
    drawSettingsLine("---", 55, 54, SSD1306_WHITE, SSD1306_BLACK);
  }
}

// Rysuje stronę 2 trybu testowego: dane do diagnostyki odcięcia pompy.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestCutoffPage() {
  drawTestHeader("ODCIECIE PRADOWE");

  char line[28];
  char value[12];
  formatTestCurrentLine(line, sizeof(line));
  drawTestLine(13, line);

  if (testInfo.baselineValid) {
    char blocked[12];
    formatTenths(value, sizeof(value), testInfo.baselineTenthsMa, false);
    formatTenths(blocked, sizeof(blocked), testInfo.blockedTenthsMa, false);
    snprintf(line, sizeof(line), "OTW:%s ZAM:%s", value, blocked);
  } else {
    snprintf(line, sizeof(line), "OTW/ZAM: BRAK");
  }
  drawTestLine(21, line);

  if (testInfo.baselineValid) {
    formatTenths(value, sizeof(value), testInfo.levelTenthsMa, false);
    snprintf(line, sizeof(line), "PROG: %s mA", value);
  } else {
    snprintf(line, sizeof(line), "PROG: --");
  }
  drawTestLine(29, line);

  if (testInfo.baselineValid && testInfo.sensorOk) {
    char limit[12];
    formatTenths(value, sizeof(value), testInfo.deltaTenthsMa, true);
    formatTenths(limit, sizeof(limit), testInfo.deltaLimitTenthsMa, false);
    snprintf(line, sizeof(line), "NARAST:%s/%s", value, limit);
  } else {
    snprintf(line, sizeof(line), "NARAST: --");
  }
  drawTestLine(37, line);

  snprintf(line, sizeof(line), "SPELN.:%u/%u POMPA:%s",
           static_cast<unsigned>(testInfo.consecutiveSamples),
           static_cast<unsigned>(testInfo.confirmSamples),
           testInfo.pumpOn ? "ON" : "OFF");
  drawTestLine(45, line);

  if (!testInfo.cutoffEnabled) {
    drawTestFooter("ODCIECIE: WYLACZONE", false);
  } else if (!testInfo.baselineValid) {
    drawTestFooter("BRAK BAZY-KALIBRUJ", false);
  } else if (testInfo.cutoffDetected) {
    drawTestFooter("ODCIECIE: WYKRYTE!", true);
  } else {
    drawTestFooter("ODCIECIE: NIE", false);
  }
}

// Rysuje stronę 3 trybu testowego: przebieg kalibracji bazy prądu krok po kroku.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestCalibrationPage() {
  drawTestHeader("KALIBRACJA PRADU");

  char line[28];
  char value[12];
  char value2[12];
  formatTestCurrentLine(line, sizeof(line));
  drawTestLine(13, line);

  const uint8_t phase = testInfo.calPhase;
  const bool running = phase == TEST_CAL_SPINUP || phase == TEST_CAL_OPEN ||
                       phase == TEST_CAL_CLOSED || phase == TEST_CAL_PAUSE;

  if (running) {
    const char *phaseName = phase == TEST_CAL_SPINUP
                                ? "ROZRUCH"
                                : (phase == TEST_CAL_OPEN
                                       ? "ZAW.OTWARTY"
                                       : (phase == TEST_CAL_CLOSED
                                              ? "ZAW.ZAMKN."
                                              : "PRZERWA"));
    snprintf(line, sizeof(line), "P%u/%u %s",
             static_cast<unsigned>(testInfo.calRun),
             static_cast<unsigned>(testInfo.calRuns), phaseName);
    drawTestLine(21, line);
    snprintf(line, sizeof(line), "POZOSTALO: %lu.%lu s",
             static_cast<unsigned long>(testInfo.calRemainingTenths / 10),
             static_cast<unsigned long>(testInfo.calRemainingTenths % 10));
    drawTestLine(29, line);

    if (phase == TEST_CAL_PAUSE) {
      snprintf(line, sizeof(line), "POPRAWNE: %u/%u",
               static_cast<unsigned>(testInfo.calValidRuns),
               static_cast<unsigned>(testInfo.calRun - 1));
      drawTestLine(37, line);
    } else if (testInfo.calSampleCount > 0) {
      formatTenths(value, sizeof(value), testInfo.calAverageTenthsMa, false);
      snprintf(line, sizeof(line), "PROBKI:%lu SR:%s",
               static_cast<unsigned long>(testInfo.calSampleCount), value);
      drawTestLine(37, line);
    } else {
      drawTestLine(37, "PROBKI: 0");
    }

    if (phase == TEST_CAL_CLOSED) {
      formatTenths(value, sizeof(value), testInfo.calDeltaTenthsMa, false);
      snprintf(line, sizeof(line), "MAX NARAST: %s/0.3s", value);
      drawTestLine(45, line);
    } else if (phase != TEST_CAL_PAUSE) {
      formatTenths(value, sizeof(value), testInfo.calMinTenthsMa, false);
      formatTenths(value2, sizeof(value2), testInfo.calMaxTenthsMa, false);
      snprintf(line, sizeof(line), "MIN:%s MAX:%s", value, value2);
      drawTestLine(45, line);
    }
  } else if (phase == TEST_CAL_DONE || phase == TEST_CAL_IDLE) {
    if (phase == TEST_CAL_DONE) {
      snprintf(line, sizeof(line), "SREDNIA Z %u/%u PRZEB.",
               static_cast<unsigned>(testInfo.calValidRuns),
               static_cast<unsigned>(testInfo.calRuns));
      drawTestLine(21, line);
    } else {
      drawTestLine(21, "KALIBRACJA: GOTOWA");
    }
    if (testInfo.baselineValid) {
      formatTenths(value, sizeof(value), testInfo.baselineTenthsMa, false);
      snprintf(line, sizeof(line), "OTWARTY: %s mA", value);
      drawTestLine(29, line);
      formatTenths(value, sizeof(value), testInfo.blockedTenthsMa, false);
      snprintf(line, sizeof(line), "ZAMKNIETY: %s mA", value);
      drawTestLine(37, line);
      formatTenths(value, sizeof(value), testInfo.calDeltaTenthsMa, false);
      snprintf(line, sizeof(line), "MAX NARAST: %s/0.3s", value);
      drawTestLine(45, line);
    } else {
      drawTestLine(29, "BRAK KALIBRACJI");
      drawTestLine(37, "5 PRZEBIEGOW, ~75 S");
    }
  } else {
    drawTestLine(21, testInfo.calSampleCount == 0 && testInfo.calOpenTenthsMa == 0
                         ? "BLAD: BRAK ODCZYTU"
                         : "BLAD: ZA MALY WZROST");
    char open[12];
    char closed[12];
    formatTenths(open, sizeof(open), testInfo.calOpenTenthsMa, false);
    formatTenths(closed, sizeof(closed), testInfo.calBlockedTenthsMa, false);
    snprintf(line, sizeof(line), "OTW:%s ZAM:%s", open, closed);
    drawTestLine(29, line);
    formatTenths(value, sizeof(value),
                 testInfo.calBlockedTenthsMa - testInfo.calOpenTenthsMa, true);
    formatTenths(value2, sizeof(value2), testInfo.calRequiredTenthsMa, false);
    snprintf(line, sizeof(line), "WZROST:%s WYM:%s", value, value2);
    drawTestLine(37, line);
    snprintf(line, sizeof(line), "POPRAWNE: %u/%u (MIN %u)",
             static_cast<unsigned>(testInfo.calValidRuns),
             static_cast<unsigned>(testInfo.calRuns),
             static_cast<unsigned>(testInfo.calRuns / 2 + 1));
    drawTestLine(45, line);
  }

  drawTestFooter(running ? "ENK./STOP: PRZERWIJ" : "ENKODER: START",
                 running);
}

// Rysuje stronę 3 trybu testowego: ciśnienie z czujnika LPS28DFW, próg i stan odcięcia
// ciśnieniowego.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestPressurePage() {
  drawTestHeader("CISNIENIE");

  char line[28];
  char value[12];
  if (lps28dfw_address() != 0) {
    snprintf(line, sizeof(line), "LPS28DFW 0x%02X OK", lps28dfw_address());
  } else {
    snprintf(line, sizeof(line), "LPS28DFW: BRAK I2C");
  }
  drawTestLine(13, line);

  if (testInfo.hasPressure && testInfo.pressureOk) {
    formatTenths(value, sizeof(value), testInfo.pressureTenthsHpa, false);
    const int16_t width = static_cast<int16_t>(strlen(value)) * 12;
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.setCursor(4, 23);
    display.print(value);
    drawSettingsLine("hPa", 4 + width + 4, 30, SSD1306_WHITE, SSD1306_BLACK);
  } else {
    drawSettingsLine(testInfo.hasPressure ? "BLAD ODCZYTU" : "...", 4, 27,
                     SSD1306_WHITE, SSD1306_BLACK);
  }

  snprintf(line, sizeof(line), "PROG: %ld hPa",
           static_cast<long>(testInfo.pressureThresholdHpa));
  drawTestLine(39, line);
  snprintf(line, sizeof(line), "PONIZEJ: %u/%u PROB",
           static_cast<unsigned>(testInfo.pressureLowSamples),
           static_cast<unsigned>(testInfo.confirmSamples));
  drawTestLine(46, line);

  if (!testInfo.pressureCutoffEnabled) {
    drawTestFooter("CISN. NIEUZYWANE", false);
  } else if (testInfo.pressureCutoffDetected) {
    drawTestFooter("ODCIECIE: WYKRYTE!", true);
  } else {
    drawTestFooter("ODCIECIE: NIE", false);
  }
}

// Zwraca zapisany parametr w jednostkach edycji: pole 0 to próg ciśnienia (hPa), pole 1
// to metoda odcięcia, pola 2-4 to prąd w dziesiątych mA (przy otwartym zaworze, przy
// zamkniętym i największy narost w 300 ms), pole 5 to typ filtra prądu, pole 6 to liczba
// odczytów filtra prądu, pole 7 to liczba próbek potwierdzających odcięcie.
// Argumenty: numer pola. Zwraca: wartość z konfiguracji.
int32_t paramStoredValue(uint8_t row) {
  switch (row) {
    case kParamRowPressure:
      return config.pressure_cutoff_hpa;
    case kParamRowMethod:
      return config.cutoff_method;
    case kParamRowOpen:
      return config.pump_current_baseline_tenths_ma;
    case kParamRowClosed:
      return config.pump_current_blocked_tenths_ma;
    case kParamRowRise:
      return config.pump_current_rise_tenths_ma;
    case kParamRowFilterType:
      return config.current_filter_type;
    case kParamRowFilter:
      return config.current_filter_samples;
    default:
      return config.cutoff_confirm_samples;
  }
}

// Sprawdza, czy pole jest prądem w dziesiątych mA (wyświetlanym z częścią dziesiętną).
// Argumenty: numer pola. Zwraca: true dla pól otwarty/zamknięty/narost.
bool paramIsCurrent(uint8_t row) {
  return row >= kParamRowOpen && row <= kParamRowRise;
}

// Dolna granica edycji pola.
// Argumenty: numer pola. Zwraca: najmniejszą dozwoloną wartość.
int32_t paramMinValue(uint8_t row) {
  switch (row) {
    case kParamRowPressure:
      return PRESSURE_CUTOFF_MIN_HPA;
    case kParamRowFilter:
    case kParamRowConfirm:
      return 1;
    default:
      return 0;
  }
}

// Górna granica edycji pola.
// Argumenty: numer pola. Zwraca: największą dozwoloną wartość.
int32_t paramMaxValue(uint8_t row) {
  switch (row) {
    case kParamRowPressure:
      return PRESSURE_CUTOFF_MAX_HPA;
    case kParamRowMethod:
      return CUTOFF_METHOD_COUNT - 1;
    case kParamRowFilterType:
      return FILTER_TYPE_COUNT - 1;
    case kParamRowFilter:
      return MAX_CURRENT_FILTER_SAMPLES;
    case kParamRowConfirm:
      return MAX_CUTOFF_CONFIRM_SAMPLES;
    default:
      return kParamMaxTenths;
  }
}

// Krok zmiany wartości pola przy obrocie enkodera.
// Argumenty: numer pola i flaga szybkiego obrotu. Zwraca: krok (prąd: 1 mA lub 10 mA,
// ciśnienie: 1 lub 10 hPa, pozostałe pola: 1).
int32_t paramStep(uint8_t row, bool fast) {
  if (paramIsCurrent(row)) {
    return fast ? 100 : 10;
  }
  return row == kParamRowPressure && fast ? 10 : 1;
}

// Zapisuje parametr do konfiguracji. Parametry prądu oznaczają kalibrację jako ustawioną ręcznie.
// Argumenty: numer pola i wartość w jednostkach edycji. Zwraca: brak; żąda zapisu do EEPROM
// i przekazuje nowe wartości do pomiaru ciśnienia i prądu oraz detekcji odcięcia.
void paramCommit(uint8_t row, int32_t value) {
  switch (row) {
    case kParamRowPressure:
      config.pressure_cutoff_hpa = static_cast<uint16_t>(value);
      break;
    case kParamRowMethod:
      config.cutoff_method = static_cast<uint8_t>(value);
      break;
    case kParamRowOpen:
      config.pump_current_baseline_tenths_ma = static_cast<uint16_t>(value);
      break;
    case kParamRowClosed:
      config.pump_current_blocked_tenths_ma = static_cast<uint16_t>(value);
      break;
    case kParamRowRise:
      config.pump_current_rise_tenths_ma = static_cast<uint16_t>(value);
      break;
    case kParamRowFilterType:
      config.current_filter_type = static_cast<uint8_t>(value);
      break;
    case kParamRowFilter:
      config.current_filter_samples = static_cast<uint8_t>(value);
      break;
    default:
      config.cutoff_confirm_samples = static_cast<uint8_t>(value);
      break;
  }
  if (paramIsCurrent(row)) {
    config.pump_current_baseline_valid = true;
  }
  configStoreRequestSave();
  procedureApplyCutoffConfig();
}

// Rysuje stronę 5 trybu testowego: ręczna edycja progu ciśnienia, metody odcięcia,
// parametrów odcięcia prądowego i filtra prądu.
// Widoczne jest 5 pól naraz; lista przewija się razem z kursorem.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestParamsPage() {
  static const char *const labels[kParamRowCount] = {
      "PROG CISN[hPa]:", "ODCIECIE:", "OTWARTY[mA]:", "ZAMKNIETY[mA]:",
      "NARAST/0.3s:", "FILTR:", "FILTR[PROBKI]:", "PROBY ODCIECIA:"};
  static const char *const filterNames[FILTER_TYPE_COUNT] = {
      "SREDNIA", "MEDIANA", "SR.OBCIETA"};
  static const char *const methodNames[CUTOFF_METHOD_COUNT] = {
      "CISNIENIE", "PRAD", "OBA"};
  drawTestHeader("PARAMETRY");

  uint8_t firstRow = 0;
  if (paramMode != kParamModeBrowse && paramCursor >= kParamVisibleRows) {
    firstRow = paramCursor - kParamVisibleRows + 1;
    if (firstRow > kParamRowCount - kParamVisibleRows) {
      firstRow = kParamRowCount - kParamVisibleRows;
    }
  }

  for (uint8_t slot = 0; slot < kParamVisibleRows; ++slot) {
    const uint8_t row = firstRow + slot;
    const int16_t y = 12 + slot * 8;
    const bool selected = paramMode != kParamModeBrowse && paramCursor == row;
    const bool editing = paramMode == kParamModeEdit && selected;
    const uint16_t foreground = editing ? SSD1306_BLACK : SSD1306_WHITE;
    const uint16_t background = editing ? SSD1306_WHITE : SSD1306_BLACK;

    char value[12];
    const int32_t shown = editing ? paramEditValue : paramStoredValue(row);
    if (row == kParamRowMethod) {
      snprintf(value, sizeof(value), "%s",
               methodNames[shown < CUTOFF_METHOD_COUNT ? shown : 0]);
    } else if (row == kParamRowFilterType) {
      snprintf(value, sizeof(value), "%s",
               filterNames[shown < FILTER_TYPE_COUNT ? shown : 0]);
    } else if (paramIsCurrent(row)) {
      formatTenths(value, sizeof(value), shown, false);
    } else {
      snprintf(value, sizeof(value), "%ld", static_cast<long>(shown));
    }
    if (editing) {
      display.fillRect(7, y - 1, 121, 9, SSD1306_WHITE);
    }
    if (selected) {
      drawSettingsLine(">", 1, y, SSD1306_WHITE, SSD1306_BLACK);
    }
    drawSettingsLine(labels[row], 8, y, foreground, background);
    drawSettingsLine(value,
                     126 - static_cast<int16_t>(strlen(value)) * 6, y,
                     foreground, background);
  }

  // Dolny wiersz: podpowiedź przy edycji i pozycji POWROT, w pozostałych przypadkach
  // ciśnienie i prąd.
  if (paramMode == kParamModeEdit) {
    drawTestFooter("OBROT / ENK: ZAPISZ", true);
  } else if (paramMode == kParamModeSelect && paramCursor >= kParamRowCount) {
    drawTestFooter("> POWROT", true);
  } else {
    char line[28];
    char value[12];
    if (testInfo.hasPressure && testInfo.pressureOk) {
      formatTenths(value, sizeof(value), testInfo.pressureTenthsHpa, false);
      snprintf(line, sizeof(line), "P:%s hPa", value);
    } else {
      snprintf(line, sizeof(line), "P: BRAK");
    }
    drawTestLine(54, line);
  }
}

// Rysuje aktualną stronę trybu testowego.
// Argumenty: brak. Zwraca: brak; modyfikuje bufor wyświetlacza.
void drawTestScreen() {
  if (testPage == kTestPageCutoff) {
    drawTestCutoffPage();
  } else if (testPage == kTestPagePressure) {
    drawTestPressurePage();
  } else if (testPage == kTestPageCalibration) {
    drawTestCalibrationPage();
  } else if (testPage == kTestPageParams) {
    drawTestParamsPage();
  } else {
    drawTestManualPage();
  }
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

  if (currentScreen == SCREEN_TEST) {
    if (testPage == kTestPageParams && paramMode == kParamModeSelect) {
      const int nextCursor = static_cast<int>(paramCursor) + direction;
      if (nextCursor < 0 || nextCursor > kParamRowCount) {
        return ENCODER_ACTION_LIMIT;
      }
      paramCursor = static_cast<uint8_t>(nextCursor);
    } else if (testPage == kTestPageParams && paramMode == kParamModeEdit) {
      // Prąd: szybki obrót zmienia wartość o 10 mA, wolny o 1 mA; filtr: o 1 odczyt.
      const uint32_t now = millis();
      const int32_t step =
          paramStep(paramCursor, now - lastParamTurnAt < kParamFastTurnMs);
      lastParamTurnAt = now;
      int32_t nextValue = paramEditValue + direction * step;
      const int32_t minValue = paramMinValue(paramCursor);
      const int32_t maxValue = paramMaxValue(paramCursor);
      nextValue = nextValue < minValue
                      ? minValue
                      : (nextValue > maxValue ? maxValue : nextValue);
      if (nextValue == paramEditValue) {
        return ENCODER_ACTION_LIMIT;
      }
      paramEditValue = nextValue;
    } else {
      const int nextPage = static_cast<int>(testPage) + direction;
      if (nextPage < 0 || nextPage >= kTestPageCount) {
        return ENCODER_ACTION_LIMIT;
      }
      if (testPage == kTestPageCalibration) {
        procedureTestCalibrationAbort();
      }
      testPage = static_cast<uint8_t>(nextPage);
    }
  } else if (currentScreen == SCREEN_SETTINGS) {
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

  if (currentScreen == SCREEN_TEST) {
    if (testPage == kTestPageParams) {
      if (paramMode == kParamModeBrowse) {
        paramMode = kParamModeSelect;
        paramCursor = 0;
      } else if (paramMode == kParamModeSelect) {
        if (paramCursor >= kParamRowCount) {
          paramMode = kParamModeBrowse;
        } else {
          paramEditValue = paramStoredValue(paramCursor);
          lastParamTurnAt = millis();
          paramMode = kParamModeEdit;
        }
      } else {
        paramCommit(paramCursor, paramEditValue);
        paramMode = kParamModeSelect;
      }
      markScreenDirty();
      return true;
    }

    if (testPage == kTestPageCalibration) {
      if (procedureTestCalibrationRunning()) {
        procedureTestCalibrationAbort();
      } else {
        procedureTestCalibrationStart();
      }
    }
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

// Sprawdza, czy ręczne przyciski Z1/Z2 mogą sterować wyjściami na bieżącym ekranie.
// Argumenty: brak. Zwraca: true na ekranie głównym i stronach testu poza kalibracją.
bool manualControlsAllowed() {
  return currentScreen == SCREEN_HOME ||
         (currentScreen == SCREEN_TEST && testPage != kTestPageCalibration);
}

// Obsługuje ręczne przyciski Z1/Z2 na ekranie głównym i blokadę po zmianie ekranu.
// Argumenty: brak. Zwraca: brak; może zmieniać wyjścia procedury i tryb ręczny.
void updateHomeManualControls() {
  const bool fillPressed = digitalRead(kFillButtonPin) == LOW;
  const bool emptyPressed = digitalRead(kEmptyButtonPin) == LOW;

  if (!manualControlsAllowed()) {
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
  if (currentScreen == SCREEN_TEST) {
    markScreenDirty();
  }
}

// Obsługuje tryb testowy: próbkowanie prądu, ręczne przyciski Z1/Z2 i odświeżanie ekranu.
// Argumenty: brak. Zwraca: brak; oznacza ekran do rysowania, gdy zmieniły się dane.
void updateTestMode() {
  procedureTestUpdate();
  updateHomeManualControls();

  PumpTestInfo info;
  procedureTestGetInfo(&info);
  const uint8_t address = ina219_address();
  if (memcmp(&info, &testInfo, sizeof(info)) != 0 ||
      address != testInfoAddress) {
    testInfo = info;
    testInfoAddress = address;
    markScreenDirty();
  }
}

}  // namespace

const Config defaultConfig = {1, 50, 100, 300, 30, 500, 0, false, 0, 0,
                              DEFAULT_CURRENT_FILTER_SAMPLES,
                              DEFAULT_CUTOFF_CONFIRM_SAMPLES,
                              FILTER_TYPE_AVERAGE,
                              DEFAULT_PRESSURE_CUTOFF_HPA,
                              CUTOFF_METHOD_PRESSURE};
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

  // STOP wciśnięty podczas uruchomienia włącza tryb testowy.
  delay(5);
  testModeActive = digitalRead(kStopButtonPin) == LOW;
  if (testModeActive) {
    currentScreen = SCREEN_TEST;
  }
  markScreenDirty();
}

// Wykonuje pojedynczy przebieg obsługi przycisków, enkodera i wejść ręcznych.
// Argumenty: brak. Zwraca: brak; może wywoływać sterowanie procedurą i zmieniać UI.
void processInput() {
  const uint32_t now = millis();
  if (testModeActive) {
    // STOP w trybie testowym przerywa kalibrację prądu albo wycofuje edycję parametrów.
    if (pollDebouncedButton(stopButton, now)) {
      if (testPage == kTestPageCalibration) {
        procedureTestCalibrationAbort();
      } else if (testPage == kTestPageParams &&
                 paramMode != kParamModeBrowse) {
        paramMode = paramMode == kParamModeEdit ? kParamModeSelect
                                                : kParamModeBrowse;
        markScreenDirty();
      }
    }
    pollEncoder();
    pollEncoderButton();
    updateTestMode();
    return;
  }

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
  } else if (currentScreen == SCREEN_TEST) {
    drawTestScreen();
  } else {
    drawHomeScreen();
  }
  display.display();

  screenDirty = false;
  hasDrawnScreen = true;
  lastScreenUpdate = now;
}