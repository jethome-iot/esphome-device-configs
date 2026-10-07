#!/bin/bash

# ESPHome Setup Script for Linux/macOS
# This script creates a virtual environment and installs ESPHome

set -e

echo "==================================="
echo "ESPHome Setup Script"
echo "==================================="
echo ""

# Check if Python 3 is installed
if ! command -v python3 &> /dev/null; then
    echo "ERROR: Python 3 is not installed or not in PATH"
    echo "Please install Python 3.12 or newer and try again"
    echo "Download from: https://www.python.org/downloads/"
    exit 1
fi

# Check Python version
PYTHON_VERSION=$(python3 --version 2>&1 | awk '{print $2}')
echo "Found Python $PYTHON_VERSION"

# Extract major and minor version numbers
PYTHON_MAJOR=$(echo "$PYTHON_VERSION" | cut -d. -f1)
PYTHON_MINOR=$(echo "$PYTHON_VERSION" | cut -d. -f2)

# ESPHome 2026.9.0 declares Requires-Python >=3.12,<3.15
if [ "$PYTHON_MAJOR" -lt 3 ] || ([ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -lt 12 ]); then
    echo ""
    echo "ERROR: ESPHome requires Python 3.12 or newer"
    echo "You have Python $PYTHON_VERSION"
    echo "Please upgrade your Python installation and try again"
    echo "Download from: https://www.python.org/downloads/"
    exit 1
fi

if [ "$PYTHON_MAJOR" -gt 3 ] || ([ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -ge 15 ]); then
    echo ""
    echo "ERROR: ESPHome does not support Python $PYTHON_VERSION yet (needs < 3.15)"
    echo "Please use Python 3.12, 3.13 or 3.14"
    exit 1
fi

# Get the project root directory (parent of scripts/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$PROJECT_ROOT/.venv"

echo "Project root: $PROJECT_ROOT"
echo ""

# Create virtual environment if it doesn't exist
if [ -d "$VENV_DIR" ]; then
    echo "Virtual environment already exists at $VENV_DIR"
else
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
    echo "Virtual environment created successfully"
fi

echo ""
echo "Activating virtual environment..."
source "$VENV_DIR/bin/activate"

echo "Upgrading pip..."
pip install --upgrade pip

echo ""
echo "Installing ESPHome from requirements.txt..."
pip install -r "$PROJECT_ROOT/requirements.txt"

echo ""
echo "Installing development tooling from requirements-dev.txt..."
pip install -r "$PROJECT_ROOT/requirements-dev.txt"

echo ""
echo "Installing the pre-commit hook..."
# Worktrees share one hooks directory, so this enables the hooks in all of them.
(cd "$PROJECT_ROOT" && pre-commit install)

echo ""
echo "==================================="
echo "Setup Complete!"
echo "==================================="
echo ""
echo "ESPHome has been installed successfully."
echo ""
echo "To activate the virtual environment, run:"
echo "  source .venv/bin/activate"
echo ""
echo "Activate it before committing: the pre-commit hooks run python3 from"
echo "whatever environment git is started in."
echo ""
echo "Then you can use ESPHome commands like:"
echo "  esphome version"
echo "  esphome compile <config.yaml>"
echo "  esphome upload <config.yaml>"
echo ""
