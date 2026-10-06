#include <Arduino.h>
#include <hardware/watchdog.h>

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
constexpr uint8_t kCutoffConfirmSamples = 3;
constexpr uint16_t kShortBeepMs = 100;
constexpr uint16_t kLongBeepMs = 800;
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
  STAGE_CALIBRATION_SETTLE,
  STAGE_INITIAL_EMPTY_ACTUATION,
  STAGE_INITIAL_EMPTY_MONITOR,
  STAGE_INITIAL_IDLE_SETTLE,
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
bool bootCalibration = false;
bool baselineValid = false;
float calibrationBaselineMa = 0.0f;
double calibrationCurrentSumMa = 0.0;
uint32_t calibrationSampleCount = 0;
uint32_t procedureStartedAt = 0;
uint32_t maximumProcedureMs = 0;
uint32_t stageDeadline = 0;
uint32_t lastCurrentSampleAt = 0;
uint32_t emptyStartedAt = 0;
uint32_t lastDisplayedSecond = UINT32_MAX;
uint8_t currentCycle = 0;
uint8_t consecutiveAboveBaseline = 0;
StatusPhase displayedPhase = STATUS_EMPTY;

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t roundedSeconds(uint32_t milliseconds) {
  return (milliseconds + 999) / 1000;
}

uint32_t procedureElapsedSeconds(uint32_t now) {
  return (now - procedureStartedAt) / 1000;
}

uint32_t statusMaximumSeconds() {
  return roundedSeconds(maximumProcedureMs);
}

void publishStatus(uint32_t now, bool force = false) {
  if (!procedureActive) {
    return;
  }

  const uint32_t elapsed = procedureElapsedSeconds(now);
  if (force || elapsed != lastDisplayedSecond) {
    lastDisplayedSecond = elapsed;
    updateStatusScreen(displayedPhase, currentCycle, config.cycles, elapsed,
                       statusMaximumSeconds());
  }
}

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
      displayedPhase = STATUS_CALIBRATION;
      break;
    case MACHINE_IDLE:
    default:
      break;
  }

  publishStatus(now, true);
}

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

void startBeepPattern(uint8_t count, uint16_t onTimeMs,
                      uint16_t offTimeMs, uint32_t now) {
  BeepStep steps[kMaxBeepSteps];
  const uint8_t stepCount = count > kMaxBeepSteps ? kMaxBeepSteps : count;
  for (uint8_t i = 0; i < stepCount; ++i) {
    steps[i] = {1000, onTimeMs, offTimeMs};
  }
  startBeepSequence(steps, stepCount, now);
}

[[noreturn]] void performSoftwareReset() {
  noTone(kBuzzerPin);
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
  watchdog_reboot(0, 0, 0);
  while (true) {
  }
}

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

uint32_t calculateMaximumProcedureMs() {
  const uint64_t actuation = config.actuation_time;
  const uint64_t emptyTimeout =
      static_cast<uint64_t>(config.max_empty_time) * 1000;
  const uint64_t fillDuration =
      static_cast<uint64_t>(config.fill_time) * 100;
  uint64_t total = actuation + emptyTimeout + actuation;

  if (config.calibration_before_procedure) {
    total += actuation +
             static_cast<uint64_t>(config.calibration_time) * 1000 +
             actuation;
  }

  total += static_cast<uint64_t>(config.cycles) *
           (4 * actuation + fillDuration + emptyTimeout);

  if (total > UINT32_MAX) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(total);
}

void beginInitialEmpty(uint32_t now) {
  setMachineState(MACHINE_EMPTY, now);
  stage = STAGE_INITIAL_EMPTY_ACTUATION;
  stageDeadline = now + config.actuation_time;
}

void beginCycleFill(uint32_t now) {
  if (currentCycle < config.cycles) {
    ++currentCycle;
  }
  startBeepPattern(1, kShortBeepMs, 0, now);
  setMachineState(MACHINE_FILL, now);
  stage = STAGE_FILL_ACTUATION;
  stageDeadline = now + config.actuation_time;
}

void beginPumpMonitor(uint32_t now, bool initialEmpty) {
  emptyStartedAt = now;
  lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  consecutiveAboveBaseline = 0;
  stage = initialEmpty ? STAGE_INITIAL_EMPTY_MONITOR
                       : STAGE_CYCLE_EMPTY_MONITOR;
}

void completeInitialEmpty(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  stage = STAGE_INITIAL_IDLE_SETTLE;
  stageDeadline = now + config.actuation_time;
}

