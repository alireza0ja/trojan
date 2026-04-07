@echo off
set /p PORT="Enter the Port to listen on (e.g., 8080): "
echo [*] Starting Shattered Mirror C2 Listener on port %PORT%...
cd /d "%~dp0C2_Backend"
python listener.py --port %PORT%
pause
