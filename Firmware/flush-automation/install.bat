@echo off
setlocal
pushd "%~dp0"

set "RP2040_INDEX=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json"
set "OLD_RP2040_INDEX=https://github.com/earlephilhower/arduino-pico/releases/download/package_rp2040_index.json"
set "ARDUINO_CLI="

where arduino-cli >nul 2>&1
if not errorlevel 1 set "ARDUINO_CLI=arduino-cli"
if not defined ARDUINO_CLI if exist "%USERPROFILE%\arduino-cli\arduino-cli.exe" set "ARDUINO_CLI=%USERPROFILE%\arduino-cli\arduino-cli.exe"

if not defined ARDUINO_CLI (
    echo ERROR: Arduino CLI was not found.
    echo Install Arduino CLI and add it to PATH, then run this script again.
    goto :failure
)

echo Checking Arduino CLI...
"%ARDUINO_CLI%" version
if errorlevel 1 goto :failure

echo Checking RP2040 package index...
"%ARDUINO_CLI%" config remove board_manager.additional_urls "%OLD_RP2040_INDEX%" >nul 2>&1
"%ARDUINO_CLI%" config get board_manager.additional_urls | findstr /C:"%RP2040_INDEX%" >nul
if errorlevel 1 (
    "%ARDUINO_CLI%" config add board_manager.additional_urls "%RP2040_INDEX%"
    if errorlevel 1 goto :failure
)

echo Checking RP2040 platform...
"%ARDUINO_CLI%" core list | findstr /B /C:"rp2040:rp2040 " >nul
if errorlevel 1 (
    echo Installing RP2040 platform...
    "%ARDUINO_CLI%" core update-index
    if errorlevel 1 goto :failure
    "%ARDUINO_CLI%" core install rp2040:rp2040
    if errorlevel 1 goto :failure
) else (
    echo RP2040 platform is already installed.
)

call :ensure_library "Adafruit SSD1306"
if errorlevel 1 goto :failure
call :ensure_library "Adafruit GFX Library"
if errorlevel 1 goto :failure
call :ensure_library "Adafruit BusIO"
if errorlevel 1 goto :failure

echo.
echo All required Arduino components are installed.
popd
pause
exit /b 0

:ensure_library
echo Checking library: %~1
"%ARDUINO_CLI%" lib list "%~1" | findstr /R /B /C:"%~1 [0-9]" >nul
if not errorlevel 1 (
    echo %~1 is already installed.
    exit /b 0
)

echo Installing %~1...
"%ARDUINO_CLI%" lib install "%~1"
if errorlevel 1 exit /b 1
exit /b 0

:failure
echo.
echo Installation failed. Review the error above and try again.
popd
pause
exit /b 1
