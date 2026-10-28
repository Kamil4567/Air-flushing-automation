#include "Lps28dfw.h"

#define LPS28DFW_ADDRESS_LOW 0x5C
#define LPS28DFW_ADDRESS_HIGH 0x5D
#define LPS28DFW_WHO_AM_I_REGISTER 0x0F
#define LPS28DFW_WHO_AM_I_VALUE 0xB4
#define LPS28DFW_CTRL_REG1 0x10
#define LPS28DFW_CTRL_REG2 0x11
#define LPS28DFW_CTRL_REG3 0x12
#define LPS28DFW_PRESS_OUT_XL 0x28
#define LPS28DFW_CTRL2_SWRESET 0x04
// Blokada aktualizacji danych: wynik 24-bitowy jest spójny przy odczycie wielobajtowym.
#define LPS28DFW_CTRL2_BDU 0x08
#define LPS28DFW_CTRL3_IF_ADD_INC 0x01
// ODR 25 Hz (bity 6:3 = 0100), uśrednianie 16 próbek (bity 2:0 = 010).
#define LPS28DFW_CTRL1_VALUE ((4u << 3) | 2u)
// Zakres 1260 hPa (FS_MODE = 0): 4096 LSB/hPa.
#define LPS28DFW_LSB_PER_HPA 4096.0f
#define LPS28DFW_MIN_VALID_HPA 200.0f
#define LPS28DFW_MAX_VALID_HPA 1300.0f
#define LPS28DFW_RESET_ATTEMPTS 10

static uint8_t lps28dfw_active_address = 0;

// Resetuje czujnik, a następnie zapisuje konfigurację i odczytuje ją z powrotem.
// Argumenty: adres urządzenia. Zwraca: true, gdy rejestry zawierają zapisane wartości.
static bool lps28dfw_configure(uint8_t address) {
  uint8_t value = 0;
  uint8_t attempt;
  bool reset_done = false;

  if (!lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG2,
                                   LPS28DFW_CTRL2_SWRESET)) {
    return false;
  }

  for (attempt = 0; attempt < LPS28DFW_RESET_ATTEMPTS; ++attempt) {
    lps28dfw_bus_delay_ms(1);
    if (lps28dfw_bus_read_registers(address, LPS28DFW_CTRL_REG2, &value, 1) &&
        (value & LPS28DFW_CTRL2_SWRESET) == 0) {
      reset_done = true;
      break;
    }
  }
  if (!reset_done) {
    return false;
  }

  if (!lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG2,
                                   LPS28DFW_CTRL2_BDU) ||
      !lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG3,
                                   LPS28DFW_CTRL3_IF_ADD_INC) ||
      !lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG1,
                                   LPS28DFW_CTRL1_VALUE)) {
    return false;
  }

  return lps28dfw_bus_read_registers(address, LPS28DFW_CTRL_REG1, &value, 1) &&
         value == LPS28DFW_CTRL1_VALUE;
}

// Sprawdza identyfikator układu pod danym adresem.
// Argumenty: adres urządzenia. Zwraca: true, gdy WHO_AM_I zawiera wartość LPS28DFW (0xB4).
static bool lps28dfw_identify(uint8_t address) {
  uint8_t id = 0;

  return lps28dfw_bus_probe(address) &&
         lps28dfw_bus_read_registers(address, LPS28DFW_WHO_AM_I_REGISTER, &id,
                                     1) &&
         id == LPS28DFW_WHO_AM_I_VALUE;
}

// Wyszukuje czujnik pod adresami 0x5C i 0x5D i konfiguruje pierwszy rozpoznany układ.
// Argumenty: brak. Zwraca: true, gdy czujnik odpowiada i został skonfigurowany.
bool lps28dfw_begin(void) {
  uint8_t address;

  lps28dfw_active_address = 0;
  for (address = LPS28DFW_ADDRESS_LOW; address <= LPS28DFW_ADDRESS_HIGH;
       ++address) {
    if (lps28dfw_identify(address) && lps28dfw_configure(address)) {
      lps28dfw_active_address = address;
      return true;
    }
  }

  return false;
}

// Zwraca adres skonfigurowanego czujnika albo 0, gdy komunikacja nie została nawiązana.
// Argumenty: brak.
uint8_t lps28dfw_address(void) {
  return lps28dfw_active_address;
}

// Odczytuje 24-bitowe ciśnienie (U2) i przelicza je na hPa.
// Argumenty: wskaźnik na wynik w hPa. Zwraca: true dla poprawnego odczytu; wynik poza
// zakresem pracy (np. 0 tuż po starcie) jest odrzucany jako nieprzygotowana próbka.
// Błąd komunikacji powoduje ponowne wyszukanie i konfigurację czujnika.
bool lps28dfw_read_pressure_hpa(float *hpa) {
  uint8_t data[3];
  int32_t raw;
  float value;

  if (lps28dfw_active_address == 0 && !lps28dfw_begin()) {
    return false;
  }

  if (!lps28dfw_bus_read_registers(lps28dfw_active_address,
                                   LPS28DFW_PRESS_OUT_XL, data, 3)) {
    lps28dfw_active_address = 0;
    return false;
  }

  raw = (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                  ((uint32_t)data[2] << 16));
  if ((raw & 0x800000) != 0) {
    raw -= 0x1000000;
  }

  value = (float)raw / LPS28DFW_LSB_PER_HPA;
  if (value < LPS28DFW_MIN_VALID_HPA || value > LPS28DFW_MAX_VALID_HPA) {
    return false;
  }

  *hpa = value;
  return true;
}
