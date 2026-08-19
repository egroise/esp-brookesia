@echo off
setlocal

set "TARGET=%~dp0managed_components\espressif__esp-sr\model\movemodel.py"

if not exist "%TARGET%" (
    echo File not found: %TARGET%
    exit /b 1
)

powershell -NoProfile -Command "(Get-Content -Raw -Encoding UTF8 '%TARGET%') -replace [char]0x2500,'-' | Set-Content -NoNewline -Encoding UTF8 '%TARGET%'"

echo Done: replaced box-drawing characters in %TARGET%

endlocal
