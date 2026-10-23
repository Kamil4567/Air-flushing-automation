#ifndef UI_H
#define UI_H

#include <stdint.h>

#define SCREEN_HOME 0
#define SCREEN_SETTINGS 1
#define SCREEN_STATUS 2
#define SCREEN_SETTINGS_NUMBER_EDIT 3
#define SCREEN_SETTINGS_CONFIRM 4
#define SCREEN_TIME_CALIBRATION 5
#define SCREEN_TEST 6

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
  // Prąd pompy przy otwartym zaworze (swobodny przepływ), w dziesiątych mA.
  uint16_t pump_current_baseline_tenths_ma;
  bool pump_current_baseline_valid;
  // Prąd pompy przy zamkniętym zaworze, w dziesiątych mA.
  uint16_t pump_current_blocked_tenths_ma;
  // Największy narost prądu w oknie 300 ms przy zamykaniu zaworu, w dziesiątych mA.
  uint16_t pump_current_rise_tenths_ma;
};

extern Config config;
extern const Config defaultConfig;
extern int currentScreen;

// Inicjalizuje I2C, wejścia użytkownika i wyświetlacz OLED.
// Argumenty/wartość zwrotna: brak; konfiguruje sprzęt oraz stan UI.
void uiBegin();
// Wykonuje jeden przebieg obsługi przycisków, enkodera i przycisków ręcznych.
// Argumenty/wartość zwrotna: brak; może zmieniać ekran, konfigurację i wyjścia.
void processInput();
// Rysuje zmieniony ekran, jeśli OLED jest gotowy i upłynął minimalny odstęp.
// Argumenty/wartość zwrotna: brak; aktualizuje wyświetlacz i flagi odświeżenia.
void updateScreen();
// Przełącza UI na ekran statusu i ustawia początkowe dane procedury.
// Argumenty: faza, numer bieżącego cyklu, liczba cykli, czas upływu i maksimum w sekundach.
// Wartość zwrotna: brak; zmienia ekran oraz dane statusu.
void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds);
// Aktualizuje dane statusu, oznaczając ekran do odrysowania po wykryciu zmiany.
// Argumenty: faza, numer bieżącego cyklu, liczba cykli, czas upływu i maksimum w sekundach.
// Wartość zwrotna: brak; modyfikuje stan statusu i w razie potrzeby flagę rysowania.
void updateStatusScreen(StatusPhase phase, uint8_t currentCycle,
                        uint8_t allCycles, uint32_t elapsedSeconds,
                        uint32_t maximumSeconds);
// Wraca do ekranu głównego i czyści zaznaczenie oraz stan kalibracji, jeśli był aktywny.
// Argumenty/wartość zwrotna: brak; zmienia stan ekranów i UI.
void returnHomeScreen();
// Rozpoczyna pomiar kalibracyjny czasów napełniania/opróżniania.
// Argumenty/wartość zwrotna: brak; resetuje pomiary, wyłącza wyjścia i zmienia ekran.
void timeCalibrationStart();
// Kończy kalibrację, wyłącza ręczne wyjścia i wraca do ekranu głównego.
// Argumenty/wartość zwrotna: brak; resetuje stan pomiaru i ustawia blokadę wejść.
void timeCalibrationStop();
// Zatwierdza pomiar albo przechodzi do kolejnego kroku kalibracji.
// Argumenty: brak. Zwraca: true dla przyjętego kroku, false dla błędnego/nieaktywnego;
// aktualizuje config i może zlecić jego zapis.
bool timeCalibrationNextStep();
// Odczytuje wejścia kalibracji, steruje wyjściami i odświeża licznik pomiaru.
// Argumenty/wartość zwrotna: brak; aktualizuje stan kalibracji i UI.
void timeCalibrationUpdate();

#endif