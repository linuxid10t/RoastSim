# Linux native shell (planned)

A GTK 4 front end over the C API (`service/include/roastsim/roastsim.h`). It should mirror `apps/win32/main.cpp`:

* `rs_host_create()` / `rs_host_start()` at startup. The servers (Modbus, WebSocket, web UI, TC4 on a pty) come for free.
* A `GtkDropDown` for roaster and bean, filled from `rs_host_command("{\"cmd\":\"list\"}")`.
* A `GtkScale` per control in the roaster definition (`{"cmd":"state","full":true}` → `roaster_def.controls`), wired to `rs_host_set_control()`.
* A `GtkDrawingArea` graph fed incrementally by `rs_host_get_history()`, plus a bean preview from `rs_host_get_visual()`.
* A `GtkGLArea` later, to host the shared 3D renderer (see `docs/ARCHITECTURE.md`).
* A 200 ms `g_timeout_add` for refresh. Every C API call is thread-safe and cheap.

Until then, run `roastsimd` and use the web UI at `http://localhost:8080/`.

Build hook: add `if(UNIX AND NOT APPLE AND NOT HAIKU AND RS_BUILD_NATIVE_UI)` with `pkg_check_modules(GTK4 gtk4)` to the top-level `CMakeLists.txt`.
