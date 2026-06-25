@echo off
if exist build rd /s /q build
mkdir build
cd build
cmake ..
cmake --build . --config Release
echo [SUCCESS] Analyzer built at build\Release\analyzer.exe
pause