void completeCycleEmpty(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  stage = STAGE_CYCLE_IDLE_SETTLE;
  stageDeadline = now + config.actuation_time;
}

void finishProcedure(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  procedureActive = false;
  bootCalibration = false;
  stage = STAGE_IDLE;
  startBeepPattern(1, kLongBeepMs, 0, now);
  returnHomeScreen();
}

void finishCalibration(uint32_t now) {
  if (calibrationSampleCount > 0) {
    calibrationBaselineMa = static_cast<float>(
        calibrationCurrentSumMa / calibrationSampleCount);
    baselineValid = true;
    const float baselineTenthsMa = calibrationBaselineMa * 10.0f + 0.5f;
    config.pump_current_baseline_tenths_ma =
        baselineTenthsMa >= 30000.0f
            ? 30000
            : static_cast<uint16_t>(baselineTenthsMa);
    config.pump_current_baseline_valid = true;
    configStoreRequestSave();
  } else {
    baselineValid = false;
  }

  setMachineState(MACHINE_IDLE, now);
  if (bootCalibration) {
    procedureActive = false;
    bootCalibration = false;
    stage = STAGE_IDLE;
    startBeepPattern(1, kShortBeepMs, 0, now);
    returnHomeScreen();
  } else {
    stage = STAGE_CALIBRATION_SETTLE;
    stageDeadline = now + config.actuation_time;
  }
}

void startCalibration(bool isBootCalibration, uint32_t now) {
  bootCalibration = isBootCalibration;
  calibrationCurrentSumMa = 0.0;
  calibrationSampleCount = 0;
  baselineValid = false;
  lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  setMachineState(MACHINE_CALIBRATION, now);
  stage = STAGE_CALIBRATION_ACTUATION;
  stageDeadline = now + config.actuation_time;
}

bool pollPumpCurrent(uint32_t now) {
  if (now - lastCurrentSampleAt < kCurrentSampleIntervalMs) {
    return false;
  }
  lastCurrentSampleAt = now;

  float currentMa;
  if (!ina219_read_current_ma(&currentMa)) {
    consecutiveAboveBaseline = 0;
    return false;
  }

  if (!baselineValid) {
    return false;
  }

  if (currentMa > calibrationBaselineMa) {
    if (consecutiveAboveBaseline < kCutoffConfirmSamples) {
      ++consecutiveAboveBaseline;
    }
  } else {
    consecutiveAboveBaseline = 0;
  }

  return consecutiveAboveBaseline >= kCutoffConfirmSamples;
}

void updateCalibration(uint32_t now) {
  if (stage == STAGE_CALIBRATION_ACTUATION) {
    if (!deadlineReached(now, stageDeadline)) {
      return;
    }
    stage = STAGE_CALIBRATION_SAMPLE;
    stageDeadline = now + config.calibration_time * 1000UL;
    lastCurrentSampleAt = now - kCurrentSampleIntervalMs;
  }

  if (stage == STAGE_CALIBRATION_SAMPLE) {
    if (now - lastCurrentSampleAt >= kCurrentSampleIntervalMs) {
      lastCurrentSampleAt = now;
      float currentMa;
      if (ina219_read_current_ma(&currentMa)) {
        calibrationCurrentSumMa += currentMa;
        ++calibrationSampleCount;
      }
    }

    if (deadlineReached(now, stageDeadline)) {
      finishCalibration(now);
    }
  }
}

void updatePumpMonitor(uint32_t now, bool initialEmpty) {
  const bool cutoffDetected =
      kEnablePumpCutoffDetection && pollPumpCurrent(now);
  const bool timedOut =
      now - emptyStartedAt >= config.max_empty_time * 1000UL;
  if (!cutoffDetected && !timedOut) {
    return;
  }

  if (initialEmpty) {
    completeInitialEmpty(now);
  } else {
    completeCycleEmpty(now);
  }
}

