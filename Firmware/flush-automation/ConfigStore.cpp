#include <Arduino.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ConfigStore.h"
#include "Ui.h"

namespace {

constexpr uint32_t kRecordMagic = 0x43464731;
constexpr uint16_t kRecordVersion = 7;
constexpr uint16_t kVersion6RecordVersion = 6;
constexpr uint16_t kVersion5RecordVersion = 5;
constexpr uint16_t kVersion4RecordVersion = 4;
constexpr uint16_t kVersion3RecordVersion = 3;
constexpr uint16_t kPreviousRecordVersion = 2;
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
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint8_t currentFilterSamples;
  uint8_t cutoffConfirmSamples;
  uint8_t currentFilterType;
  uint16_t pressureCutoffHpa;
  uint8_t cutoffMethod;
  uint32_t crc;
};

// Układ rekordu v6 (bez progu ciśnienia i metody detekcji odcięcia).
struct Version6ConfigRecord {
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
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint8_t currentFilterSamples;
  uint8_t cutoffConfirmSamples;
  uint8_t currentFilterType;
  uint32_t crc;
};

// Układ rekordu v5 (bez typu filtra prądu).
struct Version5ConfigRecord {
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
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint8_t currentFilterSamples;
  uint8_t cutoffConfirmSamples;
  uint32_t crc;
};

// Układ rekordu v4 (bez liczby próbek potwierdzających odcięcie).
struct Version4ConfigRecord {
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
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint8_t currentFilterSamples;
  uint32_t crc;
};

// Układ rekordu v3 (bez liczby próbek filtra prądu).
struct Version3ConfigRecord {
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
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint32_t crc;
};

