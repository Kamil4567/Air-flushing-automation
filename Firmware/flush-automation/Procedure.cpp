#include <Arduino.h>
#include <hardware/watchdog.h>
#include <math.h>
#include <string.h>

#include "Ina219.h"
#include "ConfigStore.h"
#include "Procedure.h"
#include "Ui.h"

namespace {

constexpr uint8_t kBuzzerPin = 0;
constexpr uint8_t kZ1Pin = 8;
constexpr uint8_t kZ2Pin = 9;
constexpr uint8_t kPumpPin = 10;
constexpr bool kEnablePumpCutoffDetection = true;
constexpr uint32_t kCurrentSampleIntervalMs = 100;
// Kalibracja: rozruch pompy, pomiar przy otwartym zaworze, potem przy zamkniętym.
constexpr uint32_t kCalibrationSpinupMs = 2000;
constexpr uint32_t kCalibrationOpenMs = 5000;
constexpr uint32_t kCalibrationClosedMs = 6000;
constexpr uint32_t kCalibrationClosedTailMs = 2000;
constexpr uint32_t kCalibrationRunPauseMs = 2000;
constexpr uint8_t kCalibrationTestRuns = 5;
constexpr uint32_t kCalibrationTotalMs =
    kCalibrationSpinupMs + kCalibrationOpenMs + kCalibrationClosedMs;
constexpr float kMinCalibrationRiseMa = 5.0f;
// Wymagany wzrost prądu to wielokrotność odchylenia standardowego prądu przy otwartym zaworze.
constexpr float kMinRiseToNoiseRatio = 3.0f;
// Prąd rozruchowy pompy jest pomijany w detekcji odcięcia.
constexpr uint32_t kMonitorIgnoreMs = 1000;
// Okno wygładzania i porównania: 3 próbki (300 ms) bieżące kontra 3 poprzednie.
constexpr uint8_t kWindowSamples = 3;
constexpr uint8_t kRingSize = 2 * kWindowSamples;
constexpr float kCutoffLevelFraction = 0.5f;
constexpr float kCutoffEarlyFraction = 0.25f;
constexpr float kCutoffSlopeFraction = 0.5f;
constexpr float kCutoffMinSlopeFraction = 0.1f;
constexpr uint8_t kCutoffConfirmSamples = 3;
constexpr uint16_t kShortBeepMs = 100;
constexpr uint16_t kFastBeepMs = 80;
constexpr uint16_t kFastBeepGapMs = 80;
constexpr uint8_t kMaxBeepSteps = 8;

enum MachineState : uint8_t {
  MACHINE_IDLE = 0,
  MACHINE_FILL = 1,
  MACHINE_EMPTY = 2,
  MACHINE_CALIBRATION = 3
};

enum ProcedureStage : uint8_t {
  STAGE_IDLE,
  STAGE_START_PENDING,
  STAGE_CALIBRATION_ACTUATION,
  STAGE_CALIBRATION_SAMPLE,
  STAGE_CALIBRATION_CLOSED,
  STAGE_CALIBRATION_PAUSE,
  STAGE_CALIBRATION_SETTLE,
  STAGE_FILL_ACTUATION,
  STAGE_FILL_DURATION,
  STAGE_FILL_IDLE_SETTLE,
  STAGE_CYCLE_EMPTY_ACTUATION,
  STAGE_CYCLE_EMPTY_MONITOR,
  STAGE_CYCLE_IDLE_SETTLE
};

struct BeepStep {
  uint16_t frequency;
  uint16_t durationMs;
  uint16_t gapMs;
};

constexpr BeepStep kEndSequence[] = {
  {2000, 333, 100},
  {2000, 333, 100},
  {2000, 500, 0},
};

struct Beeper {
  BeepStep steps[kMaxBeepSteps];
  uint8_t stepCount;
  uint8_t stepIndex;
  uint32_t nextChangeMs;
  bool active;
  bool sounding;
  bool rebootAfterSequence;
};

MachineState machineState = MACHINE_IDLE;
ProcedureStage stage = STAGE_IDLE;
Beeper beeper = {};
bool procedureActive = false;
bool baselineValid = false;
// Prąd przy otwartym zaworze (swobodny przepływ).
float calibrationBaselineMa = 0.0f;
// Prąd przy zamkniętym zaworze oraz największy narost w oknie 300 ms.
float calibrationBlockedMa = 0.0f;
float calibrationMaxDeltaMa = 0.0f;
double calibrationCurrentSumMa = 0.0;
uint32_t calibrationSampleCount = 0;
double calibrationTailSumMa = 0.0;
uint32_t calibrationTailCount = 0;
float calibrationOpenNoiseMa = 0.0f;
double calibrationSumSqMa = 0.0;
float lastCalOpenMa = 0.0f;
float lastCalBlockedMa = 0.0f;
float lastCalRequiredMa = 0.0f;
bool lastCalibrationOk = false;
// Kalibracja wieloprzebiegowa: wyniki poprawnych przebiegów są uśredniane.
uint8_t calibrationRunsTarget = 1;
uint8_t calibrationRunIndex = 0;
uint8_t calibrationValidRuns = 0;
double runSumOpenMa = 0.0;
double runSumBlockedMa = 0.0;
double runSumDeltaMa = 0.0;
float currentRing[kRingSize] = {};
uint8_t ringHead = 0;
uint8_t ringCount = 0;
float lastDeltaMa = 0.0f;
uint32_t procedureStartedAt = 0;
uint32_t maximumProcedureMs = 0;
uint32_t stageDeadline = 0;
uint32_t lastCurrentSampleAt = 0;
uint32_t emptyStartedAt = 0;
uint32_t lastDisplayedSecond = UINT32_MAX;
uint8_t currentCycle = 0;
uint8_t consecutiveAboveBaseline = 0;
StatusPhase displayedPhase = STATUS_EMPTY;
bool hasCurrentSample = false;
bool lastSensorOk = false;
float lastCurrentMa = 0.0f;
float calibrationMinMa = 0.0f;
float calibrationMaxMa = 0.0f;
bool testCalibrationActive = false;
TestCalibrationPhase testCalibrationResult = TEST_CAL_IDLE;

constexpr BeepStep kTestFailSequence[] = {
  {800, 35, 45},
  {800, 35, 45},
  {800, 35, 0},
};

// Sprawdza osiągnięcie terminu z uwzględnieniem przepełnienia licznika millis().
// Argumenty: bieżący czas i termin w ms. Zwraca: true, gdy termin minął.
bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// Zaokrągla liczbę milisekund w górę do pełnych sekund.
// Argumenty: czas w ms. Zwraca: zaokrąglony czas w sekundach.
uint32_t roundedSeconds(uint32_t milliseconds) {
  return (milliseconds + 999) / 1000;
}

// Oblicza czas od rozpoczęcia bieżącej procedury.
// Argumenty: bieżący czas w ms. Zwraca: upływ czasu w pełnych sekundach.
uint32_t procedureElapsedSeconds(uint32_t now) {
  return (now - procedureStartedAt) / 1000;
}

// Przelicza maksymalny czas procedury na sekundy do prezentacji.
// Argumenty: brak. Zwraca: zaokrąglony limit czasu w sekundach.
uint32_t statusMaximumSeconds() {
  return roundedSeconds(maximumProcedureMs);
}

// Przekazuje do UI fazę, cykl i czas, gdy zmieniła się sekunda lub wymuszono aktualizację.
// Argumenty: bieżący czas w ms i opcjonalne wymuszenie. Zwraca: brak; może
// aktualizować lastDisplayedSecond oraz dane ekranu statusu.
void publishStatus(uint32_t now, bool force = false) {
  if (!procedureActive) {
    return;
  }

  const uint32_t elapsed = procedureElapsedSeconds(now);
  if (force || elapsed != lastDisplayedSecond) {
    lastDisplayedSecond = elapsed;
    updateStatusScreen(displayedPhase, currentCycle > 0 ? currentCycle : 1,
                       config.cycles, elapsed, statusMaximumSeconds());
  }
}

// Wyłącza wszystkie wyjścia, ustawia wyjścia właściwe dla stanu i publikuje fazę.
// Argumenty: nowy stan maszyny i bieżący czas w ms. Zwraca: brak; zmienia GPIO,
// machineState, displayedPhase i stan ekranu statusu.
void setMachineState(MachineState nextState, uint32_t now) {
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
  machineState = nextState;

  switch (machineState) {
    case MACHINE_FILL:
      digitalWrite(kZ1Pin, HIGH);
      displayedPhase = STATUS_FILL;
      break;
    case MACHINE_EMPTY:
      digitalWrite(kZ2Pin, HIGH);
      digitalWrite(kPumpPin, HIGH);
      displayedPhase = STATUS_EMPTY;
      break;
    case MACHINE_CALIBRATION:
      digitalWrite(kPumpPin, HIGH);
      displayedPhase = STATUS_EMPTY;
      break;
    case MACHINE_IDLE:
    default:
      break;
  }

  publishStatus(now, true);
}

// Kopiuje ograniczoną liczbę kroków dźwięku i rozpoczyna sekwencję bez blokowania.
// Argumenty: tablica kroków, jej liczba oraz bieżący czas w ms.
// Zwraca: brak; zastępuje bieżącą sekwencję i resetuje stan brzęczyka.
void startBeepSequence(const BeepStep *steps, uint8_t count, uint32_t now) {
  noTone(kBuzzerPin);
  beeper.stepCount = count > kMaxBeepSteps ? kMaxBeepSteps : count;
  beeper.stepIndex = 0;
  for (uint8_t i = 0; i < beeper.stepCount; ++i) {
    beeper.steps[i] = steps[i];
  }
  beeper.nextChangeMs = now;
  beeper.active = beeper.stepCount > 0;
  beeper.sounding = false;
  beeper.rebootAfterSequence = false;
}

// Buduje powtarzany sygnał o częstotliwości 1 kHz i przekazuje go do odtwarzania.
// Argumenty: liczba sygnałów, czas dźwięku, przerwa (ms) i bieżący czas (ms).
// Zwraca: brak; zastępuje aktualną sekwencję brzęczyka.
void startBeepPattern(uint8_t count, uint16_t onTimeMs,
                      uint16_t offTimeMs, uint32_t now) {
  BeepStep steps[kMaxBeepSteps];
  const uint8_t stepCount = count > kMaxBeepSteps ? kMaxBeepSteps : count;
  for (uint8_t i = 0; i < stepCount; ++i) {
    steps[i] = {1000, onTimeMs, offTimeMs};
  }
  startBeepSequence(steps, stepCount, now);
}

// Wyłącza brzęczyk i wyjścia, po czym resetuje RP2040 przez watchdog.
// Argumenty: brak. Nie zwraca; w razie powrotu watchdoga pozostaje w pętli.
[[noreturn]] void performSoftwareReset() {
  noTone(kBuzzerPin);
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
  watchdog_reboot(0, 0, 0);
  while (true) {
  }
}

// Wykonuje należną zmianę stanu sekwencji brzęczyka bez oczekiwania blokującego.
// Argumenty: bieżący czas w ms. Zwraca: brak; zmienia brzęczyk i stan sekwencji,
// a po sekwencji resetu może zresetować urządzenie.
void updateBeeper(uint32_t now) {
  if (!beeper.active || !deadlineReached(now, beeper.nextChangeMs)) {
    return;
  }

  if (!beeper.sounding) {
    tone(kBuzzerPin, beeper.steps[beeper.stepIndex].frequency);
    beeper.sounding = true;
    beeper.nextChangeMs =
        now + beeper.steps[beeper.stepIndex].durationMs;
    return;
  }

  noTone(kBuzzerPin);
  beeper.sounding = false;
  const uint16_t gapMs = beeper.steps[beeper.stepIndex].gapMs;
  ++beeper.stepIndex;
  if (beeper.stepIndex >= beeper.stepCount) {
    beeper.active = false;
    if (beeper.rebootAfterSequence) {
      performSoftwareReset();
    }
  } else {
    beeper.nextChangeMs = now + gapMs;
  }
}

// Szacuje maksymalny czas procedury (opróżnianie początkowe + wszystkie cykle).
// Argumenty: brak. Zwraca: czas w ms ograniczony do zakresu uint32_t.
uint32_t calculateMaximumProcedureMs() {
  const uint64_t actuation = config.actuation_time;
  const uint64_t emptyDuration =
      static_cast<uint64_t>(config.empty_time) * 100;
  const uint64_t fillDuration =
      static_cast<uint64_t>(config.fill_time) * 100;
  const uint64_t initialEmpty = 2 * actuation + emptyDuration;
  const uint64_t total = initialEmpty + static_cast<uint64_t>(config.cycles) *
                             (4 * actuation + fillDuration + emptyDuration);

  if (total > UINT32_MAX) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(total);
}

// Rozpoczyna kolejny cykl: zwiększa licznik, sygnalizuje start i włącza napełnianie.
// Argumenty: bieżący czas w ms. Zwraca: brak; zmienia cykl, wyjścia, etap i termin.
void beginCycleFill(uint32_t now) {
  if (currentCycle < config.cycles) {
    ++currentCycle;
  }
  startBeepPattern(1, kShortBeepMs, 0, now);
  setMachineState(MACHINE_FILL, now);
  stage = STAGE_FILL_ACTUATION;
  stageDeadline = now + config.actuation_time;
}

// Zeruje okno ostatnich próbek prądu (definicja poniżej).
void resetCurrentRing();

// Inicjuje pomiar czasu opróżniania i próbkowanie prądu pompy.
// Argumenty: bieżący czas w ms. Zwraca: brak; zeruje licznik próbek i zmienia etap.
void beginPumpMonitor(uint32_t now) {
  emptyStartedAt = now;
  lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  consecutiveAboveBaseline = 0;
  resetCurrentRing();
  stage = STAGE_CYCLE_EMPTY_MONITOR;
}

// Kończy opróżnianie i uruchamia przerwę przed następnym cyklem lub końcem.
// Argumenty: bieżący czas w ms. Zwraca: brak; wyłącza wyjścia i ustawia etap/termin.
void completeCycleEmpty(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  stage = STAGE_CYCLE_IDLE_SETTLE;
  stageDeadline = now + config.actuation_time;
}

// Kończy procedurę, wyłącza wyjścia, sygnalizuje sukces i wraca do ekranu głównego.
// Argumenty: bieżący czas w ms. Zwraca: brak; dezaktywuje procedurę i resetuje etap.
void finishProcedure(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  procedureActive = false;
  stage = STAGE_IDLE;
  startBeepSequence(kEndSequence,
                    static_cast<uint8_t>(sizeof(kEndSequence) / sizeof(kEndSequence[0])),
                    now);
  returnHomeScreen();
}

// Zeruje okno ostatnich próbek prądu używane do wygładzania i liczenia narostu.
// Argumenty: brak. Zwraca: brak; zeruje licznik próbek i ostatni narost.
void resetCurrentRing() {
  ringHead = 0;
  ringCount = 0;
  lastDeltaMa = 0.0f;
}

// Średnia z trzech próbek w oknie liczonym od najnowszej (offset 0) lub poprzedniej (offset 3).
// Argumenty: przesunięcie okna wstecz w próbkach. Zwraca: średni prąd w mA.
float ringWindowAverage(uint8_t offset) {
  float sum = 0.0f;
  for (uint8_t i = 0; i < kWindowSamples; ++i) {
    const uint8_t index =
        (ringHead + kRingSize - 1 - offset - i) % kRingSize;
    sum += currentRing[index];
  }
  return sum / kWindowSamples;
}

// Średni prąd z ostatnich 300 ms. Argumenty: brak. Zwraca: wartość w mA.
float recentAverageMa() {
  return ringWindowAverage(0);
}

// Dodaje próbkę do okna i aktualizuje narost (średnia bieżąca minus poprzednia, 300 ms).
// Argumenty: prąd w mA. Zwraca: brak; zmienia ringHead, ringCount i lastDeltaMa.
void pushCurrent(float currentMa) {
  currentRing[ringHead] = currentMa;
  ringHead = (ringHead + 1) % kRingSize;
  if (ringCount < kRingSize) {
    ++ringCount;
  }
  lastDeltaMa = ringCount == kRingSize
                    ? ringWindowAverage(0) - ringWindowAverage(kWindowSamples)
                    : 0.0f;
}

// Odczytuje prąd z INA219 i zapisuje wynik w stanie diagnostycznym i oknie próbek.
// Argumenty: brak. Zwraca: true, gdy odczyt się udał; po błędzie czyści okno próbek.
bool sampleCurrent() {
  float currentMa;
  lastSensorOk = ina219_read_current_ma(&currentMa);
  hasCurrentSample = true;
  if (!lastSensorOk) {
    resetCurrentRing();
    return false;
  }
  lastCurrentMa = currentMa;
  pushCurrent(currentMa);
  return true;
}

// Zamienia prąd w mA na dziesiąte części mA ograniczone do zakresu zapisu (0-3000 mA).
// Argumenty: prąd w mA. Zwraca: wartość całkowita do zapisu w konfiguracji.
uint16_t toStoredTenthsMa(float currentMa) {
  const float tenths = currentMa * 10.0f + 0.5f;
  if (tenths <= 0.0f) {
    return 0;
  }
  return tenths >= 30000.0f ? 30000 : static_cast<uint16_t>(tenths);
}

// Przywraca dane kalibracji prądu zapisane w konfiguracji.
// Argumenty: brak. Zwraca: brak; kalibracja jest ważna tylko przy zamkniętym zaworze
// dającym wyższy prąd niż otwarty.
void restoreCalibrationFromConfig() {
  calibrationBaselineMa = config.pump_current_baseline_tenths_ma / 10.0f;
  calibrationBlockedMa = config.pump_current_blocked_tenths_ma / 10.0f;
  calibrationMaxDeltaMa = config.pump_current_rise_tenths_ma / 10.0f;
  baselineValid = config.pump_current_baseline_valid &&
                  calibrationBlockedMa > calibrationBaselineMa &&
                  calibrationMaxDeltaMa > 0.0f;
}

// Poziom prądu uznawany za zamknięty zawór (połowa drogi od otwartego do zamkniętego).
// Argumenty: brak. Zwraca: próg w mA.
float cutoffLevelMa() {
  return calibrationBaselineMa +
         kCutoffLevelFraction * (calibrationBlockedMa - calibrationBaselineMa);
}

// Minimalny narost prądu w 300 ms uznawany za zamykanie zaworu.
// Argumenty: brak. Zwraca: limit narostu w mA.
float cutoffSlopeLimitMa() {
  const float rise = calibrationBlockedMa - calibrationBaselineMa;
  const float fromSlope = kCutoffSlopeFraction * calibrationMaxDeltaMa;
  const float fromRise = kCutoffMinSlopeFraction * rise;
  return fromSlope > fromRise ? fromSlope : fromRise;
}

// Sprawdza warunek odcięcia: prąd osiągnął poziom zamkniętego zaworu albo szybko rośnie
// i jest już wyraźnie ponad prądem otwartego zaworu.
// Argumenty: brak. Zwraca: true, gdy ostatnia próbka spełnia warunek.
bool cutoffConditionMet() {
  if (!baselineValid || ringCount < kRingSize) {
    return false;
  }

  const float recent = recentAverageMa();
  const float earlyFloor =
      calibrationBaselineMa +
      kCutoffEarlyFraction * (calibrationBlockedMa - calibrationBaselineMa);
  return recent >= cutoffLevelMa() ||
         (lastDeltaMa >= cutoffSlopeLimitMa() && recent >= earlyFloor);
}

// Zeruje statystyki bieżącej fazy kalibracji (suma, licznik, min, max).
// Argumenty: brak. Zwraca: brak.
void resetCalibrationPhaseStats() {
  calibrationCurrentSumMa = 0.0;
  calibrationSumSqMa = 0.0;
  calibrationSampleCount = 0;
  calibrationMinMa = 0.0f;
  calibrationMaxMa = 0.0f;
}

// Dodaje próbkę do statystyk bieżącej fazy kalibracji.
// Argumenty: prąd w mA. Zwraca: brak.
void addCalibrationPhaseSample(float currentMa) {
  if (calibrationSampleCount == 0 || currentMa < calibrationMinMa) {
    calibrationMinMa = currentMa;
  }
  if (calibrationSampleCount == 0 || currentMa > calibrationMaxMa) {
    calibrationMaxMa = currentMa;
  }
  calibrationCurrentSumMa += currentMa;
  calibrationSumSqMa += static_cast<double>(currentMa) * currentMa;
  ++calibrationSampleCount;
}

// Zapisuje średnie z poprawnych przebiegów kalibracji, o ile jest ich dość.
// Argumenty: brak. Zwraca: true, gdy kalibracja została zaakceptowana i zapisana.
bool commitAveragedCalibration() {
  const uint8_t requiredRuns = calibrationRunsTarget / 2 + 1;
  if (calibrationValidRuns < requiredRuns) {
    return false;
  }

  calibrationBaselineMa =
      static_cast<float>(runSumOpenMa / calibrationValidRuns);
  calibrationBlockedMa =
      static_cast<float>(runSumBlockedMa / calibrationValidRuns);
  calibrationMaxDeltaMa =
      static_cast<float>(runSumDeltaMa / calibrationValidRuns);
  baselineValid = true;
  config.pump_current_baseline_tenths_ma =
      toStoredTenthsMa(calibrationBaselineMa);
  config.pump_current_blocked_tenths_ma =
      toStoredTenthsMa(calibrationBlockedMa);
  config.pump_current_rise_tenths_ma = toStoredTenthsMa(calibrationMaxDeltaMa);
  config.pump_current_baseline_valid = true;
  configStoreRequestSave();
  return true;
}

// Kończy jeden przebieg kalibracji: ocenia wzrost prądu po zamknięciu zaworu i
// dolicza go do średnich. Po ostatnim przebiegu zapisuje wynik albo przywraca poprzedni.
// Argumenty: bieżący czas w ms. Zwraca: brak; zmienia licznik przebiegów, stan maszyny i etap.
void finishCalibration(uint32_t now) {
  if (calibrationTailCount > 0) {
    const float blocked =
        static_cast<float>(calibrationTailSumMa / calibrationTailCount);
    const float rise = blocked - calibrationBaselineMa;
    const float requiredRise =
        kMinRiseToNoiseRatio * calibrationOpenNoiseMa > kMinCalibrationRiseMa
            ? kMinRiseToNoiseRatio * calibrationOpenNoiseMa
            : kMinCalibrationRiseMa;
    lastCalBlockedMa = blocked;
    lastCalRequiredMa = requiredRise;
    if (rise >= requiredRise && calibrationMaxDeltaMa > 0.0f) {
      runSumOpenMa += calibrationBaselineMa;
      runSumBlockedMa += blocked;
      runSumDeltaMa += calibrationMaxDeltaMa;
      ++calibrationValidRuns;
    }
  }
  ++calibrationRunIndex;

  setMachineState(MACHINE_IDLE, now);
  if (calibrationRunIndex < calibrationRunsTarget) {
    stage = STAGE_CALIBRATION_PAUSE;
    stageDeadline = now + kCalibrationRunPauseMs;
    return;
  }

  lastCalibrationOk = commitAveragedCalibration();
  if (!lastCalibrationOk) {
    restoreCalibrationFromConfig();
  }
  stage = STAGE_CALIBRATION_SETTLE;
  stageDeadline = now + config.actuation_time;
}

// Kończy pomiar przy otwartym zaworze i zamyka zawór (Z2 wyłączony, pompa pracuje).
// Argumenty: bieżący czas w ms. Zwraca: brak; zapisuje prąd otwarty i szum, przechodzi
// do etapu zamkniętego zaworu albo kończy kalibrację błędem przy braku próbek.
void finishOpenPhase(uint32_t now) {
  if (calibrationSampleCount == 0) {
    finishCalibration(now);
    return;
  }

  const double mean = calibrationCurrentSumMa / calibrationSampleCount;
  const double variance =
      calibrationSumSqMa / calibrationSampleCount - mean * mean;
  calibrationBaselineMa = static_cast<float>(mean);
  calibrationOpenNoiseMa =
      variance > 0.0 ? static_cast<float>(sqrt(variance)) : 0.0f;
  lastCalOpenMa = calibrationBaselineMa;
  lastCalBlockedMa = 0.0f;
  lastCalRequiredMa = 0.0f;
  calibrationBlockedMa = 0.0f;
  calibrationMaxDeltaMa = 0.0f;
  calibrationTailSumMa = 0.0;
  calibrationTailCount = 0;
  resetCalibrationPhaseStats();

  setMachineState(MACHINE_CALIBRATION, now);
  stage = STAGE_CALIBRATION_CLOSED;
  stageDeadline = now + kCalibrationClosedMs;
  lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
}

// Rozpoczyna pojedynczy przebieg kalibracji: zeruje statystyki, otwiera zawór Z2 i włącza pompę.
// Argumenty: bieżący czas w ms. Zwraca: brak; ustawia stan, etap i termin rozruchu.
void startCalibrationRun(uint32_t now) {
  resetCalibrationPhaseStats();
  resetCurrentRing();
  calibrationTailSumMa = 0.0;
  calibrationTailCount = 0;
  calibrationOpenNoiseMa = 0.0f;
  calibrationMaxDeltaMa = 0.0f;
  baselineValid = false;
  lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  setMachineState(MACHINE_EMPTY, now);
  stage = STAGE_CALIBRATION_ACTUATION;
  stageDeadline = now + kCalibrationSpinupMs;
}

// Rozpoczyna kalibrację składającą się z podanej liczby przebiegów.
// Argumenty: bieżący czas w ms i liczba przebiegów. Zwraca: brak; zeruje średnie.
void startCalibration(uint32_t now, uint8_t runs) {
  calibrationRunsTarget = runs;
  calibrationRunIndex = 0;
  calibrationValidRuns = 0;
  runSumOpenMa = 0.0;
  runSumBlockedMa = 0.0;
  runSumDeltaMa = 0.0;
  lastCalibrationOk = false;
  startCalibrationRun(now);
}

// Próbkuje prąd co 100 ms i sprawdza warunek odcięcia przez 3 kolejne próbki.
// Argumenty: bieżący czas w ms i flaga oceny warunku (false pomija rozruch pompy).
// Zwraca: true po potwierdzonym odcięciu; aktualizuje licznik kolejnych próbek.
bool pollPumpCurrent(uint32_t now, bool evaluate = true) {
  if (now - lastCurrentSampleAt < kCurrentSampleIntervalMs) {
    return false;
  }
  lastCurrentSampleAt = now;

  if (!sampleCurrent()) {
    consecutiveAboveBaseline = 0;
    return false;
  }

  if (!evaluate) {
    consecutiveAboveBaseline = 0;
    return false;
  }

  if (cutoffConditionMet()) {
    if (consecutiveAboveBaseline < kCutoffConfirmSamples) {
      ++consecutiveAboveBaseline;
    }
  } else {
    consecutiveAboveBaseline = 0;
  }

  return consecutiveAboveBaseline >= kCutoffConfirmSamples;
}

// Przeprowadza rozruch, pomiar przy otwartym zaworze i pomiar po jego zamknięciu.
// Argumenty: bieżący czas w ms. Zwraca: brak; zbiera próbki i kończy kalibrację.
void updateCalibration(uint32_t now) {
  if (stage == STAGE_CALIBRATION_PAUSE) {
    if (deadlineReached(now, stageDeadline)) {
      startCalibrationRun(now);
    }
    return;
  }

  if (stage == STAGE_CALIBRATION_ACTUATION) {
    if (!deadlineReached(now, stageDeadline)) {
      return;
    }
    resetCalibrationPhaseStats();
    resetCurrentRing();
    stage = STAGE_CALIBRATION_SAMPLE;
    stageDeadline = now + kCalibrationOpenMs;
    lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  }

  const bool sampleDue = now - lastCurrentSampleAt >= kCurrentSampleIntervalMs;

  if (stage == STAGE_CALIBRATION_SAMPLE) {
    if (sampleDue) {
      lastCurrentSampleAt = now;
      if (sampleCurrent()) {
        addCalibrationPhaseSample(lastCurrentMa);
      }
    }

    if (deadlineReached(now, stageDeadline)) {
      finishOpenPhase(now);
    }
    return;
  }

  if (stage == STAGE_CALIBRATION_CLOSED) {
    if (sampleDue) {
      lastCurrentSampleAt = now;
      if (sampleCurrent()) {
        addCalibrationPhaseSample(lastCurrentMa);
        if (ringCount == kRingSize && lastDeltaMa > calibrationMaxDeltaMa) {
          calibrationMaxDeltaMa = lastDeltaMa;
        }
        if (static_cast<int32_t>(stageDeadline - now) <=
            static_cast<int32_t>(kCalibrationClosedTailMs)) {
          calibrationTailSumMa += lastCurrentMa;
          ++calibrationTailCount;
        }
      }
    }

    if (deadlineReached(now, stageDeadline)) {
      finishCalibration(now);
    }
  }
}

// Kończy opróżnianie po wykryciu odcięcia (zamknięcia zaworu) i przechodzi dalej.
// empty_time jest tylko maksymalnym czasem opróżniania, który kończy krok awaryjnie.
// Argumenty: bieżący czas w ms. Zwraca: brak; przy spełnieniu warunku przełącza etap.
void updatePumpMonitor(uint32_t now) {
  const bool cutoffDetected =
      kEnablePumpCutoffDetection &&
      pollPumpCurrent(now, now - emptyStartedAt >= kMonitorIgnoreMs);
  const bool timedOut =
      now - emptyStartedAt >= config.empty_time * 100UL;
  if (!cutoffDetected && !timedOut) {
    return;
  }

  // Wcześniejsze odcięcie skraca szacowany całkowity czas o niewykorzystany limit.
  const uint32_t limitMs = config.empty_time * 100UL;
  const uint32_t usedMs = now - emptyStartedAt;
  if (usedMs < limitMs) {
    const uint32_t savedMs = limitMs - usedMs;
    maximumProcedureMs =
        maximumProcedureMs > savedMs ? maximumProcedureMs - savedMs : 0;
  }

  completeCycleEmpty(now);
}

// Wykonuje przejście automatu procedury zależne od bieżącego etapu i terminów.
// Argumenty: bieżący czas w ms. Zwraca: brak; aktualizuje etap, wyjścia, cykle i terminy.
void updateProcedureStage(uint32_t now) {
  switch (stage) {
    case STAGE_CALIBRATION_ACTUATION:
    case STAGE_CALIBRATION_SAMPLE:
    case STAGE_CALIBRATION_CLOSED:
    case STAGE_CALIBRATION_PAUSE:
      updateCalibration(now);
      break;

    case STAGE_START_PENDING:
      // Procedura zaczyna się od opróżniania (cykl 0), potem n x (napełnianie + opróżnianie).
      setMachineState(MACHINE_EMPTY, now);
      stage = STAGE_CYCLE_EMPTY_ACTUATION;
      stageDeadline = now + config.actuation_time;
      break;

    case STAGE_CALIBRATION_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        beginCycleFill(now);
      }
      break;

    case STAGE_FILL_ACTUATION:
      if (deadlineReached(now, stageDeadline)) {
        stage = STAGE_FILL_DURATION;
        stageDeadline = now + static_cast<uint32_t>(config.fill_time) * 100;
      }
      break;

    case STAGE_FILL_DURATION:
      if (deadlineReached(now, stageDeadline)) {
        setMachineState(MACHINE_IDLE, now);
        stage = STAGE_FILL_IDLE_SETTLE;
        stageDeadline = now + config.actuation_time;
      }
      break;

    case STAGE_FILL_IDLE_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        setMachineState(MACHINE_EMPTY, now);
        stage = STAGE_CYCLE_EMPTY_ACTUATION;
        stageDeadline = now + config.actuation_time;
      }
      break;

