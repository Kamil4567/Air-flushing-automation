#include <Arduino.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ConfigStore.h"
#include "Ui.h"

namespace {

constexpr uint32_t kRecordMagic = 0x43464731;
constexpr uint16_t kRecordVersion = 2;
constexpr uint16_t kLegacyRecordVersion = 1;
constexpr uint32_t kSaveDebounceMs = 750;
constexpr size_t kEepromBufferSize = 256;

struct ConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t cycles;
  uint16_t fillTime;
  uint16_t maxFillTime;
  uint16_t emptyTime;
  uint16_t maxEmptyTime;
  uint32_t actuationTime;
  uint16_t pumpCurrentBaselineTenthsMa;
  uint8_t pumpCurrentBaselineValid;
  uint32_t crc;
};

struct LegacyConfigRecord {
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

// Oblicza CRC-32 dla wskazanego zakresu bajtów.
// Argumenty: bufor danych i jego długość. Zwraca: obliczoną sumę kontrolną.
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

// Sprawdza zakresy wartości rekordu aktualnej wersji konfiguracji.
// Argumenty: rekord do walidacji. Zwraca: true, jeśli wszystkie pola są w zakresie.
bool configValuesValid(const ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= 999 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.emptyTime <= record.maxEmptyTime * 10UL &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie legacy v1.
// Argumenty: rekord legacy. Zwraca: true, jeśli jego pola są w zakresie.
bool legacyConfigValuesValid(const LegacyConfigRecord &record) {
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

// Weryfikuje CRC rekordu obliczone dla bajtów poprzedzających pole crc.
// Argumenty: rekord dowolnego obsługiwanego formatu. Zwraca: zgodność CRC.
template <typename Record>
bool recordCrcValid(const Record &record) {
  const uint32_t expectedCrc = calculateCrc32(
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(Record, crc));
  return record.crc == expectedCrc;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu bieżącej wersji.
// Argumenty: rekord bieżącego formatu. Zwraca: true, gdy rekord jest poprawny.
bool recordValid(const ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kRecordVersion &&
         record.size == sizeof(ConfigRecord) &&
         recordCrcValid(record) &&
         configValuesValid(record);
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie legacy.
// Argumenty: rekord v1. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool legacyRecordValid(const LegacyConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kLegacyRecordVersion &&
         record.size == sizeof(LegacyConfigRecord) &&
         recordCrcValid(record) &&
         legacyConfigValuesValid(record);
}

// Kopiuje pola rekordu bieżącej wersji do przekazanej konfiguracji.
// Argumenty: źródłowy rekord i wskaźnik do celu. Zwraca: brak; nadpisuje pola celu.
void recordToConfig(const ConfigRecord &record, Config *config) {
  config->cycles = record.cycles;
  config->fill_time = record.fillTime;
  config->max_fill_time = record.maxFillTime;
  config->empty_time = record.emptyTime;
  config->max_empty_time = record.maxEmptyTime;
  config->actuation_time = record.actuationTime;
  config->pump_current_baseline_tenths_ma =
      record.pumpCurrentBaselineTenthsMa;
  config->pump_current_baseline_valid =
      record.pumpCurrentBaselineValid != 0;
}

// Migruje ustawienia v1 do config, uzupełniając brakujące pola wartościami domyślnymi.
// Argumenty: zwalidowany rekord legacy. Zwraca: brak; nadpisuje globalne config.
void migrateLegacyRecord(const LegacyConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  const uint32_t migratedEmptyTime = record.maxEmptyTime * 10UL;
  config.empty_time = migratedEmptyTime > 999
                          ? 999
                          : static_cast<uint16_t>(migratedEmptyTime);
  config.max_empty_time = static_cast<uint16_t>(record.maxEmptyTime);
  config.actuation_time = record.actuationTime;
  config.pump_current_baseline_tenths_ma =
      record.pumpCurrentBaselineTenthsMa;
  config.pump_current_baseline_valid =
      record.pumpCurrentBaselineValid != 0;
}

// Buduje aktualny rekord EEPROM z globalnej konfiguracji i oblicza jego CRC.
// Argumenty: brak. Zwraca: gotowy rekord; nie zapisuje go samodzielnie do EEPROM.
ConfigRecord configToRecord() {
  ConfigRecord record = {};
  record.magic = kRecordMagic;
  record.version = kRecordVersion;
  record.size = sizeof(ConfigRecord);
  record.cycles = config.cycles;
  record.fillTime = config.fill_time;
  record.maxFillTime = config.max_fill_time;
  record.emptyTime = config.empty_time;
  record.maxEmptyTime = config.max_empty_time;
  record.actuationTime = config.actuation_time;
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

// Uruchamia emulację EEPROM, odczytuje i waliduje rekord, migruje v1 lub ładuje defaults.
// Argumenty: brak. Zwraca: brak; ustawia config i może zgłosić potrzebę zapisu.
void configStoreBegin() {
  EEPROM.begin(kEepromBufferSize);
  storageReady = true;

  ConfigRecord record = {};
  EEPROM.get(0, record);
  if (recordValid(record)) {
    recordToConfig(record, &config);
  } else {
    LegacyConfigRecord legacyRecord = {};
    EEPROM.get(0, legacyRecord);
    if (legacyRecordValid(legacyRecord)) {
      migrateLegacyRecord(legacyRecord);
    } else {
      config = defaultConfig;
    }
    configStoreRequestSave();
  }
}

// Zgłasza potrzebę zapisu konfiguracji po okresie bez kolejnych zmian.
// Argumenty: brak. Zwraca: brak; ustawia savePending i termin 750 ms, jeśli storage działa.
void configStoreRequestSave() {
  if (!storageReady) {
    return;
  }
  savePending = true;
  saveNotBefore = millis() + kSaveDebounceMs;
}

// Zapisuje oczekującą konfigurację po upływie debounce i zatwierdza ją w EEPROM.
// Argumenty: brak. Zwraca: brak; może wyczyścić savePending po udanym commit.
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