// Układ rekordu v2 (bez danych kalibracji zamkniętego zaworu).
struct PreviousConfigRecord {
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
         record.pumpCurrentBaselineTenthsMa <= 30000 &&
         record.pumpCurrentBlockedTenthsMa <= 30000 &&
         record.pumpCurrentRiseTenthsMa <= 30000 &&
         record.currentFilterSamples >= 1 &&
         record.currentFilterSamples <= MAX_CURRENT_FILTER_SAMPLES &&
         record.cutoffConfirmSamples >= 1 &&
         record.cutoffConfirmSamples <= MAX_CUTOFF_CONFIRM_SAMPLES &&
         record.currentFilterType < FILTER_TYPE_COUNT &&
         record.pressureCutoffHpa >= PRESSURE_CUTOFF_MIN_HPA &&
         record.pressureCutoffHpa <= PRESSURE_CUTOFF_MAX_HPA &&
         record.cutoffMethod < CUTOFF_METHOD_COUNT;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie v6.
// Argumenty: rekord v6. Zwraca: true, jeśli jego pola są w zakresie.
bool version6ConfigValuesValid(const Version6ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= 999 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.emptyTime <= record.maxEmptyTime * 10UL &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000 &&
         record.pumpCurrentBlockedTenthsMa <= 30000 &&
         record.pumpCurrentRiseTenthsMa <= 30000 &&
         record.currentFilterSamples >= 1 &&
         record.currentFilterSamples <= MAX_CURRENT_FILTER_SAMPLES &&
         record.cutoffConfirmSamples >= 1 &&
         record.cutoffConfirmSamples <= MAX_CUTOFF_CONFIRM_SAMPLES &&
         record.currentFilterType < FILTER_TYPE_COUNT;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie v5.
// Argumenty: rekord v5. Zwraca: true, jeśli jego pola są w zakresie.
bool version5ConfigValuesValid(const Version5ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= 999 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.emptyTime <= record.maxEmptyTime * 10UL &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000 &&
         record.pumpCurrentBlockedTenthsMa <= 30000 &&
         record.pumpCurrentRiseTenthsMa <= 30000 &&
         record.currentFilterSamples >= 1 &&
         record.currentFilterSamples <= MAX_CURRENT_FILTER_SAMPLES &&
         record.cutoffConfirmSamples >= 1 &&
         record.cutoffConfirmSamples <= MAX_CUTOFF_CONFIRM_SAMPLES;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie v4.
// Argumenty: rekord v4. Zwraca: true, jeśli jego pola są w zakresie.
bool version4ConfigValuesValid(const Version4ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= 999 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.emptyTime <= record.maxEmptyTime * 10UL &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000 &&
         record.pumpCurrentBlockedTenthsMa <= 30000 &&
         record.pumpCurrentRiseTenthsMa <= 30000 &&
         record.currentFilterSamples >= 1 &&
         record.currentFilterSamples <= MAX_CURRENT_FILTER_SAMPLES;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie v3.
// Argumenty: rekord v3. Zwraca: true, jeśli jego pola są w zakresie.
bool version3ConfigValuesValid(const Version3ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= 999 &&
         record.maxEmptyTime >= 1 && record.maxEmptyTime <= 999 &&
         record.emptyTime <= record.maxEmptyTime * 10UL &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.pumpCurrentBaselineValid <= 1 &&
         record.pumpCurrentBaselineTenthsMa <= 30000 &&
         record.pumpCurrentBlockedTenthsMa <= 30000 &&
         record.pumpCurrentRiseTenthsMa <= 30000;
}

// Sprawdza zakresy wartości rekordu konfiguracji w formacie v2.
// Argumenty: rekord v2. Zwraca: true, jeśli jego pola są w zakresie.
bool previousConfigValuesValid(const PreviousConfigRecord &record) {
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
  config->pump_current_blocked_tenths_ma = record.pumpCurrentBlockedTenthsMa;
  config->pump_current_rise_tenths_ma = record.pumpCurrentRiseTenthsMa;
  config->current_filter_samples = record.currentFilterSamples;
  config->cutoff_confirm_samples = record.cutoffConfirmSamples;
  config->current_filter_type = record.currentFilterType;
  config->pressure_cutoff_hpa = record.pressureCutoffHpa;
  config->cutoff_method = record.cutoffMethod;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie v6.
// Argumenty: rekord v6. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool version6RecordValid(const Version6ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kVersion6RecordVersion &&
         record.size == sizeof(Version6ConfigRecord) &&
         recordCrcValid(record) &&
         version6ConfigValuesValid(record);
}

// Migruje ustawienia v6: wszystko jest zachowane, a próg ciśnienia i metoda odcięcia
// są domyślne (850 hPa, ciśnienie).
// Argumenty: zwalidowany rekord v6. Zwraca: brak; nadpisuje globalne config.
void migrateVersion6Record(const Version6ConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.pump_current_baseline_tenths_ma = record.pumpCurrentBaselineTenthsMa;
  config.pump_current_baseline_valid = record.pumpCurrentBaselineValid != 0;
  config.pump_current_blocked_tenths_ma = record.pumpCurrentBlockedTenthsMa;
  config.pump_current_rise_tenths_ma = record.pumpCurrentRiseTenthsMa;
  config.current_filter_samples = record.currentFilterSamples;
  config.cutoff_confirm_samples = record.cutoffConfirmSamples;
  config.current_filter_type = record.currentFilterType;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie v5.
// Argumenty: rekord v5. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool version5RecordValid(const Version5ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kVersion5RecordVersion &&
         record.size == sizeof(Version5ConfigRecord) &&
         recordCrcValid(record) &&
         version5ConfigValuesValid(record);
}

// Migruje ustawienia v5: wszystko jest zachowane, a typ filtra jest domyślny (średnia).
// Argumenty: zwalidowany rekord v5. Zwraca: brak; nadpisuje globalne config.
void migrateVersion5Record(const Version5ConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.pump_current_baseline_tenths_ma = record.pumpCurrentBaselineTenthsMa;
  config.pump_current_baseline_valid = record.pumpCurrentBaselineValid != 0;
  config.pump_current_blocked_tenths_ma = record.pumpCurrentBlockedTenthsMa;
  config.pump_current_rise_tenths_ma = record.pumpCurrentRiseTenthsMa;
  config.current_filter_samples = record.currentFilterSamples;
  config.cutoff_confirm_samples = record.cutoffConfirmSamples;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie v4.
// Argumenty: rekord v4. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool version4RecordValid(const Version4ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kVersion4RecordVersion &&
         record.size == sizeof(Version4ConfigRecord) &&
         recordCrcValid(record) &&
         version4ConfigValuesValid(record);
}

// Migruje ustawienia v4: wszystko jest zachowane, a liczba próbek potwierdzających
// odcięcie jest domyślna.
// Argumenty: zwalidowany rekord v4. Zwraca: brak; nadpisuje globalne config.
void migrateVersion4Record(const Version4ConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.pump_current_baseline_tenths_ma = record.pumpCurrentBaselineTenthsMa;
  config.pump_current_baseline_valid = record.pumpCurrentBaselineValid != 0;
  config.pump_current_blocked_tenths_ma = record.pumpCurrentBlockedTenthsMa;
  config.pump_current_rise_tenths_ma = record.pumpCurrentRiseTenthsMa;
  config.current_filter_samples = record.currentFilterSamples;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie v3.
// Argumenty: rekord v3. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool version3RecordValid(const Version3ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kVersion3RecordVersion &&
         record.size == sizeof(Version3ConfigRecord) &&
         recordCrcValid(record) &&
         version3ConfigValuesValid(record);
}

// Migruje ustawienia v3: wszystko jest zachowane, a liczba próbek filtra jest domyślna.
// Argumenty: zwalidowany rekord v3. Zwraca: brak; nadpisuje globalne config.
void migrateVersion3Record(const Version3ConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.pump_current_baseline_tenths_ma = record.pumpCurrentBaselineTenthsMa;
  config.pump_current_baseline_valid = record.pumpCurrentBaselineValid != 0;
  config.pump_current_blocked_tenths_ma = record.pumpCurrentBlockedTenthsMa;
  config.pump_current_rise_tenths_ma = record.pumpCurrentRiseTenthsMa;
}

// Weryfikuje nagłówek, rozmiar, CRC i wartości rekordu w formacie v2.
// Argumenty: rekord v2. Zwraca: true, gdy rekord można bezpiecznie migrować.
bool previousRecordValid(const PreviousConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kPreviousRecordVersion &&
         record.size == sizeof(PreviousConfigRecord) &&
         recordCrcValid(record) &&
         previousConfigValuesValid(record);
}

// Migruje ustawienia v2. Zapisana wcześniej baza prądu pochodziła z innej metody
// kalibracji, więc kalibracja prądu zostaje unieważniona i wymaga ponownego wykonania.
// Argumenty: zwalidowany rekord v2. Zwraca: brak; nadpisuje globalne config.
void migratePreviousRecord(const PreviousConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
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
  // Stara baza prądu pochodzi z innej metody kalibracji; wymaga ponownej kalibracji.
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
  record.pumpCurrentBlockedTenthsMa = config.pump_current_blocked_tenths_ma;
  record.pumpCurrentRiseTenthsMa = config.pump_current_rise_tenths_ma;
  record.currentFilterSamples = config.current_filter_samples;
  record.cutoffConfirmSamples = config.cutoff_confirm_samples;
  record.currentFilterType = config.current_filter_type;
  record.pressureCutoffHpa = config.pressure_cutoff_hpa;
  record.cutoffMethod = config.cutoff_method;
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
    Version6ConfigRecord version6Record = {};
    EEPROM.get(0, version6Record);
    Version5ConfigRecord version5Record = {};
    EEPROM.get(0, version5Record);
    Version4ConfigRecord version4Record = {};
    EEPROM.get(0, version4Record);
    Version3ConfigRecord version3Record = {};
    EEPROM.get(0, version3Record);
    PreviousConfigRecord previousRecord = {};
    EEPROM.get(0, previousRecord);
    LegacyConfigRecord legacyRecord = {};
    EEPROM.get(0, legacyRecord);
    if (version6RecordValid(version6Record)) {
      migrateVersion6Record(version6Record);
    } else if (version5RecordValid(version5Record)) {
      migrateVersion5Record(version5Record);
    } else if (version4RecordValid(version4Record)) {
      migrateVersion4Record(version4Record);
    } else if (version3RecordValid(version3Record)) {
      migrateVersion3Record(version3Record);
    } else if (previousRecordValid(previousRecord)) {
      migratePreviousRecord(previousRecord);
    } else if (legacyRecordValid(legacyRecord)) {
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