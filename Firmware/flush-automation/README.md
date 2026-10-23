# Firmware automatyki przedmuchiwania

Firmware jest przeznaczony dla Raspberry Pi Pico (RP2040). Steruje zaworami
oraz pompą, wykonuje cykle napełniania i opróżniania, obsługuje wyświetlacz
OLED, enkoder i przyciski, a ustawienia przechowuje w emulowanej pamięci EEPROM.
W katalogu znajduje się również projekt połączeń i model czujnika INA219 dla
symulatora Wokwi.

## Architektura

Program jest podzielony na kilka modułów:

| Plik | Odpowiedzialność |
| --- | --- |
| [flush-automation.ino](./flush-automation.ino) | Punkt wejścia Arduino. W `setup()` inicjalizuje pamięć konfiguracji, UI i sterowanie procedurą. W `loop()` kolejno obsługuje wejścia, odświeża ekran, aktualizuje automat procedury i obsługuje odroczony zapis. Udostępnia też odczyt rejestru INA219 przez I2C. |
| [Ui.cpp](./Ui.cpp), [Ui.h](./Ui.h) | Obsługa OLED, wejść, ekranów, konfiguracji edytowanej przez użytkownika i kalibracji czasów. Udostępnia strukturę `Config` współdzieloną z pozostałymi modułami. |
| [Procedure.cpp](./Procedure.cpp), [Procedure.h](./Procedure.h) | Nieblokujący automat procedury, sterowanie wyjściami, nadzór prądu pompy, sygnały dźwiękowe i reset programowy. |
| [ConfigStore.cpp](./ConfigStore.cpp), [ConfigStore.h](./ConfigStore.h) | Odczyt, walidacja, migracja i zapis konfiguracji w EEPROM. |
| [Ina219.c](./Ina219.c), [Ina219.h](./Ina219.h) | Odczyt rejestru prądu ze znakiem czujnika INA219 i przeliczenie wyniku na mA. |
| `*Bitmaps.h` | Bitmapy ekranów i cyfr używane przez UI. |
| [diagram.json](./diagram.json), `chips/ina219.chip.c`, `chips/ina219.chip.json` | Schemat połączeń Wokwi oraz model symulowanego czujnika prądu. |

Moduły komunikują się przez interfejsy w plikach nagłówkowych. UI zmienia
wspólną konfigurację i przekazuje zdarzenia do procedury; procedura z kolei
aktualizuje ekran statusu. Główna pętla Arduino regularnie wywołuje te moduły,
więc czasy etapów i sekwencje dźwięków są realizowane bez blokujących opóźnień.

## Uruchomienie i główna pętla

1. `configStoreBegin()` inicjalizuje emulację EEPROM i ładuje konfigurację
   (albo pozostawia wartości domyślne).
2. `uiBegin()` uruchamia I2C, ustawia wejścia z podciąganiem i inicjalizuje
   wyświetlacz SSD1306 128×64 pod adresem `0x3C`.
3. `procedureBegin()` konfiguruje wyjścia, odtwarza zapisaną bazę prądu i
   rozpoczyna melodię startową.
4. W każdej iteracji `loop()`:
   - `processInput()` odczytuje przyciski i enkoder oraz aktualizuje sterowanie
     ręczne;
   - `updateScreen()` rysuje ekran, jeśli wymaga on odświeżenia (co najmniej
     16 ms między kolejnymi rysowaniami);
   - `procedureUpdate()` wykonuje przejścia automatu i obsługuje brzęczyk;
   - ekran jest ponownie odświeżany po zmianach wprowadzonych przez procedurę;
   - `configStoreService()` wykonuje oczekujący zapis, gdy minęło 750 ms od
     ostatniego żądania zapisu.

Przyciski są odczytywane z eliminacją drgań styków (25 ms). Procedura i
brzęczyk korzystają z terminów opartych o `millis()`, dzięki czemu pętla nadal
może obsługiwać interfejs i wyświetlacz.

## Obsługa użytkownika

### Ekran główny

Enkoderem wybiera się liczbę cykli, czas napełniania, czas opróżniania lub
wejście do ustawień. Naciśnięcie enkodera rozpoczyna edycję wybranego parametru;
ponowne naciśnięcie zatwierdza wartość i żąda jej zapisu. Obrót poza dopuszczalny
zakres sygnalizowany jest dźwiękiem błędu.

