# Roaster preset fields

Defaults are in `core/include/roastsim/roaster_def.hpp`. Any parameter you omit takes its default. Units are SI unless the name says otherwise: W, kg/s, J/K, W/K, W/(m²·K), m, °C.

## Contents
1. Top level, controls, probes
2. `fluid_bed` (air roasters, poppers)
3. `drum`
4. `stir_pan`
5. `cooling`
6. Scaling rules for a new machine

## 1. Top level

```json
{
  "id": "my_roaster", "name": "Shown in pickers", "description": "1-2 sentences shown in the UIs",
  "kind": "fluid_bed | drum | stir_pan",
  "batch": { "default_g": 150, "min_g": 80, "max_g": 225 },
  "controls": [ ... ], "probes": [ ... ],
  "fluid_bed": { ... } | "drum": { ... } | "stir_pan": { ... },
  "cooling": { "h_on": 45, "h_off": 10 },
  "charge_bean_temp_C": 20
}
```

`charge_bean_temp_C` is optional; it defaults to ambient.

**Controls:** `{ "id", "label", "default", "min", "max", "step", "toggle", "slew_per_s" }`. All values are percent.

| id | meaning per family |
|---|---|
| `heat` | fluid bed: heater duty; drum: gas valve or element/induction power; stir pan: heater duty |
| `fan` | fluid bed: blower; drum: exhaust fan or damper (airflow); stir pan: dome fan |
| `drum` | drum rpm as % of `rpm_max` (if absent the drum runs at 75 %) |
| `stir` | stir-arm rpm as % of `stir_rpm_max` (if absent, 60 %) |
| `voltage` | stock appliance on a variac: % of line voltage to heater **and** motor (`max` may exceed 100, e.g. 110) |
| `cooling` | cooling tray toggle; added automatically if not declared |

A stock popper has only `voltage`. A modded popper has `heat` + `fan`. Gas drums should set `slew_per_s` on `heat`, because a valve doesn't jump.

**Probes:** `{ "id", "label", "location", "tau_air_s", "tau_bean_s", "noise_sd", "resolution", "offset" }`

* `BT` and `ET` are required. The ids of extras become Artisan WebSocket nodes and fill Modbus registers 2 and 3 in order.
* `location`: `bean_mass`, `inlet_air`, `exhaust_air`, `chamber_air` (drum air or dome air), `wall` (drum shell or pan surface), `element`, `ambient`.
* `tau_bean_s` sets the turning-point timing: the bigger it is, the later and higher the TP. Typical values are 5 s for a thin probe in a popper, 8–10 s for home drums and 14–16 s for shop drums with a thick probe well. `tau_air_s` is the time constant in moving air (3–20 s).
* `noise_sd` 0.1–0.3 °C; `resolution` 0.25 (MAX31855) or 0.1 / 0.5.

## 2. `fluid_bed`

