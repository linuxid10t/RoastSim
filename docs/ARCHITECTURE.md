# Architecture

RoastSim is a set of layers. Each layer may only depend on the layers below it.

```
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ Front ends                                                                 │
 │  apps/win32 (native Windows)   apps/gtk (Linux, planned)   apps/haiku      │
 │  web/ (browser UI)             apps/roastsimd (headless + console)         │
 │  future: 3D view (renderer library hosted by each native shell)            │
 ├────────────────────────────────────────────────────────────────────────────┤
 │ C API  service/include/roastsim/roastsim.h      (stable, versioned ABI)    │
 ├────────────────────────────────────────────────────────────────────────────┤
 │ Service  (roastsim_service)                                                │
 │  Host: owns the Simulation, real-time pacing thread, mutex, JSON API       │
 │  Protocol adapters: Modbus TCP · HTTP + WebSocket (UI + Artisan) · TC4     │
 │  Platform layer: sockets · serial/pty · executable paths   (only OS code)  │
 ├────────────────────────────────────────────────────────────────────────────┤
 │ Core  (roastsim_core)   pure C++17, no OS headers, no threads, no sockets  │
 │  Simulation · MachineModel (fluid bed / drum / stir pan) · BeanBatch       │
 │  Probes · RoasterDef/BeanSpec (data-driven JSON) · Scenario runner         │
 └────────────────────────────────────────────────────────────────────────────┘
```

## Portability rules

