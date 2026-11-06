# StyleScanner — Style Scanner & Deco Copier

Geode mod ID: developer.stylescanner  
Version: 1.0.0  
Game target: Geometry Dash 2.2081  
Platforms: Windows x64 and Android (armv7 / arm64)  
SDK compatibility declared by this project: Geode >=3.0.0 <4.00.0

## Features

- Press F3 while a level is open in Play mode or in the Level Editor to open / close StyleScanner.
- Scan the active level's serialized decoration objects, color triggers, level name, creator name, and level visual settings.
- Export as a single-line, portable STY1 Base64 code.
- Copy code to the system clipboard on Windows and on the official Geode Android launcher.
- Paste a code into the import field to show its source name and creator.
- Apply its decoration and color-trigger records additively in the Level Editor.
- Copy background, ground, middle-ground, ground-line, font, color-page, and default palette settings without replacing gameplay start settings.

## Code format and safety limits

The STY1 code contains Base64-encoded metadata, a serialized LevelSettingsObject settings string, and individual GameObject::getSaveString records. The importer validates the header, Base64 fields, record IDs, total size, and object count before passing records to Geometry Dash's editor parser.

Current bounds: 4 MiB code, 512 KiB settings data, 12 KiB per object record, and 2,500 object records.

Import is additive: StyleScanner does not clear the target level or intentionally replace its layout blocks. Created objects are checked after creation; anything that is not a decoration or color trigger is removed.

**Important merge note:** Geometry Dash color-channel IDs are level-global. The current format preserves the source IDs and does not remap them to unused target IDs. When applying to a level that already has color triggers or channel-dependent objects, review possible channel-ID conflicts and use Undo if the merged result is not what you intended. Apply first to a copy of your level when testing unfamiliar codes.

The serialized settings string contains other editor settings, but StyleScanner copies only visual/background/palette fields. It does not copy start mode, speed, dual, platformer mode, or other gameplay settings.

## Build

The GitHub Actions workflow builds three packages on every push and pull request:

- Windows x64 (Win64)
- Android armv7 (Android32)
- Android arm64 (Android64)

It uses the maintained geode-sdk/build-geode-mod action and Geode SDK 3.5.0. Download the corresponding .geode file from the workflow run's artifacts.

Local build requirements: CMake 3.21+, a Geode SDK installation, and the GEODE_SDK environment variable pointing to that installation.

Local build commands:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

## Compatibility note

The requested Geode bound >=3.0.0 <4.00.0 excludes Geode 4.x by definition. This source therefore uses the Geode 3.x templated geode::Popup<> API and builds against SDK 3.5.0. A separate v4 port would need a new manifest bound and API verification.