Przycisk START rozpoczyna procedurę. STOP zatrzymuje ją, wyłącza wyjścia i
wraca do ekranu głównego. Ekran statusu pokazuje bieżącą fazę, numer cyklu,
czas i pasek postępu względem obliczonego czasu maksymalnego.

Przyciski ręczne napełniania i opróżniania działają na ekranie głównym.
Jednoczesne wciśnięcie obu nie włącza żadnego wyjścia. Po przejściu do innego
ekranu sterowanie ręczne jest blokowane do zwolnienia przycisków, aby
pozostawiony wciśnięty przycisk nie uruchomił wyjścia po powrocie.

### Ustawienia i kalibracja czasu

Menu ustawień umożliwia powrót do ekranu głównego, kalibrację czasów,
ustawienie maksymalnego czasu napełniania lub opróżniania oraz wybór
potwierdzenia resetu programowego.

Kalibracja czasów jest ręcznym pomiarem długości włączenia wyjścia:

1. Po wejściu do kalibracji naciśnij enkoder, aby rozpocząć pomiar napełniania.
2. Przytrzymuj przycisk napełniania, aby włączyć wyjście Z1; zmierzony czas
   zostanie zapisany w dziesiątych częściach sekundy.
3. Naciśnij enkoder, aby zatwierdzić napełnianie i przejść do opróżniania.
4. Przytrzymuj przycisk opróżniania, aby włączyć Z2 i pompę; zatwierdź pomiar
   enkoderem, aby zapisać go i zakończyć kalibrację.

Wartość pomiaru jest zaokrąglana do 0,1 s i ograniczona do 99,9 s. Pomiar równy
zero nie jest przyjmowany. Jeśli zmierzony czas przekracza skonfigurowany
limit, limit jest automatycznie podnoszony do wymaganej pełnej sekundy.

## Automat procedury

Automat w `Procedure.cpp` zaczyna od jednorazowego opróżniania (kroki 4-6
poniżej), a następnie wykonuje `cycles` razy następujący przebieg:

1. Włącza Z1 (napełnianie) na czas `actuation_time`.
2. Utrzymuje napełnianie przez `fill_time`.
3. Wyłącza wyjścia i odczekuje `actuation_time`.
4. Włącza Z2 i pompę na czas `actuation_time`.
5. Monitoruje prąd pompy; wykrycie odcięcia kończy krok, a `empty_time` jest
   maksymalnym czasem opróżniania.
6. Wyłącza wyjścia i odczekuje `actuation_time`.
7. Rozpoczyna następny cykl lub kończy procedurę.

Podczas zakończenia normalnego odtwarzana jest seria trzech sygnałów 3 kHz
(każdy 333 ms, przerwy 100 ms). STOP
natychmiast ustawia wyjścia w stan niski, przerywa automat i odtwarza serię
krótkich sygnałów.

Wyjścia są ustawiane centralnie przez zmianę stanu maszyny:

| Stan | Wyjścia aktywne |
| --- | --- |
| Bezczynność | Brak |
| Napełnianie | Z1 |
| Opróżnianie | Z2 i pompa |
| Kalibracja prądu | Pompa |

### Pomiar prądu i ważna uwaga o kalibracji

`Ina219.c` odczytuje rejestr prądu `0x04` czujnika INA219 pod adresem I2C
`0x40`. Surowa wartość jest interpretowana jako liczba ze znakiem, a każdy
krok rejestru odpowiada `0,1 mA`. Odczyt rejestru przez `Wire` zapewnia szkic
Arduino.

Wykrywanie odcięcia jest włączone. Podczas opróżniania (po 1 s rozruchu pompy)
prąd jest próbkowany co 100 ms i wygładzany średnią z 3 próbek (300 ms).
Odcięcie (zamknięcie zaworu) jest zgłaszane, gdy przez 3 kolejne próbki
spełniony jest jeden z warunków: średni prąd osiągnął połowę drogi między
prądem przy otwartym i zamkniętym zaworze, albo narost prądu w 300 ms
(średnia bieżąca minus poprzednia) przekracza połowę największego narostu
zmierzonego w kalibracji (min. 10% różnicy prądów), a prąd jest już ponad
25% tej drogi. Wykrycie odcięcia natychmiast kończy krok opróżniania i
procedura przechodzi dalej. `empty_time` jest tylko maksymalnym czasem
opróżniania: kończy krok awaryjnie, gdy odcięcie nie zostanie wykryte.
Nieudany odczyt zeruje okno próbek i licznik.

