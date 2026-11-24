# RoastSim native Haiku UI — port plan

Recreate the current Win32 shell in BeAPI while reusing RoastSim’s simulation and service layer. Match the layout, graph, and workflow, but use native Haiku widgets rather than copying Windows chrome. Use the source as the reference for behavior. A screenshot of the entire Win32 window mid-roast, with a populated graph, will help tune visual fidelity; don’t wait for one to start.

**Implement milestone 1 only, then stop and report back.** The first deliverable is a native Haiku window that can run a roast and plot it live, not a full-parity port.

## Milestone 1 — Run a roast and plot live data

1. **Read the current implementation.** Start with `apps/win32/main.cpp`, `apps/win32/roastsim.rc`, `service/include/roastsim/roastsim.h`, and `CMakeLists.txt`. Check unfamiliar BeAPI signatures under `/boot/system/develop/headers/os/`.
2. **Add a `roastsim_haiku` CMake target.** Keep the shell in `apps/haiku/`. Gate it on `HAIKU AND RS_BUILD_NATIVE_UI` inside the existing service/apps build block. Link the existing `roastsim_service` and Haiku’s `be` library, and call `rs_copy_runtime_files(roastsim_haiku)`. Reuse the C API; leave the Win32 target and simulation core unchanged. Create the host with `rs_host_create(NULL, …)` so it reads and saves the user's settings (`~/config/settings/RoastSim/roastsim.json`), and keep the shell's own settings with `{"cmd":"ui_settings","shell":"haiku",…}`. Surface startup errors (including `config_error` from the `server` command), and stop refresh messages before releasing the host on shutdown.
3. **Wire up a native main window.** Use `BApplication`, `BWindow`, and `BLayoutBuilder`. Use `BMenuField` with radio-mode popup menus for roaster and bean, `BTextControl` for batch weight, and native buttons, checkboxes, and sliders. Wire up Charge, Drop, Mark, Power, and Cooling. Show the controls supported by the selected roaster, preserve stepped controls, and don’t let refresh overwrite a slider while the user is dragging it. Omit or disable any commands that aren’t implemented yet.
4. **Draw the live graph.** Refresh at 200 ms with `BMessageRunner`. Fetch history incrementally through `rs_host_get_history()` and draw from cached data in `BView::Draw()`. Implement time and temperature axes and live traces first. Start with targeted invalidation, not a copy of the Win32 memory-DC buffering. Defer event-label packing, phase bands, and other styling.
5. **Build, test, and smoke-test on Haiku.** From the repository root:

   ```sh
   cmake -S . -B build
   cmake --build build -j"$(nproc)"
   ctest --test-dir build --output-on-failure
   ./build/roastsim_haiku
   ```

   Select a roaster and bean, set the batch weight, and charge a batch. Change heat while the UI refreshes and verify the slider doesn’t jump back. Confirm the plot updates, then drop, resize the window, and close the app. If possible, capture a screenshot mid-roast. Report which checks you actually ran, what works, what remains, and which files changed. Distinguish automated test results from GUI checks.

## Later milestones — wait for approval before starting

- **Main-window parity:** Finish graph annotations, phase bands, readouts, and the 2D bean preview. Match end-of-roast graph freezing and reporting. Build the six-segment status strip from layout-managed views, not `BStatusBar` (a progress indicator). Only Speed and Environment are interactive; anchor the Speed popup with `BPopUpMenu::Go()`.
- **Dialogs and commands:** Port Environment, Preheat, and Preset as custom `BWindow` forms. Drive the Preset creator/editor from the sections returned by the service rather than assuming a fixed page count. Check menu commands, enabled states, units, and shortcuts against Win32.
- **Native sound:** Reuse the procedural engine through `rs_audio_render()` and add Haiku playback, for example with `BSoundPlayer`. The existing native playback backend is Windows-only (`service/src/platform_audio.cpp`); don’t bring back the old `makeClick()` approach.

The shared 3D renderer is planned separately. It is not required for this port.

## Headless fallback

The Haiku service path uses `libnetwork` for sockets, `posix_openpt` for TC4 pseudo-terminals, and `get_next_image_info` to locate runtime resources alongside the executable. Until the native shell is ready, build as above and run `./build/roastsimd --tc4 pty`, then open `http://localhost:8080/` in your browser.
