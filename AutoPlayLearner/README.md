# AutoPlay Learner

A separate Geode mod project for Geometry Dash 2.2081.

## What it does

1. Start a recording from the pause menu. The mod restarts the current level and captures every press/release event.
2. It records the game command tick, input button, player slot, and active mode (Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing).
3. Stop recording, die, complete the level, or leave the level. The latest trial is saved in the mod's Geode save folder.
4. Open the same level and choose **Replay Last**. The level restarts and AutoPlay sends the saved inputs back on the same command ticks.

The profile summary counts presses and estimates hold duration per mode. Mode changes are not forced by the mod: Geometry Dash's portals and triggers change the player's mode, while the macro repeats the recorded button inputs.

## Current limits

- Replay is intended for the same level, same level version, and same gameplay settings.
- This first version records one continuous run. It does not yet merge multiple practice checkpoints into a complete route.
- It replays recorded inputs; it is not an AI that can solve an unseen level or invent a new route.
- It supports the game's three input channels (jump / left / right), including holds and releases, plus two-player input attribution.

## Build

The GitHub Actions workflow `.github/workflows/autoplay.yml` builds Windows x64, Android armv7, and Android arm64 packages.