    case STAGE_CYCLE_EMPTY_ACTUATION:
      if (deadlineReached(now, stageDeadline)) {
        beginPumpMonitor(now);
      }
      break;

    case STAGE_CYCLE_EMPTY_MONITOR:
      updatePumpMonitor(now);
      break;

    case STAGE_CYCLE_IDLE_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        if (currentCycle >= config.cycles) {
          finishProcedure(now);
        } else {
          beginCycleFill(now);
        }
      }
      break;

    case STAGE_IDLE:
    default:
      break;
  }
}

}  // namespace

// Inicjalizuje GPIO procedury, odtwarza bazę prądu z config i uruchamia melodię startową.
// Argumenty: brak. Zwraca: brak; konfiguruje sprzęt oraz wewnętrzny stan procedury.
void procedureBegin() {
  pinMode(kBuzzerPin, OUTPUT);
  pinMode(kZ1Pin, OUTPUT);
  pinMode(kZ2Pin, OUTPUT);
  pinMode(kPumpPin, OUTPUT);
  restoreCalibrationFromConfig();
  setMachineState(MACHINE_IDLE, millis());

  const uint32_t now = millis();
  const BeepStep bootMelody[] = {
      {800, 80, 80},
      {800, 80, 80},
      {1400, 100, 80},
      {1000, 450, 0},
  };
  startBeepSequence(bootMelody, 4, now);

}

