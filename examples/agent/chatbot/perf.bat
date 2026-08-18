@echo off
idf.py clean
set START=%TIME%
idf.py build
set END=%TIME%

set OPTIONS="tokens=1,2,3,4* delims=:."
for /f %OPTIONS% %%a in ("%START%") do set START_H=%%a&set START_M=%%b&set START_S=%%c&set START_CS=%%d
for /f %OPTIONS% %%a in ("%END%") do set END_H=%%a&set END_M=%%b&set END_S=%%c&set END_CS=%%d

set /a START_TOTAL=(1%START_H%-100)*360000 + (1%START_M%-100)*6000 + (1%START_S%-100)*100 + (1%START_CS%-100)
set /a END_TOTAL=(1%END_H%-100)*360000 + (1%END_M%-100)*6000 + (1%END_S%-100)*100 + (1%END_CS%-100)
set /a ELAPSED=END_TOTAL-START_TOTAL
if %ELAPSED% lss 0 set /a ELAPSED+=8640000

set /a ELAPSED_S=ELAPSED/100
set /a ELAPSED_MIN=ELAPSED_S/60
set /a ELAPSED_SEC=ELAPSED_S%%60

echo Temps de build : %ELAPSED_MIN% min %ELAPSED_SEC% s