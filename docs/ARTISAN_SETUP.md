# Connecting Artisan

RoastSim offers three Artisan device types at the same time. Pick one:

| Option | Best for | Control from Artisan | Extra software |
|---|---|---|---|
| **Modbus TCP** (port 502) | most setups; closest to commercial roasters | sliders/buttons write registers | none |
| **WebSocket** (port 8080, path `WebSocket`) | auto CHARGE/DROP/FC marks pushed from the simulator | sliders/buttons send JSON | none |
| **TC4 serial** | testing TC4/aArtisanQ setups, Artisan's firmware PID | `OT1`/`OT2` serial commands | Windows: a virtual COM pair (e.g. com0com). Linux/Haiku: none (`pty`) |

Defaults listen on `127.0.0.1`. To run Artisan on another machine, start with `--lan` (or `"bind_address": "0.0.0.0"` in `roastsim.json`) and allow the ports through the firewall.

While Artisan is connected, the simulator is locked to real time. Temperatures are °C unless you start with `--units F`.

---

## 1. Modbus TCP

**Config » Device:** choose `MODBUS`.

**Config » Port » Modbus tab:**

| Field | Value |
|---|---|
| Type | TCP |
| Host | 127.0.0.1 |
| Port | 502 (or whatever `--modbus-port` says) |
| Input 1 (Artisan shows it as ET) | Device 1 · Register **1** · Function **3** · Divider **1/10** · Decode **int16 (signed)** · Mode C |
| Input 2 (BT) | Device 1 · Register **0** · Function 3 · Divider 1/10 · int16 · C |
| Input 3/4 (optional extra probe) | Register 2 / 3, same settings |

Device ID 0 tells Artisan to skip the input, so use 1 (any ID 1–247 is answered). Function 4 (input registers) returns the same values as function 3. Registers are 0-based. If you type 40001-style addresses, Artisan converts them.

### Register map

| Reg | Read (FC3/FC4) | | Reg | Write (FC6/FC16), also readable |
|---|---|---|---|---|
| 0 | BT ×10 | | 100 | heat % |
| 1 | ET ×10 | | 101 | fan % |
| 2 | 3rd probe ×10 (Inlet / Exhaust / Drum / Dome) | | 102 | drum % |
| 3 | 4th probe ×10 | | 103 | stir % |
| 4 | ambient ×10 | | 104 | voltage % (variac, stock popper) |
| 5 | BT RoR ×10 (°/min) | | 105 | cooling tray (0/100) |
| 6 | ET RoR ×10 | | 110 | command: 1 CHARGE · 2 DROP · 3 COOL END · 4 empty · 5 cold reset · 6 repair · 10 DRY · 11 FCs · 12 FCe · 13 SCs · 14 SCe |
| 7–12 | heat, fan, drum, stir, voltage, cooling % | | 111 | batch size, g |
| 13 | phase 0 idle · 1 roasting · 2 cooling · 3 done | | 112 | preheat target (writing it starts a fast preheat) |
| 14 | roast time, s | | | |
| 15 | batch, g | | Coils (FC5/FC15) | 0 charge · 1 drop · 2 cooling tray |
| 16 | event flags (bit0 CHARGE, 1 DRY, 2 FCs, 3 FCe, 4 SCs, 5 SCe, 6 DROP, 7 COOL) | | Discrete inputs (FC2) | 0 roasting · 1 FC started · 2 SC started · 3 beans in tray |
| 17 | alarm bits | | | |
| 18 | true mean bean temperature ×10 (no probe lag; useful for teaching) | | | |
| 19 | Agtron (ground) ×10 | | | |
| 20 | heater power, W | | | |
| 21 | smoke % | | | |
| 22 | moisture % ×100 | | | |
| 23 | weight loss % ×100 | | | |

An open (faulted) probe reads `-10`, which shows as `-1.0`, Artisan's "no value".

### Sliders and buttons

**Config » Events » Sliders:** set the action to `MODBUS Command`:

| Slider | Command |
|---|---|
| Burner / Heat | `writeSingle(1,100,{})` |
| Air / Fan | `writeSingle(1,101,{})` |
| Drum | `writeSingle(1,102,{})` |
| Variac (stock popper) | `writeSingle(1,104,{})` |

**Config » Events » Buttons** (or the CHARGE/DROP button actions under *Config » Events*, "MODBUS Command"):

| Button | Command |
|---|---|
| CHARGE | `writeSingle(1,110,1)` |
| DROP | `writeSingle(1,110,2)` |
| COOL END | `writeSingle(1,110,3)` |
| Preheat to 200 °C | `writeSingle(1,112,200)` |

The roaster's own controls ignore registers it doesn't have. For example, the stir pan has no drum.

---

## 2. WebSocket

RoastSim uses **Artisan's default WebSocket node names**, so only host, port and channel nodes need setting.

**Config » Device:** choose `WebSocket`.