// Rozpoczyna procedurę, jeśli nie jest już aktywna, i przełącza UI na ekran statusu.
// Argumenty: brak. Zwraca: brak; ustawia czas, limit, licznik cykli i etap startowy.
void procedureStart() {
  if (procedureActive) {
    return;
  }

  const uint32_t now = millis();
  procedureActive = true;
  procedureStartedAt = now;
  maximumProcedureMs = calculateMaximumProcedureMs();
  currentCycle = 0;
  lastDisplayedSecond = UINT32_MAX;
  startBeepPattern(1, kShortBeepMs, 0, now);

  displayedPhase = STATUS_EMPTY;
  showStatusScreen(STATUS_EMPTY, 1, config.cycles, 0,
                   statusMaximumSeconds());
  stage = STAGE_START_PENDING;
}

// Uruchamia krótki sygnał potwierdzający obrót enkodera.
// Argumenty: brak. Zwraca: brak; zastępuje bieżącą sekwencję brzęczyka.
void procedureEncoderTurnBeep() {
  const BeepStep steps[] = {{2000, 30, 0}};
  startBeepSequence(steps, 1, millis());
}

// Uruchamia sygnał potwierdzający naciśnięcie enkodera.
// Argumenty: brak. Zwraca: brak; zastępuje bieżącą sekwencję brzęczyka.
void procedureEncoderPressBeep() {
  const BeepStep steps[] = {{2000, 80, 0}};
  startBeepSequence(steps, 1, millis());
}

