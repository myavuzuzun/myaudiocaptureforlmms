$ErrorActionPreference = "Stop"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "  lmmsaudiocapture - Windows x64 Build" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Error "g++ compiler not found in PATH."
    exit 1
}

New-Item -ItemType Directory -Force -Path bin, dist | Out-Null

Write-Host "[1/4] Building & Running Unit Tests..." -ForegroundColor Yellow
g++ -O2 -std=c++17 -Wall -Wextra -Iinclude src/standalone/main_test.cpp -lole32 -lwinmm -luuid -o bin/lmmsaudiocapture_test.exe
if ($LASTEXITCODE -ne 0) { throw "Failed to build unit tests." }
& .\bin\lmmsaudiocapture_test.exe
if ($LASTEXITCODE -ne 0) { throw "Unit tests failed." }

Write-Host "[2/4] Building VeSTige Plugin (dist/lmmsaudiocapture.dll)..." -ForegroundColor Yellow
g++ -O3 "-msse4.2" -flto -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Iinclude -fno-math-errno -fno-trapping-math -shared -static "-Wl,--gc-sections" -s `
    src/vestige/LMMSAudioCaptureVestige.cpp src/vestige/LMMSAudioCaptureVestige.def `
    -ld2d1 -ldwrite -lole32 -lwinmm -luuid -lcomdlg32 -lshell32 -lgdi32 -luser32 `
    -o dist/lmmsaudiocapture.dll
if ($LASTEXITCODE -ne 0) { throw "Failed to build dist/lmmsaudiocapture.dll." }

Write-Host "[3/4] Verifying VeSTige DLL..." -ForegroundColor Yellow
& powershell -ExecutionPolicy Bypass -File .\test_vestige.ps1
if ($LASTEXITCODE -ne 0) { throw "VeSTige verification failed." }

Write-Host "[4/4] Building Standalone Host (bin/lmmsaudiocapture_standalone.exe)..." -ForegroundColor Yellow
g++ -O3 "-msse4.2" -flto -ffunction-sections -fdata-sections -std=c++17 -Wall -Wextra -Iinclude -fno-math-errno -fno-trapping-math -static -mwindows "-Wl,--gc-sections" -s `
    src/standalone/gui_main.cpp `
    -ld2d1 -ldwrite -lole32 -lwinmm -luuid -lcomdlg32 -lshell32 -lgdi32 -luser32 `
    -o bin/lmmsaudiocapture_standalone.exe
if ($LASTEXITCODE -ne 0) { throw "Failed to build standalone executable." }

$lmmsVst = "$env:USERPROFILE\Documents\lmms\plugins\vst"
if (Test-Path $lmmsVst) {
    Copy-Item "dist/lmmsaudiocapture.dll" -Destination "$lmmsVst\lmmsaudiocapture.dll" -Force -ErrorAction SilentlyContinue
    Write-Host "Copied to $lmmsVst\lmmsaudiocapture.dll" -ForegroundColor Green
}

Write-Host "`nAll build targets completed successfully!" -ForegroundColor Green
