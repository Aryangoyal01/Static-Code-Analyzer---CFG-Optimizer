@echo off
setlocal

REM Go to the project directory
cd /d "%~dp0"

echo ==========================================
echo Installing Python dependencies...
echo ==========================================
python -m pip install -r requirements.txt
if errorlevel 1 (
    echo.
    echo Failed to install Python dependencies.
    pause
    exit /b 1
)

echo.
echo ==========================================
echo Configuring CMake...
echo ==========================================
if not exist build mkdir build

cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo ==========================================
echo Building analyzer...
echo ==========================================
cmake --build . --config Release
if errorlevel 1 (
    echo.
    echo Build failed.
    pause
    exit /b 1
)

cd ..

echo.
echo ==========================================
echo Starting Streamlit UI...
echo ==========================================

python -m streamlit run "%~dp0webinterface.py"

pause