@echo off
cd /d "%~dp0"

echo Installing Python dependencies...
python -m pip install -r requirements.txt

echo Building the analyzer...
if not exist build mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
cd ..

echo.
echo Starting the UI...
python -m streamlit run webinterface.py
pause