Zwykły start procedury nie wykonuje kalibracji; korzysta z danych zapisanych
w EEPROM (prąd przy otwartym zaworze, przy zamkniętym zaworze i największy
narost). Bez poprawnej kalibracji wykrywanie nie działa i cykl kończy się
limitem `empty_time`.

Kalibrację prądu wykonuje się w trybie testowym (włączenie z wciśniętym
przyciskiem STOP, strona 3): 5 przebiegów, każdy to rozruch pompy z otwartym
zaworem Z2, pomiar prądu swobodnego przepływu, a następnie zamknięcie zaworu
Z2 przy pracującej pompie i pomiar prądu oraz narostu. Wynik jest średnią z
poprawnych przebiegów (wymagana ich większość) i jest zapisywany w EEPROM.

## Konfiguracja

Wartości domyślne zdefiniowane w UI:

| Parametr | Wartość domyślna | Jednostka / znaczenie |
| --- | ---: | --- |
| Liczba cykli | 1 | cykle |
| Czas napełniania | 50 | 0,1 s (5 s) |
| Maksymalny czas napełniania | 100 | s |
| Czas opróżniania | 300 | 0,1 s (30 s) |
| Maksymalny czas opróżniania | 30 | s |
| Czas zadziałania | 500 | ms |
| Baza prądu pompy | nieważna | brak wartości domyślnej |

Wewnętrzne czasy napełniania i opróżniania są przechowywane w dziesiątych
częściach sekundy. `max_fill_time` i `max_empty_time` są limitami w sekundach;
odpowiadający im maksymalny czas w edytorze parametrów wynosi odpowiednio
100 s i 999 s. Liczba cykli jest ograniczona do 1–99.

Emulacja EEPROM używa 256-bajtowego bufora. Rekord zawiera magiczną wartość,
wersję, rozmiar, ustawienia i CRC-32.
Niepoprawny rekord bieżącej wersji jest sprawdzany pod kątem formatu wersji 1;
jeśli rekord legacy jest prawidłowy, ustawienia są migrowane do aktualnej
struktury. W przeciwnym razie używane są wartości domyślne. Zmiany są
zapisywane z opóźnieniem, aby ograniczyć częstotliwość zapisów pamięci.

## Połączenia sprzętowe

Mapowanie pinów wynika z firmware’u; schemat Wokwi przedstawia połączenia
symulowane.

| GPIO Pico | Funkcja |
| --- | --- |
| GP0 | Brzęczyk |
| GP1 | START |
| GP2 | STOP |
| GP3 | Enkoder – CLK |
| GP4 | I2C SDA: OLED i INA219 |
| GP5 | I2C SCL: OLED i INA219 |
| GP6 | Enkoder – DT |
| GP7 | Przycisk enkodera |
| GP8 | Wyjście Z1 (napełnianie) |
| GP9 | Wyjście Z2 (opróżnianie) |
| GP10 | Wyjście pompy |
| GP16 | Przycisk ręcznego opróżniania / kalibracji |
| GP17 | Przycisk ręcznego napełniania / kalibracji |

Przyciski w firmware mają włączone wewnętrzne podciąganie do stanu wysokiego
i są aktywne stanem niskim. GPIO GP16 i GP17 są używane przez kod, ale nie są
podłączone w dostarczonym `diagram.json`; należy dodać je do symulacji lub
podłączyć w urządzeniu, jeśli mają być używane.

## Budowanie i symulacja

`libraries.txt` wymienia biblioteki Arduino: Adafruit SSD1306 i Adafruit GFX.
Szkic korzysta także z API Arduino dla RP2040, `Wire`, `EEPROM` oraz
watchdoga sprzętowego Pico. W katalogu nie ma osobnego manifestu budowania ani
konfiguracji testów — wybierz w środowisku Arduino płytkę Raspberry Pi Pico
(RP2040) i doinstaluj biblioteki z `libraries.txt`.

Do symulacji Wokwi otwórz `diagram.json`. Model niestandardowego czujnika
INA219 jest opisany w `chips/ina219.chip.json` i zaimplementowany w
`chips/ina219.chip.c`.
