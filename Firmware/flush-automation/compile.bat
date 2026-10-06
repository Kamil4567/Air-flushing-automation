@echo off
echo ========================================
echo   Building Raspberry Pi Pico firmware
echo ========================================
echo.

@REM  Install rp2040 platform in arduino-cli
@REM  before running this script
@REM  


arduino-cli compile --fqbn rp2040:rp2040:rpipico --output-dir build .

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ========================================
    echo   BUILD FAILED
    echo ========================================
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo ========================================
echo   BUILD SUCCESSFUL
echo ========================================
echo.
echo Firmware:
echo   build\flush-automation.ino.uf2
echo.

pause