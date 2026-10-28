#ifndef PROCEDURE_H
#define PROCEDURE_H

#include <stdint.h>

enum TestCalibrationPhase : uint8_t {
  TEST_CAL_IDLE,
  TEST_CAL_SPINUP,
  TEST_CAL_OPEN,
  TEST_CAL_CLOSED,
  TEST_CAL_DONE,
  TEST_CAL_FAILED,
  TEST_CAL_PAUSE
};

// Zrzut danych diagnostycznych trybu testowego; wartości prądu w dziesiątych mA.
// "Otwarty" = zawór wylotowy otwarty (swobodny przepływ), "zamknięty" = zawór zamknięty.
struct PumpTestInfo {
  int32_t currentTenthsMa;
  int32_t baselineTenthsMa;
  int32_t blockedTenthsMa;
  int32_t levelTenthsMa;
  int32_t deltaTenthsMa;
  int32_t deltaLimitTenthsMa;
  int32_t pressureTenthsHpa;
  int32_t pressureThresholdHpa;
  int32_t calAverageTenthsMa;
  int32_t calMinTenthsMa;
  int32_t calMaxTenthsMa;
  int32_t calBlockedTenthsMa;
  int32_t calDeltaTenthsMa;
  int32_t calOpenTenthsMa;
  int32_t calRequiredTenthsMa;
  uint32_t calSampleCount;
  uint32_t calRemainingTenths;
  uint8_t consecutiveSamples;
  uint8_t confirmSamples;
  uint8_t calPhase;
  uint8_t pressureLowSamples;
  uint8_t cutoffMethod;
  uint8_t calRun;
  uint8_t calRuns;
  uint8_t calValidRuns;
  bool hasSample;
  bool hasPressure;
  bool pressureOk;
  bool pressureCutoffEnabled;
  bool pressureCutoffDetected;
  bool blanked;
  bool sensorOk;
  bool cutoffEnabled;
  bool baselineValid;
  bool cutoffDetected;
  bool pumpOn;
};

// Inicjalizuje GPIO, zapisany próg prądu i sygnał startowy.
// Argumenty/wartość zwrotna: brak; przygotowuje stan sterowania procedurą.
void procedureBegin();
// Rozpoczyna procedurę, jeśli nie jest już aktywna.
// Argumenty/wartość zwrotna: brak; inicjuje automat, timer i ekran statusu.
void procedureStart();
// Zatrzymuje procedurę, wyłącza wyjścia i wraca do ekranu głównego.
// Argumenty/wartość zwrotna: brak; dezaktywuje automat i sygnalizuje zatrzymanie.
void procedureStop();
// Ustawia ręczne wyjścia dla napełniania/opróżniania; sprzeczne żądania wyłączają wyjścia.
// Argumenty: flagi napełniania i opróżniania. Wartość zwrotna: brak; zmienia GPIO i stan maszyny.
void procedureSetManualOutputs(bool filling, bool emptying);
// Aktualizuje dźwięk i wykonuje krok aktywnego automatu procedury.
// Argumenty/wartość zwrotna: brak; może zmieniać etapy, wyjścia i ekran statusu.
void procedureUpdate();
// Odtwarza krótki sygnał obrotu enkodera.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderTurnBeep();
// Odtwarza sygnał naciśnięcia enkodera.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderPressBeep();
// Odtwarza sygnał błędu wejścia poza zakresem.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderErrorBeep();
// Zleca reset programowy po odtworzeniu sygnału potwierdzającego.
// Argumenty/wartość zwrotna: brak; ustawia flagę resetu w sekwencji dźwiękowej.
void procedureRequestSoftwareReset();
// Próbkuje prąd pompy (ta sama logika co w opróżnianiu) albo obsługuje kalibrację testową.
// Argumenty/wartość zwrotna: brak; wywoływana w każdej iteracji pętli trybu testowego.
void procedureTestUpdate();
// Wypełnia zrzut diagnostyczny: prąd, baza, licznik odcięcia i stan kalibracji.
// Argumenty: wskaźnik na wynik. Wartość zwrotna: brak.
void procedureTestGetInfo(PumpTestInfo *info);
// Zwraca, czy kalibracja prądu uruchomiona z trybu testowego trwa.
bool procedureTestCalibrationRunning();
// Wczytuje parametry odcięcia z konfiguracji po ich ręcznej edycji.
// Argumenty/wartość zwrotna: brak; zeruje stan detekcji odcięcia.
void procedureApplyCutoffConfig();
// Uruchamia kalibrację bazy prądu (pompa włączona) bez rozpoczynania cyklu.
// Argumenty/wartość zwrotna: brak; wynik jest zapisywany do konfiguracji.
void procedureTestCalibrationStart();
// Przerywa trwającą kalibrację testową, wyłącza pompę i przywraca zapisaną bazę.
// Argumenty/wartość zwrotna: brak; nic nie robi, gdy kalibracja nie trwa.
void procedureTestCalibrationAbort();

#endif