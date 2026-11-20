# Command API

One JSON command set, reachable four ways:

| Transport | How |
|---|---|
| WebSocket | `ws://host:8080/ws`: send `{"cmd": ..., "req": 1}` and get the reply with the same `req` |
| HTTP | `POST /api/command` with the command as the body. Shortcuts: `GET /api/state`, `GET /api/list` |
| C API | `rs_host_command(host, json, origin, out, outlen)` |
| Console | `roastsimd` → `json {"cmd": ...}` (or the short forms; type `help`) |

Replies are objects with `"ok": true|false` (and `"error"`) unless noted.

## Commands

| cmd | parameters | effect / reply |
|---|---|---|
| `state` | `full` (bool) | live state (below); `full` adds `roaster_def` |
| `list` | | `roasters` (id, name, kind, controls, probes, batch limits), `beans` |
| `load` | `roaster`, `bean` (either optional) | load presets; cold, empty machine |
| `bean` | `id` | green coffee for the next charge |
| `set` | `control` + `value`, or `controls: {heat: 80, fan: 60}` | set controls in %. Names: `heat` (`burner`, `gas`, `power`), `fan` (`air`, `airflow`, `damper`), `drum`, `stir`, `voltage` (`variac`), `cooling` |
| `power` | `value` (bool; toggles if absent) | master switch (default **off**): with the machine switched off every actuator is forced to 0 and the machine just cools passively, while the control settings are kept for when it is switched on; `preheat` refuses to run |
| `batch` | `batch_g` | batch size for the next charge |
| `charge` | `batch_g` (optional) | beans in |
| `drop` | | beans to the cooling tray |
| `cool_end` | | end cooling (COOL event); otherwise automatic below 40 °C |
| `mark` | `event`: DRY, FCs, FCe, SCs, SCe, or free text (NOTE) | manual marks (stop auto-detection of that mark) |
| `preheat` | `target_C` (or `target` in logger units) | fast-forwards the empty machine with a PI controller until ET holds the target; reply `simulated_s` |
| `empty` | | discard beans, keep machine temperatures |
| `reset` | | discard beans, machine back to ambient, history cleared |
| `repair` | | replace a blown thermal fuse |
| `env` | any of `ambient_C`, `humidity_rh`, `line_voltage`, `source_ohms`, `altitude_m`, `gas_pressure` | environment; reply `env` |
| `fault` | `probe` (BT, ET, …), `type` (`none`, `open`, `short`, `spikes`, `drift`, `stuck`, `noisy`), `param` | probe fault injection |
| `time_scale` | `value` (0–120] | speed when no logger is connected |
| `pause` | `value` (bool; toggles if absent) | |
| `step` | `seconds` | advance the simulation immediately (testing, teaching) |
| `units` | `value`: C or F | units reported to Artisan (Modbus/WS/TC4) |
| `parcels` | `value` | bean parcels for the next charge (default 200) |
| `history` | `since` (index), `max` | `samples` as `[t, t_roast, bt, et, ror_bt, ror_et, bean_true, heat, fan, drum]`, `next` |
| `events` | `since` (seq) | `events`: `{seq, type, detail, origin, t, t_roast, bt}` |
| `report` | | roast report: times, DTR, Agtron whole/ground/spread, weight & moisture loss, defects, notes |
| `visual` | `max` | render state for 3D/2D views; `beans` as `[r, g, b, size_mm, oil, temp, count, exposure, cracked, scorch]` |
| `server` | | listener status, connected clients, logger status, config |
| `subscribe` | `history_from`, `event_seq`, `value` | **WebSocket only**: stream `{"type":"state", … , "history":[…]}` at 4 Hz plus `{"type":"event", …}` messages |
| `audio` | `value` (bool), `rate` (8000–48000, default 24000) | **WebSocket only**: stream the roaster's sound as binary frames of int16 little-endian mono PCM (see [SOUND.md](SOUND.md)) |
| `audio_mix` | `master`, `machine`, `cracks` (0–1) | **WebSocket only**: levels for this connection's sound stream |

