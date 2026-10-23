#include "Ina219.h"

#define INA219_FIRST_ADDRESS 0x40
#define INA219_LAST_ADDRESS 0x4F
#define INA219_CONFIG_REGISTER 0x00
#define INA219_CURRENT_REGISTER 0x04
#define INA219_CALIBRATION_REGISTER 0x05
// 32 V, PGA /8 (±320 mV), 12-bit, ciągły pomiar napięcia bocznika i szyny.
#define INA219_CONFIG_VALUE 0x399F
// 0,04096 / (0,0001 A * 0,1 ohm) = 4096; przy boczniku 0,1 ohm daje 0,1 mA/LSB.
#define INA219_CALIBRATION_VALUE 4096
#define INA219_CURRENT_LSB_MA 0.1f

static uint8_t ina219_active_address = 0;

// Zapisuje konfigurację i kalibrację, po czym odczytuje je z powrotem.
// Argumenty: adres urządzenia. Zwraca: true, gdy odczytane wartości zgadzają się z zapisanymi.
static bool ina219_configure(uint8_t address) {
  uint16_t config_readback;
  uint16_t calibration_readback;

  if (!ina219_bus_write_register(address, INA219_CONFIG_REGISTER,
                                 INA219_CONFIG_VALUE) ||
      !ina219_bus_write_register(address, INA219_CALIBRATION_REGISTER,
                                 INA219_CALIBRATION_VALUE)) {
    return false;
  }

  if (!ina219_bus_read_register(address, INA219_CONFIG_REGISTER,
                                &config_readback) ||
      !ina219_bus_read_register(address, INA219_CALIBRATION_REGISTER,
                                &calibration_readback)) {
    return false;
  }

  return config_readback == INA219_CONFIG_VALUE &&
         calibration_readback == INA219_CALIBRATION_VALUE;
}

// Szuka INA219 w zakresie 0x40-0x4F (niepodłączone A0/A1 dają 0x40) i konfiguruje
// pierwsze urządzenie, które poprawnie potwierdzi zapis kalibracji.
// Argumenty: brak. Zwraca: true, gdy czujnik odpowiada i został skonfigurowany.
bool ina219_begin(void) {
  ina219_active_address = 0;

  for (uint8_t address = INA219_FIRST_ADDRESS;
       address <= INA219_LAST_ADDRESS; ++address) {
    if (ina219_bus_probe(address) && ina219_configure(address)) {
      ina219_active_address = address;
      return true;
    }
  }

  return false;
}

// Zwraca adres skonfigurowanego czujnika albo 0, gdy komunikacja nie została nawiązana.
// Argumenty: brak.
uint8_t ina219_address(void) {
  return ina219_active_address;
}

// Odczytuje rejestr prądu i sprawdza, czy kalibracja czujnika nie została utracona
// (np. po resecie lub zaniku zasilania INA219, co dawałoby stale 0 mA).
// Argumenty: wskaźnik na surowy wynik. Zwraca: true, gdy odczyt i kalibracja są poprawne.
static bool ina219_try_read_raw(uint16_t *raw_value) {
  uint16_t calibration;

  return ina219_bus_read_register(ina219_active_address,
                                  INA219_CURRENT_REGISTER, raw_value) &&
         ina219_bus_read_register(ina219_active_address,
                                  INA219_CALIBRATION_REGISTER,
                                  &calibration) &&
         calibration == INA219_CALIBRATION_VALUE;
}

// Odczytuje rejestr prądu INA219, interpretuje go jako signed i przelicza na mA.
// Argumenty: wskaźnik na zmienną wyniku. Zwraca: powodzenie odczytu; przy sukcesie
// zapisuje prąd do *current_ma, a przy błędzie nie modyfikuje wyniku. Przy błędzie
// odczytu lub utracie kalibracji czujnik jest natychmiast inicjalizowany ponownie,
// a odczyt powtarzany raz.
bool ina219_read_current_ma(float *current_ma) {
  uint16_t raw_value;

  if (ina219_active_address == 0 && !ina219_begin()) {
    return false;
  }

  if (!ina219_try_read_raw(&raw_value)) {
    if (!ina219_begin() || !ina219_try_read_raw(&raw_value)) {
      ina219_active_address = 0;
      return false;
    }
  }

  int32_t signed_value = raw_value;
  if ((raw_value & 0x8000u) != 0) {
    signed_value -= 0x10000;
  }

  *current_ma = (float)signed_value * INA219_CURRENT_LSB_MA;
  return true;
}