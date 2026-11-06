# StyleScanner — Style Scanner & Deco Copier

Geode mod ID: `developer.stylescanner`  
Version: `1.0.0`  
Game target: Geometry Dash `2.2081`  
Platforms: Windows x64 and Android (armv7 / arm64)  
Build baseline: Geode SDK `5.11.2`, with the 2.2081 bindings pinned to commit `cd269606418c1f749d7a9e71d602478f944c8af8`.

## Features

- Press **F3** while a level is open in Play mode or in the Level Editor to open / close StyleScanner.
- Scan the active level's serialized decoration objects, color triggers, level name, creator name, and level visual settings.
- Export as a single-line, portable `STY1` Base64 code.
- Copy code to the system clipboard on Windows and on the official Geode Android launcher.
- Paste a code into the import field to show its source name and creator.
- Apply its decoration and color-trigger records additively in the Level Editor.
- Copy background, ground, middle-ground, ground-line, font, color-page, and default palette settings without replacing gameplay start settings.

## Code format and safety limits

The `STY1` code contains Base64-encoded metadata, a serialized `LevelSettingsObject` settings string, and individual `GameObject::getSaveString` records. The importer validates the header, Base64 fields, record IDs, total size, and object count before passing records to Geometry Dash's editor parser.

Current bounds: 4 MiB code, 512 KiB settings data, 12 KiB per object record, and 2,500 object records.

Import is additive: StyleScanner does not clear the target level or intentionally replace its layout blocks. Created objects are checked after creation; anything that is not a decoration or color trigger is removed.

**Important merge note:** Geometry Dash color-channel IDs are level-global. The current format preserves the source IDs and does not remap them to unused target IDs. When applying to a level that already has color triggers or channel-dependent objects, review possible channel-ID conflicts and use Undo if the merged result is not what you intended. Apply first to a copy of your level when testing unfamiliar codes.

The serialized settings string contains other editor settings, but StyleScanner copies only visual/background/palette fields. It does not copy start mode, speed, dual, platformer mode, or other gameplay settings.

## Build and Geode compatibility

The source is built against Geode SDK 5.11.2 and the pinned Geometry Dash 2.2081 bindings snapshot. The workflow uses the October/November 2026 bindings commit recorded above so the generated 2.2081 bindings include their required type dependencies and current Cocos mappings.

The manifest uses `>=5.0.0`. Geode's loader requires matching major versions, so this admits Geode 5.x and does not claim compatibility with Geode 4.x or earlier.

The SDK's CMake packaging helper requires the manifest's `geode` field to exactly equal the SDK version during packaging. CI temporarily changes the build workspace manifest to `5.11.2`, builds the mod, then restores `>=5.0.0` inside the generated `.geode` archive.

The GitHub Actions workflow builds three packages on every push and pull request:

- Windows x64 (`Win64`)
- Android armv7 (`Android32`)
- Android arm64 (`Android64`)

It uses `geode-sdk/build-geode-mod` and uploads the resulting `.geode` packages as separate workflow artifacts.

Local build requirements: CMake 3.21+, the Geode 5.11.2 SDK, and the `GEODE_SDK` environment variable pointing to that SDK. Configure with the same Geometry Dash version values used by CI:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DGEODE_GD_VERSION=2.2081 \
  -DGEODE_COMP_GD_VERSION=22081 \
  -DGEODE_DISABLE_FMT_CONSTEVAL=ON
cmake --build build --config Release
```
