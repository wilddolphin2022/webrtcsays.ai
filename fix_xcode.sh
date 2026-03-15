#!/bin/bash

# Script to automate fixing xcode-select error for xcodebuild

# Exit on any error
set -e

# Function to check if Xcode is installed
check_xcode() {
    if xcodebuild -version >/dev/null 2>&1; then
        echo "Xcode is installed: $(xcodebuild -version)"
        return 0
    else
        echo "Xcode is not installed or not properly configured."
        return 1
    fi
}

# Function to set xcode-select path
set_xcode_path() {
    XCODE_PATH="/Applications/Xcode.app/Contents/Developer"
    CURRENT_PATH=$(xcode-select -p 2>/dev/null || echo "")
    
    if [[ "$CURRENT_PATH" == "$XCODE_PATH" ]]; then
        echo "xcode-select path is already set to $XCODE_PATH"
    else
        echo "Setting xcode-select path to $XCODE_PATH"
        sudo xcode-select -s "$XCODE_PATH"
        echo "xcode-select path set to: $(xcode-select -p)"
    fi
}

# Function to accept Xcode license
accept_license() {
    echo "Checking Xcode license agreement..."
    sudo xcodebuild -license accept >/dev/null 2>&1 || {
        echo "License acceptance failed. Please run 'sudo xcodebuild -license' manually."
        exit 1
    }
    echo "Xcode license accepted."
}

# Function to install Command Line Tools (optional, if needed)
install_clt() {
    echo "Installing Command Line Tools (if not already installed)..."
    xcode-select --install || {
        echo "Command Line Tools already installed or installation not needed."
    }
}

# Main automation logic
echo "Starting Xcode configuration automation..."

# Step 1: Check if Xcode is installed
if ! check_xcode; then
    echo "Xcode is required but not found."
    echo "Please install Xcode from the Mac App Store (https://apps.apple.com/us/app/xcode/id497799835) or Apple Developer Portal (https://developer.apple.com/download/all/)."
    echo "Alternatively, you can install Command Line Tools if Xcode is not needed."
    read -p "Would you like to try installing Command Line Tools? (y/n): " response
    if [[ "$response" == "y" || "$response" == "Y" ]]; then
        install_clt
    else
        echo "Exiting. Please install Xcode and rerun the script."
        exit 1
    fi
fi

# Step 2: Set xcode-select path
set_xcode_path

# Step 3: Accept Xcode license
accept_license

# Step 4: Verify xcodebuild works
if xcodebuild -version; then
    echo "Success: xcodebuild is now configured and working."
else
    echo "Error: xcodebuild still not working. Please check manually."
    exit 1
fi

echo "Automation complete!"