**Config » Port » WebSocket tab:**

| Field | Value |
|---|---|
| Host | 127.0.0.1 |
| Port | 8080 |
| Path | `WebSocket` (any path works) |
| ID | 0 |
| Message ID / Machine ID / Command / Data nodes | `id` / `roasterID` / `command` / `data` (Artisan defaults) |
| Data Request | `getData` |
| Input nodes | ET → `ET`, BT → `BT` (extra channels: `Inlet`, `Exhaust`, `Drum`, `Dome`, `ambient`, `RoR_BT`, `bean_true`, `agtron`, `moisture`, `smoke`, `heat`, `fan`, …) |
| Push message node | `pushMessage` |
| CHARGE / DROP messages | `startRoasting` / `endRoasting` |
| Event message / node | `addEvent` / `event` |
| DRY / FCs / FCe / SCs / SCe | `colorChangeEvent` / `firstCrackBeginningEvent` / `firstCrackEndEvent` / `secondCrackBeginningEvent` / `secondCrackEndEvent` |
| START on CHARGE / OFF on DROP | tick both if you want the simulator to drive recording |

Each `getData` reply contains every probe plus controls and state, for example:

```json
{"id":4711,"roasterID":0,"data":{"BT":187.25,"ET":229.5,"ambient":22,"RoR_BT":9.4,"RoR_ET":0.5,
  "heat":100,"fan":70,"cooling":100,"state":"roasting","roast_time":201.5,"bean_true":181.9,
  "agtron":104.2,"moisture":5.26,"heater_W":1334,"smoke":12.4}}
```

### What gets pushed to Artisan

* **CHARGE / DROP** started anywhere else (web UI, native UI, Modbus, console) → `{"pushMessage":"startRoasting"}` / `{"pushMessage":"endRoasting"}`. Events Artisan itself triggered are not echoed back.
* **DRY, FCs, FCe, SCs, SCe** as detected by the bean physics → `{"pushMessage":"addEvent","data":{"event":"firstCrackBeginningEvent"}}`. Artisan only accepts these while recording. Turn them off with `"artisan_ws": {"push_crack_events": false}` in `roastsim.json` if you'd rather mark cracks yourself.

### Sliders and buttons

Slider action `WebSocket Command`. Inside slider and custom-button actions, literal braces must be doubled:

```
send({{"command": "setControlParams", "params": {{"heat": {}}}}})
send({{"command": "setControlParams", "params": {{"fan": {}}}}})
```

`burner`, `gas`, `air`, `airflow`, `damper`, `drum`, `stir`, `variac` and `cooling` are accepted as parameter names too.

Buttons (main CHARGE/DROP actions need no doubling):

```
send({"command": "charge"})
send({"command": "drop"})
send({"command": "preheat", "params": {"target_C": 200}})
send({"command": "mark", "params": {"event": "FCs"}})
```

Any native API command works as `{"command": "<cmd>", "params": {...}}`. See [API.md](API.md).

---

## 3. TC4 / aArtisanQ serial

The simulator behaves like a TC4 shield running aArtisanQ. TC1 = ET, TC2 = BT, and TC3/TC4 = the roaster's extra probes. `CHAN;1200` gives Artisan ET on chan1 and BT on chan2, as it expects.

**Windows:** install a virtual null-modem pair (com0com creates e.g. `COM20 ⇄ COM21`).

```powershell
.\roastsimd.exe --tc4 COM20
```

Then in Artisan: **Config » Device** `TC4`; **Config » Port** `COM21`, 115200 baud, 8N1.

**Linux / Haiku:** no driver needed. The simulator creates a pseudo-terminal and prints its path:

```
./roastsimd --tc4 pty
  TC4 serial: TC4 emulation on /dev/pts/5 @115200
```

Give Artisan that path as the serial port.

**Supported commands:** `CHAN`, `UNITS`, `FILT`, `READ`, `OT1` (heat, or the variac on a stock popper), `OT2`/`IO3`/`DCFAN` (fan, or drum speed if there's no fan), `PID;ON|OFF|SV;v|T;p;i;d|CHAN;n`. With the firmware PID on, `READ` adds `heater,fan,SV` as aArtisanQ does.

Slider actions: `Serial Command` with `OT1;{}` (heat) and `OT2;{}` (fan).

---

## Troubleshooting

* **"cannot bind 127.0.0.1:502"**: another program (or a second RoastSim) has the port. Use `--modbus-port 5020` and set the same port in Artisan.
* **Artisan shows -1**: a probe fault is active (Win32 **Machine ▸ Probe faults** menu / web UI) or the register or node name is wrong.
* **Curves look jagged in Artisan**: Artisan's sampling interval (Config » Sampling) of 1–3 s is normal. The modded popper's ET ripple is real, because its SSR heater runs a 2 s on/off cycle.
* **Time seems frozen**: once Artisan connects, the speed returns to 1× by design.