// Uruchamia dwutonowy sygnał błędu lub próby wyjścia poza dozwolony zakres.
// Argumenty: brak. Zwraca: brak; zastępuje bieżącą sekwencję brzęczyka.
void procedureEncoderErrorBeep() {
  const BeepStep steps[] = {
      {800, 35, 45},
      {800, 35, 0},
  };
  startBeepSequence(steps, 2, millis());
}

// Zleca reset programowy po zakończeniu krótkiego sygnału potwierdzenia.
// Argumenty: brak. Zwraca: brak; ustawia flagę resetu w sekwencji brzęczyka.
void procedureRequestSoftwareReset() {
  procedureEncoderPressBeep();
  beeper.rebootAfterSequence = true;
}

// Zatrzymuje procedurę, wyłącza wyjścia i wraca do ekranu głównego.
// Argumenty: brak. Zwraca: brak; dezaktywuje automat i przy aktywnym cyklu sygnalizuje STOP.
void procedureStop() {
  const uint32_t now = millis();
  if (!procedureActive) {
    setMachineState(MACHINE_IDLE, now);
    returnHomeScreen();
    return;
  }

  procedureActive = false;
  stage = STAGE_IDLE;
  restoreCalibrationFromConfig();
  setMachineState(MACHINE_IDLE, now);
  startBeepPattern(5, kFastBeepMs, kFastBeepGapMs, now);
  returnHomeScreen();
}

