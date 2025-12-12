@echo off
REM ESPHome Setup Script for Windows
REM This script creates a virtual environment and installs ESPHome

setlocal enabledelayedexpansion

echo ===================================
echo ESPHome Setup Script
echo ===================================
echo.

REM Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python is not installed or not in PATH
    echo Please install Python 3.11 or higher and try again
    echo Download from: https://www.python.org/downloads/
    pause
    exit /b 1
)

REM Get Python version
for /f "tokens=2 delims= " %%i in ('python --version 2^>^&1') do set PYTHON_VERSION=%%i
echo Found Python !PYTHON_VERSION!

REM Extract major and minor version numbers
for /f "tokens=1,2 delims=." %%a in ("!PYTHON_VERSION!") do (
    set PYTHON_MAJOR=%%a
    set PYTHON_MINOR=%%b
)

REM Check if Python version is 3.11 or higher
if !PYTHON_MAJOR! LSS 3 (
    echo.
    echo ERROR: ESPHome requires Python 3.11 or higher
    echo You have Python !PYTHON_VERSION!
    echo Please upgrade your Python installation and try again
    echo Download from: https://www.python.org/downloads/
    pause
    exit /b 1
)
if !PYTHON_MAJOR! EQU 3 if !PYTHON_MINOR! LSS 11 (
    echo.
    echo ERROR: ESPHome requires Python 3.11 or higher
    echo You have Python !PYTHON_VERSION!
    echo Please upgrade your Python installation and try again
    echo Download from: https://www.python.org/downloads/
    pause
    exit /b 1
)

REM Get the project root directory (parent of scripts\)
set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR:~0,-9%
set VENV_DIR=%PROJECT_ROOT%.venv

echo Project root: %PROJECT_ROOT%
echo.

REM Create virtual environment if it doesn't exist
if exist "%VENV_DIR%" (
    echo Virtual environment already exists at %VENV_DIR%
) else (
    echo Creating virtual environment...
    python -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to create virtual environment
        pause
        exit /b 1
    )
    echo Virtual environment created successfully
)

echo.
echo Activating virtual environment...
call "%VENV_DIR%\Scripts\activate.bat"

echo Upgrading pip...
python -m pip install --upgrade pip

echo.
echo Installing ESPHome from requirements.txt...
pip install -r "%PROJECT_ROOT%requirements.txt"

echo.
echo ===================================
echo Setup Complete!
echo ===================================
echo.
echo ESPHome has been installed successfully.
echo.
echo To activate the virtual environment, run:
echo   .venv\Scripts\activate.bat
echo.
echo Then you can use ESPHome commands like:
echo   esphome version
echo   esphome compile ^<config.yaml^>
echo   esphome upload ^<config.yaml^>
echo.
pause

