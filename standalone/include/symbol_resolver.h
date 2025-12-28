#pragma once

#include <windows.h>
#include <string>

// Initialize symbol resolver (loads dbghelp)
bool InitSymbolResolver();

// Cleanup symbol resolver
void CleanupSymbolResolver();

// Find a function by decorated name in a module
void* FindFunctionBySymbol(HMODULE hModule, const char* decoratedName);

// Find a function by decorated name in a module (wide string version)
void* FindFunctionBySymbol(HMODULE hModule, const wchar_t* decoratedName);
