# StyleScanner — Style Scanner & Deco Copier

Geode mod ID: `developer.stylescanner`  
Version: `1.0.0`  
Game target: Geometry Dash `2.2081`  
Platforms: Windows x64 and Android (armv7 / arm64)  
Geode compatibility: Geode 3.x (SDK build target 3.5.0)

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

The Geode 3.x loader parses a single version comparator in the `geode` field, rather than a two-sided expression such as `>=3.0.0 <4.00.0`. Its comparator also rejects a mismatched major version. The checked-in manifest therefore uses `>=3.0.0`, which admits Geode 3.x and rejects Geode 4.x under the loader's major-version rule.

Geode SDK 3.5.0's CMake packaging helper separately requires the manifest's `geode` value to exactly match the SDK version during the build. The CI workflow temporarily changes the build workspace's manifest to `3.5.0`, builds the mod, and then patches the `.geode` archive back to the loader-supported `>=3.0.0` constraint before uploading it.

The GitHub Actions workflow builds three packages on every push and pull request:

- Windows x64 (`Win64`)
- Android armv7 (`Android32`)
- Android arm64 (`Android64`)

It uses `geode-sdk/build-geode-mod`, Geode SDK 3.5.0, and uploads the resulting `.geode` packages as separate workflow artifacts.

Local build requirements: CMake 3.21+, a Geode SDK installation, and the `GEODE_SDK` environment variable. For a local build using SDK 3.5.0, change the `geode` value to the exact string `3.5.0` before CMake configuration. After packaging, use `>=3.0.0` in the package's embedded `mod.json` for loader compatibility.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --config Release
```