| Field | Default | Meaning / typical |
|---|---|---|
| `wiring` | `"independent"` | `"stock"`: heater and fan motor share the `voltage` control and the fan slows with voltage. `"independent"`: separate `heat` and `fan`. |
| `heater_W`, `nominal_V` | 1400, 120 | Rated element power at its rated voltage; actual power follows (V/V_nom)². |
| `element_C` | 45 | Element thermal mass, J/K (30–60): about a 5–10 s heater lag. |
| `element_eff` | 0.9 | Element-to-air effectiveness. |
| `element_loss_UA` | 0.4 | Element losses to housing, W/K. |
| `fan_max_flow` | 0.0062 | Air mass flow at 100 % fan, kg/s. **Sets the inlet temperature**: ΔT ≈ heater_W / (flow · 1006). |
| `fan_tau` | 0.8 | Blower spin-up, s. |
| `fan_min_pct` | 15 | Independent wiring: the fan stalls below this. |
| `stock_stall_frac` | 0.45 | Stock wiring: the motor stalls below this fraction of nominal V. |
| `wall_C`, `wall_UA_in`, `wall_UA_out` | 350, 1.5, 1.2 | Chamber wall mass and exchange with air and ambient. |
| `h_ref` | 16 | Effective bean heat-transfer coefficient at full flow (14–24). Main speed knob. |
| `h_flow_exp` | 0.6 | How h scales with flow. |
| `fluidize_flow_per_kg` | 0.045 | Air (kg/s) needed per kg of green beans to lift them. Agitation = flow / (this × batch mass); stall below 0.7, eject above `eject_agitation`. |
| `eject_agitation` | 2.6 | |
| `unevenness` | 0.08 | Persistent hot/cold-zone bias (0.03 good fluid bed … 0.12 popper). |
| `thermostat`, `thermostat_open_C`, `thermostat_close_C` | false, 240, 215 | Bimetal cut-out on inlet air (stock poppers). |
| `fuse_C`, `fuse_hold_s` | 0, 3 | One-shot thermal fuse on inlet air (0 = none). |
| `pwm_period_s` | 0 | SSR time-proportioning period (2 s gives a realistic ET ripple); 0 = smooth power. |
| `chamber_diameter_m`, `chamber_height_m` | 0.08, 0.16 | Geometry: fill fraction and the 3D view. |

## 3. `drum`

| Field | Default | Meaning / typical |
|---|---|---|
| `heat_source` | `"gas"` | `gas`, `electric` (resistive, follows V²), `induction` (regulated), `ir` (quartz/halogen, part radiates straight at beans) |
| `heat_max_W` | 15000 | Burner or element rating at full. |
| `heat_min_frac` | 0 | Gas minimum flame as a fraction of full (0.1–0.15) whenever heat > 0. |
| `heat_tau` | 1.5 | Source response, s: gas 1–1.5, induction 0.5, element 5–20, IR 3–5. |
| `efficiency` | 0.65 | Fraction of fuel energy that enters the roaster (gas 0.55–0.7, electric 0.75–0.9). |
| `frac_drum`, `frac_air`, `frac_beans_rad` | 0.45, 0.55, 0 | Split of that energy: shell / drawn air / direct radiation onto beans. Should sum to about 1. Induction 1/0/0; conduction-heavy gas 0.6/0.4; "hot-air" drums 0.25/0.75; IR 0.45/0.15/0.4. |
| `nominal_V` | 0 | Electric: > 0 makes power follow line voltage and sag. |
| `drum_mass_kg`, `drum_cp` | 60, 500 | Thermal mass: the roaster's "momentum". |
| `drum_diameter_m`, `drum_length_m` | 0.5, 0.6 | Geometry: wall contact area, fill, critical speed 42.3/√D rpm. |
| `double_wall`, `outer_frac`, `wall_G` | true, 0.5, 150 | Two shell nodes; the outer takes the flame, `wall_G` W/K conducts inward. |
| `loss_UA` | 8 | Shell losses to ambient, W/K. |
| `airflow_max`, `airflow_min_frac`, `fan_tau` | 0.05, 0.25, 2 | Exhaust air mass flow at 100 % fan; the minimum is natural draft at 0 %. |
| `h_air_ref`, `h_flow_exp` | 12, 0.6 | Air-to-bean convection at full flow. |
| `wall_air_UA` | 20 | Drum wall ↔ drum air, W/K. Higher means ET tracks the drum more closely. |
| `h_contact` | 160 | Bed-to-wall contact conduction (140–220). |
| `emissivity` | 0.7 | Drum interior radiating onto the bed top. |
| `rpm_max` | 60 | rpm at 100 % drum control. |
| `unevenness` | 0.03 | |
| `bt_probe_full_kg` | 3.0 | Batch at which the BT probe is fully buried. Smaller batches read toward air temperature. About 0.5 × the default batch. |

## 4. `stir_pan`

