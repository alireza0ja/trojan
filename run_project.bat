@echo off
setlocal

:: Get project root
set "PROJECT_ROOT=%~dp0"
cd /d "%PROJECT_ROOT%"

echo [*] Starting Shattered Mirror Framework...

:: 1. Check Python and dependencies
echo [*] Verifying Python C2 Dependencies...
python -c "import aiohttp" 2>NUL
if %ERRORLEVEL% NEQ 0 (
    echo [!] aiohttp not found. Installing...
    pip install aiohttp
)


:: 2. Launch Builder GUI
echo [*] Launching Shattered Mirror Builder (WPF GUI)...
cd Builder_GUI
dotnet run --project ShatteredBuilder.csproj

pause
