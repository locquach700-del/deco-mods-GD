Auto Decorator - all block and spike variants

Files:
- CMakeLists.txt: includes the Windows Geode SDK duplicate-init compatibility fix.
- src/main.cpp: expanded block/slope and spike/hazard detection and decoration.
- mod.json: original mod metadata and settings.

Install these files into the repository, replacing CMakeLists.txt and src/main.cpp.
Keep the existing repository workflow files and other assets.

Detection coverage:
- Solid, slope and collision-object GameObjectType categories.
- Hazard and animated-hazard GameObjectType categories.
- Explicit list of known decorative/fake/invisible/coloured/wide/tiny spike assets.

Decoration:
- Block face motifs now apply to exposed, isolated and interior cells.
- Spikes receive a scale/rotation-aware highlight, base line and halo.
- Decoration objects remain marked No Touch.

Build verification:
This source has been statically inspected and packaged, but has not been compiled
against the user's current Geode SDK checkout. Run the existing GitHub Actions build
and inspect its log before treating this revision as build-verified.