Event types: `CHARGE`, `TP`, `DRY`, `FCs`, `FCe`, `SCs`, `SCe`, `DROP`, `COOL`, `ALARM` (`detail` = alarm name), `NOTE` (`detail` = free text; the simulator writes `power on` / `power off` and `cold reset`, the first event after a cold reset or roaster load, which front ends use to start a new graph).

Alarm names (and C API bit numbers): `thermostat_open` (0), `fuse_blown` (1), `beans_ejecting` (2), `bed_stalled` (3), `scorching` (4), `smoke_heavy` (5), `fire_risk` (6), `overheat` (7), `centrifuging` (8), `probe_fault` (9).

## State object (abridged)

```json
{
  "type": "state", "t": 812.3, "t_roast": 455.1, "phase": "roasting", "bean_location": "machine",
  "roaster": {"id": "drum_home_1kg", "name": "...", "kind": "drum"},
  "bean": {"id": "colombia_washed", "name": "..."},
  "bt": 188.2, "et": 171.5, "ror_bt": 7.1, "ror_et": 2.0, "eta_dry_s": -1, "eta_fc_s": 142,
  "probes": [{"id": "BT", "location": "bean_mass", "value": 188.2, "fault": false}],
  "controls": {"heat": {"value": 75, "actual": 75}, "fan": {"value": 40, "actual": 40}},
  "machine": {"heater_W": 1620, "airflow_g_s": 2.4, "inlet_C": 22, "exhaust_C": 171, "wall_C": 230,
              "agitation": 0.32, "mix_quality": 0.97, "fill": 0.09, "drum_rpm": 48, "bed_state": "tumbling"},
  "beans": {"present": true, "T_mean": 185.1, "T_surface": 187.0, "T_core": 184.1, "moisture_pct": 3.9,
            "weight_loss_pct": 9.8, "agtron_whole": 92, "agtron_ground": 99, "agtron_sd": 4.1,
            "fc_frac": 0, "sc_frac": 0, "level": "...", "rgb": [0.71, 0.53, 0.32]},
  "smoke": 0.21, "pops_fc": 0, "pops_sc": 0, "alarms": [], "time_scale": 1, "paused": false, "power": true,
  "logger_connected": true, "event_seq": 7
}
```

`eta_dry_s` / `eta_fc_s` are the simulator's estimate of roast seconds until it would call DRY (bean mass mean at 150 C) and first crack (2 % of beans cracked, using each bean group's own crack threshold). The beans are projected forward along a Newton heating curve: they level off at the temperature the machine is driving them towards (`machine.bean_env_C`: hot air, drum wall, pan and radiation weighted by how strongly each couples to the beans), with the time constant that matches their current smoothed heating rate. Values are `-1` when not applicable (not roasting, the first 20 s after CHARGE, the event has happened or been marked, or, for first crack, before DRY: until drying ends evaporation slows the beans in a way the projection can't see) and `-2` when the milestone is still ahead but won't be reached at the current heat (stalling). The estimate assumes the controls stay where they are, so it moves when you change heat or airflow, and it tightens as the milestone nears. The C API has the same values in `rs_state.eta_dry_s` / `eta_fc_s` (API 1.2).

## Host configuration (`roastsim.json` / `--config`)

```json
{
  "bind_address": "127.0.0.1",
  "http_port": 8080, "http_enabled": true,
  "modbus_port": 502, "modbus_enabled": true,
  "tc4_port": "", "tc4_baud": 115200,
  "roaster": "popper_modded", "bean": "colombia_washed",
  "units": "C", "time_scale": 1, "lock_realtime_with_logger": true,
  "seed": 0, "parcels": 200,
  "artisan_ws": { "push_charge_drop": true, "push_crack_events": true }
}
```

The Win32 shell reads `roastsim.json` next to its `.exe`. `roastsimd` takes `--config FILE`, and its command-line flags override the file.

## C API

`service/include/roastsim/roastsim.h`: `rs_host_create/start/stop/destroy`, `rs_host_command` (JSON), and typed fast paths `rs_host_get_state`, `rs_host_set_control`, `rs_host_get_history`, `rs_host_get_visual`. Structs carry `struct_size` for forward compatibility. All functions are thread-safe.
