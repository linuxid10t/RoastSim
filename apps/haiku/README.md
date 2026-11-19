# Haiku native shell (planned)

A BeAPI front end (`BApplication` / `BWindow`) over the C API.

* **Layout:** `BLayoutBuilder` with `BMenuField` (roaster, bean), a `BSlider` per control, `BButton`s for Charge/Drop/Preheat, and a custom `BView` graph (`Draw()` from `rs_host_get_history()` samples).
* **Refresh:** a `BMessageRunner` posting a refresh message every 200 ms. The C API is thread-safe, so window threads can call it directly.
* **3D:** host the shared renderer in a `BGLView` (Mesa). Most Haiku machines render in software (llvmpipe), so the renderer needs its low-quality tier.
* **Sound:** `BSoundPlayer` with the same synthesised crack clicks as the Win32 shell (`makeClick()` in `apps/win32/main.cpp`).

The service layer already handles Haiku:

* Sockets come from `libnetwork`, which CMake links when `HAIKU` is set.
* `posix_openpt` provides the TC4 pseudo-terminal (`--tc4 pty`).
* `get_next_image_info` locates the executable so `data/` and `web/` are found next to it.

Until the shell exists:

```sh
cmake -S . -B build && cmake --build build
./build/roastsimd --tc4 pty
```

Then open `http://localhost:8080/` in WebPositive. Haiku's packaged GCC (13.x) builds the C++17 code as-is; this has not yet been tried on real Haiku.
