# StyleScanner — Style Scanner & Deco Copier

Geode mod ID: `developer.stylescanner`  
Version: `1.0.0`  
Game target: Geometry Dash `2.2081`  
Platforms: Windows x64 and Android (armv7 / arm64)  
Build baseline: Geode SDK `4.10.2`, with the 2.2081 bindings pinned to commit `f58fcb8edf0a86aa89e4782efcf7dcc840c9c5c5`.

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

There is a real compatibility conflict in the requested version bounds: the Geode 3.x SDK released before the 2.2081 bindings were available, and SDK 3.5.0 ships only the older 2.206 bindings. It cannot produce a valid 2.2081 binary by simply compiling against today's bindings. The Geode 4.10.2 SDK is the compatible 4.x baseline used here.

Geode's loader version parser accepts one comparator and treats differing major versions as incompatible. The manifest therefore uses `>=4.0.0`, which admits Geode 4.x and rejects other major versions. It cannot truthfully be `>=3.0.0 <4.00.0` for a 2.2081 build.

The SDK's CMake packaging helper requires an exact `geode` value during packaging. CI temporarily changes the build workspace manifest to `4.10.2`, builds against the pinned 2.2081 bindings, then restores `>=4.0.0` inside the generated `.geode` archive.

The workflow builds three packages on every push and pull request:

- Windows x64 (`Win64`)
- Android armv7 (`Android32`)
- Android arm64 (`Android64`)

It uses `geode-sdk/build-geode-mod` and uploads the resulting `.geode` packages as separate workflow artifacts.

Local build requirements: CMake 3.21+, the Geode 4.10.2 SDK, and the `GEODE_SDK` environment variable pointing to that SDK. The build must use the 2.2081 bindings revision pinned in the workflow and configure with:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DGEODE_GD_VERSION=2.2081 \
  -DGEODE_COMP_GD_VERSION=22081 \
  -DGEODE_DISABLE_FMT_CONSTEVAL=ON
cmake --build build --config Release
```