// Ustawia ręczny stan wyjść: napełnianie, opróżnianie albo bezczynność.
// Argumenty: żądanie napełniania i opróżniania. Zwraca: brak; zmienia GPIO i stan maszyny.
void procedureSetManualOutputs(bool filling, bool emptying) {
  if (filling && !emptying) {
    setMachineState(MACHINE_FILL, millis());
  } else if (emptying && !filling) {
    setMachineState(MACHINE_EMPTY, millis());
  } else {
    setMachineState(MACHINE_IDLE, millis());
  }
}

// Aktualizuje brzęczyk i wykonuje bieżący etap automatu, jeśli procedura jest aktywna.
// Argumenty: brak. Zwraca: brak; może zmieniać wyjścia, etapy i dane ekranu statusu.
void procedureUpdate() {
  const uint32_t now = millis();
  updateBeeper(now);

  if (!procedureActive) {
    return;
  }

  updateProcedureStage(now);
  publishStatus(now);
}

// Wykonuje krok trybu testowego: kalibrację albo próbkowanie prądu z detekcją odcięcia.
// Argumenty: brak. Zwraca: brak; po zakończeniu kalibracji sygnalizuje wynik dźwiękiem.
void procedureTestUpdate() {
  const uint32_t now = millis();

  if (procedureActive) {
    return;
  }

  if (!testCalibrationActive) {
    pollPumpCurrent(now);
    return;
  }

  updateCalibration(now);
  if (stage != STAGE_CALIBRATION_SETTLE) {
    return;
  }

  testCalibrationActive = false;
  stage = STAGE_IDLE;
  if (lastCalibrationOk) {
    testCalibrationResult = TEST_CAL_DONE;
    startBeepPattern(2, kShortBeepMs, kFastBeepGapMs, now);
  } else {
    testCalibrationResult = TEST_CAL_FAILED;
    startBeepSequence(kTestFailSequence,
                      static_cast<uint8_t>(sizeof(kTestFailSequence) /
                                           sizeof(kTestFailSequence[0])),
                      now);
  }
  consecutiveAboveBaseline = 0;
}

