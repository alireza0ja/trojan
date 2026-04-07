@echo off
set PROJECT_ROOT=%~dp0
cd /d "%PROJECT_ROOT%"

:: --- SHATTERED MIRROR: MASTER CONTROLLER ---

echo [*] Initializing Shattered Mirror Framework...

:: 1. Dependency Check
echo [*] Checking Python C2 Dependencies...
python -c "import aiohttp, flask" 2>NUL
if errorlevel 1 (
    echo [!] Missing dependencies. Installing now...
    python -m pip install aiohttp flask --quiet
)

:choice
cls
echo =============================================================
echo        SHATTERED MIRROR v1 — MASTER COMMAND HUB
echo =============================================================
echo.
echo  [1] Launch Master C2 Console (Operator Panel)
echo  [2] Launch Payload Builder (WPF GUI)
echo  [3] Run System Cleaner (Wipe Traces)
echo  [4] Exit
echo.

set /p choice="Shattered Mirror > "

if "%choice%"=="1" (
    echo [*] Starting Master C2 Console...
    cd /d "%PROJECT_ROOT%C2_Backend"
    python Shattered_Console.py
    cd /d "%PROJECT_ROOT%"
    pause
    goto choice
)

if "%choice%"=="2" (
    echo [*] Opening Payload Builder GUI...
    cd /d "%PROJECT_ROOT%Builder_GUI"
    dotnet run --project ShatteredBuilder.csproj
    cd /d "%PROJECT_ROOT%"
    pause
    goto choice
)

if "%choice%"=="3" (
    call full_cleaner.bat
    pause
    goto choice
)

if "%choice%"=="4" exit /b 0

goto choice
