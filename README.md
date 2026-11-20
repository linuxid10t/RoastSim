# RoastSim 2026

A virtual coffee roaster that roast-logging software such as **Artisan** can connect to as if it were real hardware. It simulates the heat transfer, moisture loss, browning, cracks, smoke and probe behaviour of:

* **Fluid-bed air roasters**
* **Popcorn poppers**: stock (variac, thermostat, thermal fuse) and modded (SSR heater + separate fan)
* **Drum roasters**: home induction 1 kg, tabletop IR 250 g, shop gas 5 kg and 15 kg, and any drum you describe in JSON
* **Stir-arm pan roasters**

Artisan connects over **Modbus TCP**, **WebSocket** (Artisan's default dialect, including auto CHARGE/DROP/crack marks), or **TC4/aArtisanQ serial**. You can also drive the roaster directly from the **native Windows app**, the **web UI**, or the **headless console**.

```
roastsim_win32.exe   native Windows control panel (hosts all servers)
roastsimd(.exe)      headless simulator + console; same servers; Windows/Linux/Haiku
roastsim_script      run scripted roasts at full speed (calibration, testing, teaching)
roastsim_check       validate presets and run reference roasts
roastsim_sound       render the roaster's sound to a WAV file
http://localhost:8080/   web control panel (served by either of the above)
```

## Quick start (Windows)

```powershell
cmake -S . -B build -G Ninja
```

```powershell
cmake --build build
```

```powershell
.\build\roastsim_win32.exe
```

The build needs MSYS2's UCRT64 toolchain (`C:\msys64\ucrt64\bin`) on `PATH`. Binaries are statically linked, so `build\*.exe` together with `build\data` and `build\web` can be copied anywhere.

Then in Artisan: **Config » Device » MODBUS**, host `127.0.0.1`, port `502`, Input 1 register 1 (ET), Input 2 register 0 (BT), function 3, divider 1/10, int16. The full guide, including WebSocket and TC4, is in [docs/ARTISAN_SETUP.md](docs/ARTISAN_SETUP.md).

## Linux / Haiku

The core and service layers are portable C++17 with all OS code isolated in `service/src/platform_*.cpp`:

```sh
cmake -S . -B build && cmake --build build
./build/roastsimd --lan --tc4 pty      # web UI on :8080, Modbus on :502, TC4 on a pty
```

On Linux, port 502 needs privileges. Use `--modbus-port 5020` and set the same port in Artisan. Native GTK and Haiku BeAPI shells are planned against the same C API; see `apps/gtk/README.md` and `apps/haiku/README.md`.

## Using it

* **Power:** the machine boots switched off. Flip the power switch (web UI, native app, console `power on`) before preheating or roasting. Switched off, the dials keep their settings but nothing heats, blows or turns, and the machine just cools to ambient.
* **Roast:** pick a roaster and bean, set the batch, **Preheat** (drums), set heat/fan, **Charge**, watch for the turning point, DRY and first crack (with crack sounds), then **Drop**. The roast report (times, DTR, Agtron whole/ground/spread, weight loss, defects) appears when cooling ends.
* **Environment:** ambient temperature, line voltage and sag (big for poppers), altitude (less air mass), humidity, gas pressure.
* **Faults:** open/shorted/drifting/spiking probes to test Artisan setups; blow the popper's thermal fuse by choking its airflow, then **Repair**.
* **Speed:** pause, or run 1–120× when no logger is connected. Artisan connecting locks it to real time.
* **Sound:** hear the blower, burner and drum, the beans tumbling, and every first and second crack, voiced from each bean's size, hardness and moisture (a dense Kenya pops loud and deep, a decaf barely ticks). Use **Simulation ▸ Sound** in the Win32 app, the **Sound** checkbox in the web UI, or `roastsimd --sound`. `roastsim_sound` renders WAV files offline. See [docs/SOUND.md](docs/SOUND.md).

## Testing

```powershell
.\build\roastsim_tests.exe data
```

The 23 tests cover physics envelopes for every preset, the density effect, crack sounds per bean, the sound engine, determinism, Modbus framing and registers, the Artisan WebSocket dialect and push messages, HTTP, TC4 commands and the C API.

## Project layout

```
core/      pure C++17 simulation (no OS code): beans, machines, probes, scenarios
service/   host (real-time thread, JSON API), Modbus/HTTP/WebSocket/TC4, platform layer, C API
apps/      roastsimd (headless), win32 (native shell), gtk + haiku (planned)
web/       browser control panel (served by the host)
data/      roaster and bean presets (JSON; add your own)
scripts/   scripted calibration roasts
tests/     test-suite
docs/      ARCHITECTURE (incl. the 3D plan), ARTISAN_SETUP, API, PHYSICS, SOUND
```