void updateProcedureStage(uint32_t now) {
  switch (stage) {
    case STAGE_CALIBRATION_ACTUATION:
    case STAGE_CALIBRATION_SAMPLE:
      updateCalibration(now);
      break;

    case STAGE_START_PENDING:
      if (bootCalibration || config.calibration_before_procedure) {
        startCalibration(bootCalibration, now);
      } else {
        beginInitialEmpty(now);
      }
      break;

    case STAGE_CALIBRATION_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        beginInitialEmpty(now);
      }
      break;

    case STAGE_INITIAL_EMPTY_ACTUATION:
      if (deadlineReached(now, stageDeadline)) {
        beginPumpMonitor(now, true);
      }
      break;

    case STAGE_INITIAL_EMPTY_MONITOR:
      updatePumpMonitor(now, true);
      break;

    case STAGE_INITIAL_IDLE_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        if (config.cycles == 0) {
          finishProcedure(now);
        } else {
          beginCycleFill(now);
        }
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
        beginPumpMonitor(now, false);
      }
      break;

    case STAGE_CYCLE_EMPTY_MONITOR:
      updatePumpMonitor(now, false);
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

void procedureBegin() {
  pinMode(kBuzzerPin, OUTPUT);
  pinMode(kZ1Pin, OUTPUT);
  pinMode(kZ2Pin, OUTPUT);
  pinMode(kPumpPin, OUTPUT);
  calibrationBaselineMa =
      config.pump_current_baseline_tenths_ma / 10.0f;
  baselineValid = config.pump_current_baseline_valid;
  setMachineState(MACHINE_IDLE, millis());

  const uint32_t now = millis();
  const BeepStep bootMelody[] = {
      {800, 80, 80},
      {800, 80, 80},
      {1400, 100, 80},
      {1000, 450, 0},
  };
  startBeepSequence(bootMelody, 4, now);

  if (config.calibration_at_boot) {
    procedureActive = true;
    procedureStartedAt = now;
    maximumProcedureMs = config.actuation_time +
                        config.calibration_time * 1000UL;
    currentCycle = 0;
    lastDisplayedSecond = UINT32_MAX;
    showStatusScreen(STATUS_CALIBRATION, currentCycle, config.cycles, 0,
                     roundedSeconds(maximumProcedureMs));
    bootCalibration = true;
    stage = STAGE_START_PENDING;
  }
}

void procedureStart() {
  if (procedureActive) {
    return;
  }

  const uint32_t now = millis();
  procedureActive = true;
  bootCalibration = false;
  procedureStartedAt = now;
  maximumProcedureMs = calculateMaximumProcedureMs();
  currentCycle = 0;
  lastDisplayedSecond = UINT32_MAX;
  startBeepPattern(1, kShortBeepMs, 0, now);

  const StatusPhase initialPhase = config.calibration_before_procedure
                                      ? STATUS_CALIBRATION
                                      : STATUS_EMPTY;
  displayedPhase = initialPhase;
  showStatusScreen(initialPhase, currentCycle, config.cycles, 0,
                   statusMaximumSeconds());
  stage = STAGE_START_PENDING;
}

void procedureStartCalibrationOnly() {
  if (procedureActive) {
    return;
  }

  const uint32_t now = millis();
  procedureActive = true;
  bootCalibration = true;
  procedureStartedAt = now;
  maximumProcedureMs = config.actuation_time +
                       config.calibration_time * 1000UL;
  currentCycle = 0;
  lastDisplayedSecond = UINT32_MAX;
  showStatusScreen(STATUS_CALIBRATION, currentCycle, config.cycles, 0,
                   roundedSeconds(maximumProcedureMs));
  stage = STAGE_START_PENDING;
}

void procedureEncoderTurnBeep() {
  const BeepStep steps[] = {{2000, 30, 0}};
  startBeepSequence(steps, 1, millis());
}

void procedureEncoderPressBeep() {
  const BeepStep steps[] = {{2000, 80, 0}};
  startBeepSequence(steps, 1, millis());
}

void procedureEncoderErrorBeep() {
  const BeepStep steps[] = {
      {800, 35, 45},
      {800, 35, 0},
  };
  startBeepSequence(steps, 2, millis());
}

void procedureRequestSoftwareReset() {
  procedureEncoderPressBeep();
  beeper.rebootAfterSequence = true;
}

void procedureStop() {
  const uint32_t now = millis();
  if (!procedureActive) {
    setMachineState(MACHINE_IDLE, now);
    returnHomeScreen();
    return;
  }

  procedureActive = false;
  bootCalibration = false;
  stage = STAGE_IDLE;
  setMachineState(MACHINE_IDLE, now);
  startBeepPattern(5, kFastBeepMs, kFastBeepGapMs, now);
  returnHomeScreen();
}

void procedureUpdate() {
  const uint32_t now = millis();
  updateBeeper(now);

  if (!procedureActive) {
    return;
  }

  updateProcedureStage(now);
  publishStatus(now);
}