// Zwraca, czy kalibracja prądu uruchomiona z trybu testowego trwa.
// Argumenty: brak. Zwraca: stan flagi kalibracji testowej.
bool procedureTestCalibrationRunning() {
  return testCalibrationActive;
}

// Uruchamia kalibrację bazy prądu z trybu testowego bez startu cyklu.
// Argumenty: brak. Zwraca: brak; włącza pompę i zeruje wynik poprzedniej kalibracji.
void procedureTestCalibrationStart() {
  if (procedureActive || testCalibrationActive) {
    return;
  }

  const uint32_t now = millis();
  testCalibrationActive = true;
  testCalibrationResult = TEST_CAL_IDLE;
  consecutiveAboveBaseline = 0;
  startCalibration(now, kCalibrationTestRuns);
}

// Przerywa kalibrację testową, wyłącza wyjścia i przywraca zapisaną bazę prądu.
// Argumenty: brak. Zwraca: brak; sygnalizuje przerwanie szybką serią sygnałów.
void procedureTestCalibrationAbort() {
  if (!testCalibrationActive) {
    return;
  }

  const uint32_t now = millis();
  testCalibrationActive = false;
  testCalibrationResult = TEST_CAL_IDLE;
  stage = STAGE_IDLE;
  setMachineState(MACHINE_IDLE, now);
  restoreCalibrationFromConfig();
  consecutiveAboveBaseline = 0;
  startBeepPattern(5, kFastBeepMs, kFastBeepGapMs, now);
}

