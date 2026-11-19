# Calibrating a roaster preset

Change one knob at a time, re-run `roastsim_check --roaster <id>` (or your scenario script), and watch the column you meant to move. Reference runs take well under a second.

## Symptom → knob

| Symptom in the reference roast | Fluid bed | Drum | Stir pan |
|---|---|---|---|
| FC too late, RoR flattens far below FC ("stall") | raise `h_ref`; lower `fan_max_flow` (hotter inlet); raise `heater_W` if it is really underrated | lower `loss_UA`; raise `efficiency` or `heat_max_W`; raise `h_contact` / `h_air_ref`; check the drum isn't too massive | raise `h_contact`; lower `pan_loss_UA`; check the thermostat isn't cutting out (alarm `thermostat_open`) |
| FC too early / everything too fast | the opposite of the above | the opposite; or a heavier drum (`drum_mass_kg`) for more momentum | lower `h_contact` or `heater_W` |
| ET far above BT the whole roast | fluid beds really do this (inlet ET); for an exhaust ET, lower `h_ref` | lower `frac_air`, or raise `airflow_max` (cooler inlet) | lower `dome_vent_UA` |
| ET **below** BT in a drum | n/a | raise `wall_air_UA`; lower `airflow_max`; raise `frac_air` | n/a |
| Turning point too early or too low | raise BT `tau_bean_s` | raise BT `tau_bean_s` (shop 14–16) | raise BT `tau_bean_s` |
| BT probe reads far from the true bean temperature (the `bean` column of `roastsim_script`) | that is fluid-bed physics (10–20 °C high is realistic); reduce only if the user's machine doesn't | small batch: raise or lower `bt_probe_full_kg` | `bt_probe_full_g` |
| Bed stalls (`bed_stalled`) at the default fan | raise `fan_max_flow` or lower `fluidize_flow_per_kg` | drum too slow: raise `rpm_max` | stir too slow: raise `stir_rpm_max` |
| Beans ejected | lower `fan_max_flow` or raise `fluidize_flow_per_kg` / `eject_agitation` | n/a | n/a |
| Centrifuging | n/a | lower `rpm_max` (it is above the critical speed 42.3/√D) | n/a |
| Scorching at sensible settings | n/a | lower `h_contact` or `frac_drum`; check the preheat isn't absurd | lower `h_contact`; raise `stir_rpm_max` |
| Tipping | lower `h_ref` early on, or the user's profile is just aggressive | lower `frac_beans_rad` (IR) | rare |
| Colour spread (`agtron_sd`) too high | lower `unevenness` | lower `unevenness`; check drum speed | lower `unevenness`; raise `stir_rpm_max` |
| Preheat takes forever (`--preheat`) | n/a | `heat_max_W` too small for `drum_mass_kg` | `pan_C` too big |
| Fuse blows or the thermostat trips at normal settings | raise `fuse_C` / `thermostat_open_C`, or raise `fan_max_flow` | n/a | raise `thermostat_open_C` |

## Energy check for a stall

When a roast stalls (RoR falls toward 0 well before FC), do this arithmetic before turning knobs. `roastsim_script` prints `inlet`, `wall` and `power` (heat-source W) columns, and `bean` is the true bean temperature.

```
heat in      ≈ power × efficiency                                    (drum)   or  power (fluid bed / pan)
exhaust loss ≈ airflow_kg_s × 1006 × (ET_exhaust − ambient)
shell loss   ≈ loss_UA × (wall − ambient)
left for beans = heat in − exhaust loss − shell loss
needed       ≈ batch_kg × (1800 × RoR/60  +  2.5e6 × drying rate)    → mid-roast roughly 250–450 W per kg
```

If "left for beans" is near zero, the preset can't roast this batch. Lower the losses (`airflow_max`, `loss_UA`) or raise `efficiency`/power within the reference ranges, rather than inflating heat-transfer coefficients, which only move heat around and don't create it.

**Expected, not a bug:** in drums, RoR typically sags to about 4–6 °C/min in the middle of the roast (drying absorbs heat while the drum's stored heat is used up), then recovers slightly toward first crack as the exotherm starts. The calibrated drum presets all show this. Treat it as a problem only if RoR reaches ~0 or the roast never cracks.

## Matching a real roast log

1. From the user's log or Artisan `.alog`, note batch, charge temperature, control changes (time → setting), TP, DRY, FCs and DROP times and temperatures.
2. Write `scripts/<roaster-id>_<lot>.json` with the same `batch_g`, `preheat_C` (the ET at charge) and `actions`. See `core/include/roastsim/scenario.hpp` for triggers: `at`, `roast_at`, `bt_ge`, `event` + `delay`.
3. Run `build/roastsim_script scripts/<file>.json --data data --every 30`, and compare TP, FCs, drop BT and the RoR shape.
4. Fix timing first (power and losses), then the curve shape (probe τ, heat-transfer split), then the extras (spread, alarms).

Close is good enough: real roasters vary by ±30 s from batch to batch. Don't over-fit one log by pushing parameters outside the ranges in `roaster-fields.md`. If it takes that, the machine may belong to a different family or the log may be unusual.

## Sanity checks before finishing

```
build/roastsim_check --data data --roaster <id>                 # defaults
build/roastsim_check --data data --roaster <id> --batch <min_g>
build/roastsim_check --data data --roaster <id> --batch <max_g>
build/roastsim_check --data data --all                          # nothing else broke
```

The max batch may legitimately be slower, and the min batch may read odd because of probe immersion, as on real machines. Mention these in the report instead of tuning them away.
