#include <Arduino.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ConfigStore.h"
#include "Ui.h"

namespace {

constexpr uint32_t kRecordMagic = 0x43464731;
constexpr uint16_t kRecordVersion = 1;
constexpr uint32_t kSaveDebounceMs = 750;
constexpr size_t kEepromBufferSize = 256;

struct ConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t cycles;
  uint16_t fillTime;
  uint8_t calibrationAtBoot;
  uint8_t calibrationBeforeProcedure;
  uint32_t maxEmptyTime;
  uint32_t actuationTime;
  uint32_t calibrationTime;
  uint16_t pumpCurrentBaselineTenthsMa;
  uint8_t pumpCurrentBaselineValid;
  uint32_t crc;
};

static_assert(sizeof(ConfigRecord) <= kEepromBufferSize,
              "Config record must fit in EEPROM emulation buffer");

bool storageReady = false;
bool savePending = false;
uint32_t saveNotBefore = 0;

uint32_t calculateCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

bool configValuesValid(const ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.calibrationAtBoot <= 1 &&
         record.calibrationBeforeProcedure <= 1 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.calibrationTime >= 1 && record.calibrationTime <= 99 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000;
}

bool recordValid(const ConfigRecord &record) {
  const uint32_t expectedCrc = calculateCrc32(
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(ConfigRecord, crc));
  return record.magic == kRecordMagic &&
         record.version == kRecordVersion &&
         record.size == sizeof(ConfigRecord) &&
         record.crc == expectedCrc &&
         configValuesValid(record);
}

void recordToConfig(const ConfigRecord &record, Config *config) {
  config->cycles = record.cycles;
  config->fill_time = record.fillTime;
  config->calibration_at_boot = record.calibrationAtBoot != 0;
  config->calibration_before_procedure =
      record.calibrationBeforeProcedure != 0;
  config->max_empty_time = record.maxEmptyTime;
  config->actuation_time = record.actuationTime;
  config->calibration_time = record.calibrationTime;
  config->pump_current_baseline_tenths_ma =
      record.pumpCurrentBaselineTenthsMa;
  config->pump_current_baseline_valid =
      record.pumpCurrentBaselineValid != 0;
}

ConfigRecord configToRecord() {
  ConfigRecord record = {};
  record.magic = kRecordMagic;
  record.version = kRecordVersion;
  record.size = sizeof(ConfigRecord);
  record.cycles = config.cycles;
  record.fillTime = config.fill_time;
  record.calibrationAtBoot = config.calibration_at_boot ? 1 : 0;
  record.calibrationBeforeProcedure =
      config.calibration_before_procedure ? 1 : 0;
  record.maxEmptyTime = config.max_empty_time;
  record.actuationTime = config.actuation_time;
  record.calibrationTime = config.calibration_time;
  record.pumpCurrentBaselineTenthsMa =
      config.pump_current_baseline_tenths_ma;
  record.pumpCurrentBaselineValid =
      config.pump_current_baseline_valid ? 1 : 0;
  record.crc = calculateCrc32(
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(ConfigRecord, crc));
  return record;
}

}  // namespace

void configStoreBegin() {
  EEPROM.begin(kEepromBufferSize);
  storageReady = true;

  ConfigRecord record = {};
  EEPROM.get(0, record);
  if (recordValid(record)) {
    recordToConfig(record, &config);
  } else {
    config = defaultConfig;
    configStoreRequestSave();
  }
}

void configStoreRequestSave() {
  if (!storageReady) {
    return;
  }
  savePending = true;
  saveNotBefore = millis() + kSaveDebounceMs;
}

void configStoreService() {
  if (!storageReady || !savePending ||
      static_cast<int32_t>(millis() - saveNotBefore) < 0) {
    return;
  }

  const ConfigRecord record = configToRecord();
  EEPROM.put(0, record);
  if (EEPROM.commit()) {
    savePending = false;
  }
}