// Wypełnia zrzut diagnostyczny trybu testowego (prąd w dziesiątych części mA).
// Argumenty: wskaźnik na wynik. Zwraca: brak; zeruje strukturę przed wypełnieniem.
void procedureTestGetInfo(PumpTestInfo *info) {
  const uint32_t now = millis();
  memset(info, 0, sizeof(*info));

  info->hasSample = hasCurrentSample;
  info->sensorOk = lastSensorOk;
  info->currentTenthsMa = static_cast<int32_t>(lroundf(lastCurrentMa * 10.0f));
  info->baselineValid = baselineValid;
  info->baselineTenthsMa =
      static_cast<int32_t>(lroundf(calibrationBaselineMa * 10.0f));
  info->blockedTenthsMa =
      static_cast<int32_t>(lroundf(calibrationBlockedMa * 10.0f));
  if (baselineValid) {
    info->levelTenthsMa = static_cast<int32_t>(lroundf(cutoffLevelMa() * 10.0f));
    info->deltaLimitTenthsMa =
        static_cast<int32_t>(lroundf(cutoffSlopeLimitMa() * 10.0f));
  }
  info->deltaTenthsMa = static_cast<int32_t>(lroundf(lastDeltaMa * 10.0f));
  info->consecutiveSamples = consecutiveAboveBaseline;
  info->confirmSamples = kCutoffConfirmSamples;
  info->cutoffEnabled = kEnablePumpCutoffDetection;
  info->cutoffDetected =
      baselineValid && consecutiveAboveBaseline >= kCutoffConfirmSamples;
  info->pumpOn =
      machineState == MACHINE_EMPTY || machineState == MACHINE_CALIBRATION;

  info->calPhase = testCalibrationResult;
  if (testCalibrationActive) {
    const uint32_t untilDeadline =
        deadlineReached(now, stageDeadline) ? 0 : stageDeadline - now;
    uint32_t remainingMs = untilDeadline;
    if (stage == STAGE_CALIBRATION_PAUSE) {
      info->calPhase = TEST_CAL_PAUSE;
      remainingMs += kCalibrationTotalMs;
    } else if (stage == STAGE_CALIBRATION_ACTUATION) {
      info->calPhase = TEST_CAL_SPINUP;
      remainingMs += kCalibrationOpenMs + kCalibrationClosedMs;
    } else if (stage == STAGE_CALIBRATION_SAMPLE) {
      info->calPhase = TEST_CAL_OPEN;
      remainingMs += kCalibrationClosedMs;
    } else {
      info->calPhase = TEST_CAL_CLOSED;
    }
    // calibrationRunIndex liczy ukończone przebiegi, więc wzór jest ten sam w przerwie i w trakcie przebiegu.
    const uint8_t runsAfterThis =
        calibrationRunsTarget - calibrationRunIndex - 1;
    remainingMs +=
        runsAfterThis * (kCalibrationTotalMs + kCalibrationRunPauseMs);
    info->calRemainingTenths = (remainingMs + 99) / 100;
  }
  info->calRun = calibrationRunIndex < calibrationRunsTarget
                     ? calibrationRunIndex + 1
                     : calibrationRunsTarget;
  info->calRuns = calibrationRunsTarget;
  info->calValidRuns = calibrationValidRuns;

  info->calSampleCount = calibrationSampleCount;
  if (calibrationSampleCount > 0) {
    info->calAverageTenthsMa = static_cast<int32_t>(lroundf(
        static_cast<float>(calibrationCurrentSumMa / calibrationSampleCount) *
        10.0f));
    info->calMinTenthsMa = static_cast<int32_t>(lroundf(calibrationMinMa * 10.0f));
    info->calMaxTenthsMa = static_cast<int32_t>(lroundf(calibrationMaxMa * 10.0f));
  }
  if (calibrationTailCount > 0) {
    info->calBlockedTenthsMa = static_cast<int32_t>(lroundf(
        static_cast<float>(calibrationTailSumMa / calibrationTailCount) *
        10.0f));
  }
  info->calOpenTenthsMa = static_cast<int32_t>(lroundf(lastCalOpenMa * 10.0f));
  info->calRequiredTenthsMa =
      static_cast<int32_t>(lroundf(lastCalRequiredMa * 10.0f));
  info->calDeltaTenthsMa =
      static_cast<int32_t>(lroundf(calibrationMaxDeltaMa * 10.0f));
}