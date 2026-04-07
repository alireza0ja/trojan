@echo off
setlocal enabledelayedexpansion

echo [*] --- Shattered Mirror: FULL MAX CLEANER ---
echo [*] Initializing cleanup sequence...

:: 1. Force Kill Processes
echo [*] Terminating Trojan processes...
taskkill /F /IM VaultSvc.exe /T 2>nul
taskkill /F /IM Shattered_Mirror.exe /T 2>nul
taskkill /F /IM Dropper.exe /T 2>nul

:: 2. Remove Scheduled Task Persistence
echo [*] Removing Scheduled Task: "OneDrive Standalone Sync Service"...
schtasks /delete /tn "OneDrive Standalone Sync Service" /f 2>nul

:: 3. Delete Hijack Files
echo [*] Deleting Sideloading artifacts...
set "TARGET_DIR=%LOCALAPPDATA%\Microsoft\Vault"
if exist "%TARGET_DIR%" (
    del /f /q "%TARGET_DIR%\version.dll" 2>nul
    del /f /q "%TARGET_DIR%\VaultSvc.exe" 2>nul
    rmdir /s /q "%TARGET_DIR%" 2>nul
    echo [+] Successfully deleted hijack directory: %TARGET_DIR%
)

:: 4. Clean Logs and Temp Artifacts
echo [*] Cleaning logs, build objects, and temporary bin files...
del /F /Q shattered_debug.log 2>nul
del /F /Q client_logs.py 2>nul
del /F /Q payload.bin 2>nul
del /F /Q *.obj 2>nul
del /F /Q *.lib 2>nul
del /F /Q *.exp 2>nul
del /F /Q Atoms\*.obj 2>nul
del /F /Q Evasion_Suite\*.obj 2>nul
del /F /Q Evasion_Suite\src\*.obj 2>nul
del /F /Q Builder_GUI\*.res 2>nul
del /F /Q _shattered_build_tmp.bat 2>nul

:: 5. Flush Network States (Optional but good for cleanliness)
echo [*] Flushing DNS cache...
ipconfig /flushdns >nul

echo [!] --- CLEANUP COMPLETE ---
echo [!] System state has been restored.
pause
