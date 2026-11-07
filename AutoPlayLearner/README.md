# AutoPlay Learner

A Geode mod project for Geometry Dash 2.2081.

## Editor level scan

1. Open a level in the Editor.
2. Open the Editor pause menu and choose **Analyze Level**.
3. Enter a name for the scan.
4. The mod scans the saved level objects and writes a named scan under the mod save directory in `level-scans/`. The report includes object count, effect/trigger-object count, horizontal object span, and a level fingerprint.

This is a structural scan of the editor data. It does not simulate all Geometry Dash physics or automatically invent a valid path through an unplayed level.

## Named input routes

1. Open the level in normal Play mode and open Pause.
2. Choose **Record Named**, enter a route name, and play normally after the level restarts.
3. The route is saved when the player dies, completes the level, leaves it, or manually stops recording.
4. Open Pause and choose **Saved Routes**. Select a saved route to restart and replay it.

Routes are stored separately under the mod save directory in `routes/`; multiple named routes can be kept for the same level.

## What is recorded

- Game command tick for each press/release.
- Input channel and player side.
- Active mode at the time of input (Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing).
- Press and release events, including held inputs.

The mode summary is descriptive. The game's portals and triggers change the player mode during replay; the mod repeats the saved inputs.

## Limits

- A real trial input sequence must be recorded before it can be replayed. A static editor scan alone cannot supply a full playable input route.
- Replay expects the same level ID and level name, with the same level content and gameplay settings for reliable results.
- This version records one continuous run; it does not merge multiple practice checkpoints into a complete route.
- It replays captured inputs. It is not an AI that solves unseen levels.

## Build

The GitHub Actions workflow `.github/workflows/autoplay.yml` builds Windows x64, Android armv7, and Android arm64.
