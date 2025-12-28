# Better File Sizes - Standalone Version

This is a standalone version of the "Better File Sizes in Explorer Details" Windhawk module.
It provides the same functionality without requiring Windhawk to be installed.

## Features

- **Show folder sizes** in Explorer's details view
  - Via Everything integration (recommended, fast)
  - Or calculated manually
- **Mix files and folders when sorting by size**
- **Use MB/GB for large files** instead of KB only
- **Use IEC terms** (KiB, MiB, GiB instead of KB, MB, GB)

## Requirements

- Windows 10/11 (x64)
- Visual Studio 2019 or later (for building)
- CMake 3.20 or later
- [Everything](https://www.voidtools.com/) (optional, for fast folder size calculation)

## Building

### Using Visual Studio Developer Command Prompt

1. Open "x64 Native Tools Command Prompt for VS 2022" (or similar)
2. Navigate to the `standalone` directory
3. Run:
   ```batch
   build.bat
   ```

### Using CMake directly

```batch
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

## Installation

1. Copy `BetterFileSizesHook.dll` and `BetterFileSizesInjector.exe` to a folder
2. Run as Administrator:
   ```batch
   BetterFileSizesInjector.exe inject
   ```

## Usage

### Injector Commands

```batch
BetterFileSizesInjector.exe inject   # Inject into explorer.exe
BetterFileSizesInjector.exe unload   # Unload from explorer.exe
BetterFileSizesInjector.exe status   # Check if loaded
BetterFileSizesInjector.exe help     # Show help
```

### Configuration

Edit `BetterFileSizes.ini` (created automatically next to the DLL):

```ini
[Settings]
; Show folder sizes: disabled, everything, withShiftKey, always
calculateFolderSizes=disabled

; Mix files and folders when sorting by size
sortSizesMixFolders=1

; Use MB/GB for large files instead of KB only
disableKbOnlySizes=1

; Use IEC terms (KiB, MiB, GiB)
useIecTerms=0
```

### Using Everything for Folder Sizes

For fast folder size calculation:

1. Install [Everything](https://www.voidtools.com/)
2. Enable folder size indexing in Everything:
   - Tools → Options → Indexes
   - Check "Index file size"
   - Check "Index folder size"
3. Set `calculateFolderSizes=everything` in the config

## Auto-start on Login

To automatically inject on login:

1. Create a shortcut to `BetterFileSizesInjector.exe inject`
2. Place it in `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`

Or use Task Scheduler to run at logon with Administrator privileges.

## Uninstalling

1. Run: `BetterFileSizesInjector.exe unload`
2. Delete the files

## Credits

Original Windhawk module by [m417z](https://github.com/m417z).

## License

GNU General Public License v3.0
