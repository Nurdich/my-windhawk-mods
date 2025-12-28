#pragma once

#include <windows.h>
#include <string>

enum class CalculateFolderSizes {
    disabled,
    everything,
    withShiftKey,
    always,
};

struct Settings {
    CalculateFolderSizes calculateFolderSizes = CalculateFolderSizes::disabled;
    bool sortSizesMixFolders = true;
    bool disableKbOnlySizes = true;
    bool useIecTerms = false;
};

// Load settings from INI file
Settings LoadSettings();

// Get the path to the config file (next to the DLL)
std::wstring GetConfigPath();

// Save default settings
void SaveDefaultSettings();

// Logging function
void LogMessage(const wchar_t* format, ...);

// Enable/disable console logging
void SetConsoleLogging(bool enable);
