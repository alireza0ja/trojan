# Hardcoded Absolute Path for the IDE
$idePath = "D:\Programs\Antigravity\Antigravity.exe"

Write-Host "[*] Launching flood into ANTIGRAVITY EXPRESS..."
Write-Host "[*] Path: $idePath"

$files = Get-ChildItem -Recurse -Include *.cpp,*.h,*.cs,*.py,*.asm | Select-Object -ExpandProperty FullName
foreach ($f in $files) {
    # Using Start-Process with hardcoded path and proper quoting
    Start-Process -FilePath $idePath -ArgumentList "--reuse-window", "`"$f`"" -NoNewWindow
    Start-Sleep -Milliseconds 50
}
