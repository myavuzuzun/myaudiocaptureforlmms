$ErrorActionPreference = "Stop"

Write-Host "Building & running VeSTige loader verification..." -ForegroundColor Yellow
g++ -O2 -std=c++17 -Wall -Wextra -Iinclude src/vestige/test_vestige_loader.cpp -o bin/test_vestige_loader.exe
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_vestige_loader.exe" }

& .\bin\test_vestige_loader.exe "dist/myaudiocaptureforlmms.dll"
if ($LASTEXITCODE -ne 0) { throw "VeSTige loader verification failed" }