| Layer | Allowed dependencies | Notes |
|---|---|---|
| core | C++17 standard library only | `<filesystem>` is used only by the catalog loader. Deterministic `Rng` instead of `<random>` distributions, which differ between libstdc++ and MSVC. Locale-free number formatting (`to_chars`/`from_chars`). |
| service | core + `platform_*.cpp` | All `#ifdef _WIN32 / __HAIKU__` code lives in three files: `platform_net.cpp` (Winsock vs BSD sockets; Haiku has BSD sockets via `libnetwork`), `platform_serial.cpp` (Win32 COM vs termios/`posix_openpt`) and `platform_paths.cpp`. |
| front ends | C API (+ the core's JSON helper for parsing replies) | Each OS gets a native shell. The C API means a shell can be written in C (GTK), C++ (BeAPI) or anything with an FFI. |

The CMake build already handles `HAIKU` (links `network be`) and Linux (`util`, `Threads`). Porting means writing a shell. The core and service should build unchanged with Haiku's GCC 13 and any current Linux GCC/Clang.

## Threads

```
sim thread   ── every 10 ms: lock, Simulation::step(wall_dt × time_scale), unlock
net thread   ── select() over Modbus + HTTP/WS sockets; calls Host (locks briefly)
tc4 thread   ── blocking serial reads (100 ms timeout); calls Host
UI thread(s) ── C API calls → Host (copy snapshot under the lock, render without it)
```

`Host` owns one mutex around the `Simulation`. Readers take **copies** (`Snapshot`, `VisualState`, history slices), so a slow renderer never stalls the physics. The simulation integrates in fixed 50 ms steps (`Simulation::kStep`). Results don't depend on how often or how irregularly the host calls `step()`, and the same seed gives the same roast.

**Time:** while a logger (Artisan via Modbus, WebSocket or TC4) has been active in the last 5 s, the host forces 1× real time, because Artisan samples on wall-clock time. Otherwise the speed can be paused or set to 1–120×. Scripted roasts (`roastsim_script`, `roastsimd --script`) run as fast as the CPU allows. A 12-minute roast takes well under a second.

## The command API is the only control path

Every front end, and every Artisan adapter, ends up in `Host::command(json, origin)` or its typed fast paths. Consequences:

* Whatever one front end can do, all of them can: web, native, console, Artisan buttons via `{"command":"charge"}`, Modbus register 110.
* Each change carries an **origin** (`ui`, `web`, `modbus`, `artisan-ws`, `tc4`, `sim`, …). The WebSocket adapter uses it to avoid echoing a CHARGE back to the Artisan instance that sent it.
* UIs never cache authority. They poll or subscribe and re-render, so several UIs and Artisan can control the same roaster at once (last writer wins).

## Physics in one paragraph

Each **machine model** advances its own thermal nodes: heater element, drum shell(s), pan, chamber wall, plus quasi-steady air. It then writes, for every bean **parcel**, the heat flowing into its surface (`q_in`) and the hottest local surface temperature (`spot_T`, used for scorch). `BeanBatch` integrates each parcel as two nodes (surface and core) with Schwartzberg-style drying, latent heat, Arrhenius exothermic pyrolysis, browning and development integrals, first and second crack per bean, expansion, chaff and oil. Mixing is an Ornstein–Uhlenbeck process per parcel, with a correlation time set by the machine (fan lift, drum Froude ratio, stir-arm rpm). That is what makes a stalled popper or a stopped stir arm roast unevenly. Probes are first-order lags whose time constant depends on immersion, with noise, quantisation and injectable faults. See [PHYSICS.md](PHYSICS.md).

## Plan for the 3D view

You chose a **shared C++ OpenGL renderer hosted by each native shell**. The groundwork is already in place.

### What exists now

* `core/include/roastsim/visual_state.hpp` is the render contract: per-parcel colour, size, oil, temperature, crack fraction, scorch, plus machine pose (drum or stir-arm angle), agitation, fill, element glow, smoke, chaff rate and monotonic crack counters for spawning particles and sounds.
* `Host::visual()`, `rs_host_get_visual()` (C API) and `{"cmd":"visual"}` (JSON) all expose it. A 3D client can therefore be in-process (native shell) or remote (WebSocket).
* Both current UIs already draw a 2D preview from this exact data. It proves the data path and gives the 3D renderer something to replace.

### Contract

1. **The simulation owns all physics; the renderer is read-only.** Nothing visual feeds back. The renderer may invent purely cosmetic motion (bean trajectories in the fluid bed, tumbling in the drum) seeded from `agitation`, `mix_quality`, `angle_rad` and `fill`.
2. **One parcel, many rendered beans.** With ~200 parcels and 600–33 000 real beans, the renderer instances N beans (e.g. 1–3 k) and assigns each to a parcel. Colour and size come from the parcel; position comes from a per-family motion model in the renderer.
3. **Decoupled rates.** The renderer runs at display rate, snapshots `VisualState` at ~20 Hz and interpolates between snapshots.

### Proposed module layout (next milestone)

```
render/                    roastsim_render (static lib), C++17 + OpenGL 3.3 core
  include/roastsim/render.h  C API: rs_render_create(), rs_render_frame(w,h,dt), rs_render_set_camera()
  src/scene_fluidbed.cpp     glass chamber, spouting bean fountain, chaff particles
  src/scene_drum.cpp         drum + vanes rotating with angle_rad, cascading bed, cooling tray
  src/scene_stirpan.cpp      pan, rotating arm, dome with condensation/smoke
  src/beans.cpp              instanced bean mesh, per-instance colour/size/oil/scorch
  src/particles.cpp          smoke, chaff, crack "puffs" keyed off pops_fc/pops_sc
  src/gl_loader.cpp          tiny GL function loader (no GLFW/GLEW dependency)
```

Host surfaces per OS:

| OS | GL surface | Context |
|---|---|---|
| Windows | child `HWND` in the Win32 shell | WGL (`wglCreateContextAttribsARB`, 3.3 core) |
| Linux | `GtkGLArea` | GTK provides the context |
| Haiku | `BGLView` | Mesa. Software (llvmpipe) on most machines, so the renderer needs a "low" quality tier (fewer instances, no shadows, no smoke volume). |

The renderer gets the GL context from the shell. It never creates windows, which keeps its code identical on all three platforms. The web UI can later get a three.js view that reads the same `{"cmd":"visual"}` JSON.

### Data still to add when the renderer arrives

* Per-parcel stable IDs and positions are not needed (positions stay cosmetic). A per-parcel `bias` export would let "hot-zone" beans sit nearer the heat source in the bean fountain.
* Sound: crack counters are already exposed. Add fan and burner loudness from `fan` and `heat`.
* Optional: per-family geometry details (vane count, screen size) in the roaster JSON under a `"visual"` block. The loader already ignores unknown keys.

## Sound

`AudioSynth` (service layer, portable C++) turns the live simulation into the roaster's sound: machine layers from telemetry, and individual cracks from `Simulation::cracksSince()`, which carry each cracking bean's size, hardness, moisture and loudness. Native shells play it locally or pull samples through `rs_audio_render()`; the web server renders one stream per listening browser. See [SOUND.md](SOUND.md). Crack events are also meant to drive crack particles in the future 3D view.

## Adding things

* **A roaster:** copy a JSON file in `data/roasters/`. A new *family* means a new `MachineModel` subclass (one `.cpp`) plus a `RoasterKind` value.
* **A protocol:** add an adapter next to `modbus_server.cpp` that translates its messages into `Host::command` or `Host::setControl` calls.
* **A front end:** link `roastsim_service`, include `roastsim.h`, and follow `apps/win32/main.cpp` as the reference.