| Field | Default | Meaning / typical |
|---|---|---|
| `heater_W`, `nominal_V` | 1200, 120 | |
| `pan_C` | 650 | Pan + element thermal mass, J/K (aluminium 0.5 kg ≈ 450 + element). |
| `pan_loss_UA` | 1.4 | Pan underside losses. |
| `pan_diameter_m` | 0.24 | Sets how many beans touch the pan (monolayer coverage). |
| `thermostat`, `thermostat_open_C`, `thermostat_close_C` | true, 270, 250 | Pan thermostat. |
| `h_contact` | 170 | Pan-to-bean conduction through the bean footprint. Main speed knob. |
| `h_dome`, `pan_air_UA`, `dome_vent_UA`, `dome_loss_UA` | 10, 3, 1.2, 1 | Dome air balance (lid/vent). Bigger vent or loss gives a cooler dome. |
| `fan_max_flow` | 0.004 | Dome fan at 100 %. |
| `stir_rpm_max` | 12 | Mixing correlation time ≈ 30/rpm s. |
| `unevenness` | 0.06 | |
| `pwm_period_s` | 0 | |
| `bt_probe_full_g` | 150 | Batch (g) burying the BT probe. |
| `dome_height_m` | 0.12 | For the 3D view. |

## 5. `cooling`

`h_on` (forced air, 35–60) and `h_off` (natural, about 10) for the cooling tray after DROP.

## 6. Scaling rules for a new machine

Start from the closest preset and scale. These ratios reproduce the calibrated presets:

**Fluid bed**
* `fan_max_flow` ≈ `heater_W` / (1006 × 210…225). This gives an inlet temperature around 230–250 °C at full fan.
* `fluidize_flow_per_kg` ≈ `fan_max_flow` × (default fan %/100) / (default batch kg × 1.3). At the default fan the default batch then sits at agitation about 1.3.
* `h_ref` 14 (big, gentle chamber) … 24 (popper vortex).
* `element_C` ≈ 30 J/K per kW.

**Drum**

| | Home electric/induction | Tabletop IR | Small gas / sample (≤ 2 kg, often cast iron, single wall) | Shop gas |
|---|---|---|---|---|
| `heat_max_W` per kg capacity | 1.8–3 kW | 4–5 kW (small batches) | 2–3 kW | 3.5–4.5 kW |
| `drum_mass_kg` per kg capacity | 4–6 | 5–6 | 12–14 (cast iron is heavy) | 12–14 |
| `airflow_max` per kg capacity | 0.007 kg/s | 0.02 kg/s | ~0.004 kg/s | 0.005–0.006 kg/s |
| `wall_air_UA` per m² of drum inner area (π·D·L) | ~50 | ~20 | ~32 | ~32 |
| `loss_UA` | 1–3 | 2–3 | 1–1.5 | 5 (5 kg) … 11 (15 kg) |
| `efficiency`, `frac_drum`/`frac_air` | 0.85–0.9, 1/0 (induction) | 0.75, 0.45/0.15 (+0.4 IR) | 0.70, 0.6/0.4, `double_wall: false`, `drum_cp` 460 | 0.6–0.65, 0.45/0.55 |

The small-gas column is calibrated: a 2.5 kW, 1.2 kg sample drum (`drum_sample_1kg_gas`) reaches FC at about 8:30 with 800 g at 70 % gas. Low burner power per kg leaves no room for losses, so airflow and `loss_UA` must stay at the low end. Use the energy check in `calibration.md` before tuning anything else.

* Drum diameter grows roughly as capacity^(1/3): 0.20 m for 1 kg, 0.45 m for 5 kg, 0.68 m for 15 kg. Length is about 1.2–1.3 × diameter.
* `rpm_max` such that the default drum % gives 50–75 % of the critical speed 42.3/√D.
* Probe `tau_bean_s`: 8–10 s at home, 14–16 s for shop drums.

**Stir pan:** scale `pan_C` with pan area (650 J/K for 0.24 m), keep `h_contact` 150–190, and set `bt_probe_full_g` at about 0.75 × the default batch.
