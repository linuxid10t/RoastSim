# Physics model

The aim is "behaves like the real machine for the reasons the real machine does". It is not CFD, but every effect a roaster notices comes from a physical mechanism rather than a scripted curve.

## Beans (`core/src/bean.cpp`)

A batch is **N parcels** (default 200). Each parcel stands for a group of similar beans and has:

* **Size and density:** bean size comes from mass alone (at a reference density of 700). The coffee's free-settled `density` (g/L) then acts on the bean as a material: harder, denser beans conduct to the core more slowly (`G × (700/ρ)³`) and release water a little more slowly (`× 700/ρ`). Dense coffee cracks later and hotter with a bigger core lag; it is never a "smaller, faster" bean.
* **Two thermal nodes:** surface (35 % of mass) and core, coupled by `G = 7·k·d`. Fast heating makes a surface–core gradient, which drives tipping and the gap between whole-bean and ground Agtron.
* **Heat capacity** of dry matter `cp = 1100 + 3.5·T J/kg·K`, plus each node's own water mass.
* **Drying** tracks water in kg in the core and surface. The nodes exchange water toward equal dry-basis moisture at a temperature-dependent rate. Surface evaporation uses the Schwartzberg-style term `A (X_surface−Xeq)² / d² · exp(−B/T_surface)` scaled by the positive vapor-pressure difference between the bean surface and ambient air. Evaporation takes latent heat from the surface. Water moving between the nodes carries sensible heat. The model uses an effective water activity and the inlet humidity. It omits condensation and vapor accumulation in the bed.
* **Exothermic pyrolysis:** `dξ/dt = k0 e^(−Ea/RT) (1−ξ)`, `H = 60 kJ/kg`. Small below 190 °C, significant in development, and runaway territory past second crack.
* **Development integrals** (seconds-equivalent at 200 °C, Arrhenius `Ea/R = 10 000 K`) for the surface and the core. Agtron = `18 + 105·e^(−D/196)`. Whole-bean colour uses the surface integral; ground uses the mass-weighted mean.
* **Yellowing integral** at 150 °C drives green → straw colour before browning.
* **Cracks:** each parcel has its own first- and second-crack threshold (bean variety ± spread, shifted by its moisture and size). The fraction of beans past the threshold (σ = 1.6 °C inside a parcel) gives Poisson-distributed pops, which feed the sounds and the 3D view. Each crack also takes a small endothermic flash (moisture/CO₂ burst), part of the RoR "crash".
* **Mass:** water loss plus organic loss `10 %·(1 − e^(−D/400))`. Expansion grows with development and cracks. Oil appears with second crack or deep development. Chaff detaches with drying and first crack.
* **Defects:** *scorch* is a local burn. It accumulates while a parcel's hot spot (`spot_T`) is above 240 °C (`scorch_T`) and at least 10 °C (`scorch_dT`) hotter than the rest of the bean's surface, at a rate set by how far it is past both. A bean counts as scorched once its index passes 0.35. The hot spot comes from contact with a hot drum wall or pan floor, weighted by how long the bean rests there, or, in a fluid bed or popper, from a stalled bed resting in the jet of inlet air off the heater. A bed that keeps moving has no hot spot, so an even, very dark roast is dark but not scorched. *Tipping* accumulates while the surface–core gradient exceeds 38 °C and the beans are still wet.

## Mixing

Each parcel carries two Ornstein–Uhlenbeck processes (convective exposure, contact exposure) with unit variance and correlation time `τ_mix` from the machine. Good mixing (short τ) averages out, so beans stay even. Poor mixing (stalled fluid bed, stopped stir arm, very slow drum) keeps the same beans near the heat, which shows up as colour spread, scorching, and an unhelpful BT probe. A static per-parcel bias (position or shape) scaled by the roaster's `unevenness` models persistent differences such as popper hot zones.

## Machines (`core/src/machine_*.cpp`)

**Fluid bed (air roaster, popper):** the heater element has thermal mass (about a 10 s lag), heats the inlet air (effectiveness ε), and the air crosses the bean bed with NTU = ΣhA/ṁcp. Bypass is folded into an effective h that grows with flow^0.6 by default. Downstream of the bed, the exhaust and chamber wall exchange equal and opposite heat via an NTU calculation. The wall also loses heat to ambient. Agitation is airflow divided by the airflow needed to lift the batch. That requirement falls as beans lose mass and expand, so a fan setting that barely lifts green beans can blow roasted ones out (`beans_ejecting`). Below about 0.7 the bed stalls. Air mass flow scales with air density (altitude, intake temperature). *Stock wiring:* heater and motor share the variac output; the line voltage sags through the source impedance; a bimetal thermostat on the inlet air cycles the heater; a one-shot thermal fuse can blow. *Independent wiring:* SSR duty with optional time-proportioning (ET ripple) and a fan that stalls below a minimum %.

**Drum:** a heat source (gas burner with minimum flame and supply-pressure dependence, a resistive element following V², induction, or IR with a direct-radiation share) with a time constant. Energy splits into drum shell / drawn air / beans. Single- or double-wall drum nodes, with losses to ambient. Drum air is solved quasi-steadily from inlet enthalpy, wall exchange and bean convection. Beans get heat by wall contact (wetted fraction rises with fill), by convection, and by radiation from the drum interior onto the bed top. Drum speed against the critical speed `42.3/√D` sets mixing; above it the beans centrifuge. How deep the BT probe sits depends on batch size, so small batches read toward air temperature, as on real machines.

**Stir pan:** the pan is a node heated by an element under a thermostat. Beans conduct from the pan through their footprint, and the share of beans touching the pan follows monolayer coverage. The stir-arm rpm sets how often each bean gets its turn. Dome air is quasi-steady, with vent and fan exchange. Chaff stays in the pan and smoulders above 260 °C (smoke).

**Cooling tray:** after DROP, beans move to a tray with forced (or natural) convection. Development coasts while they cool, as it does in reality.

## Probes (`core/src/sensors.cpp`)

First-order lag, with τ blending from `tau_air_s` to `tau_bean_s` by immersion. The bean-mass probe reads `immersion·T_bean_surface + (1−immersion)·T_air`. Fluid beds therefore read high at first crack, and the turning point and its timing come out of probe physics rather than a script. Gaussian noise, quantisation (0.25 °C, like a MAX31855) and offset are applied. Faults: open (−1), short, spikes, drift, stuck, noisy.

## Calibration

The defaults were tuned with `roastsim_script` so that typical profiles land in realistic envelopes. These are checked by `tests/test_main.cpp`:

| Roaster | Typical FC | Notes |
|---|---|---|
| Popper (modded, full heat) | 3:40–4:30 | fast, high early RoR, BT probe reads 10–20 °C above true bean temperature |
| Air roaster 150 g | ~5 min | fan sets temperature; lowering the fan makes it hotter |
| Stir pan 200 g | ~4:45 | colour spread is larger; stop the arm and it scorches |
| IR drum 225 g | ~5:45 | radiant, responsive |
| Home drum 500 g (preheat 190) | ~7 min | |
| Shop drum 5 kg (preheat 210) | ~10:30 | heavy drum momentum, TP ≈ 0:35 |

Final weight loss is 10–16 %, moisture 1.5–3.5 % for medium roasts, and Agtron falls with longer development. Every constant is in JSON: machine parameters in `data/roasters/*.json`, and bean kinetics under `"kinetics"` in `data/beans/*.json` (see `Kinetics` in `bean.hpp`). To fit your own machine, log a real roast, script the same control moves (see `scripts/`), and adjust.

### Pilot spouted-bed preset

The `spouted_bed_pilot` roaster and `kenya_pilot_blend` bean reproduce experiments on a 500 g pilot roaster. The fit uses two 350 g batches at an inlet setpoint of 250 °C. A 500 g batch at higher airflow is reserved for validation. The fixture contains selected product thermocouple and steady inlet readings digitized from Figures 4(a) and 13 of the 2025 paper, together with tabulated mass loss and wet-basis moisture from the 2024 supplement, Table 10. [Temperature plots and experiment setup](https://doi.org/10.3390/beverages11060162), [Mass and moisture supplement](https://ars.els-cdn.com/content/image/1-s2.0-S0260877424001626-mmc1.docx)

The fixture compares the product thermocouple with simulated BT and replays cooling for 60 seconds before comparing mass loss and moisture. The tests set the air supply to the reported steady inlet temperature. They also exercise a separate simulated mid-roast fan change at fixed heater power. The published trials used constant airflow, so the fan-step check is a simulation regression.

| Trial | Batch | Airflow (kg/s) | BT RMSE (°C) | Mass-loss RMSE (percentage points) | Moisture RMSE (percentage points) |
|---|---:|---:|---:|---:|---:|
| Fit | 350 g | 0.0185 | 6.03 | 0.80 | 0.24 |
| Fit | 350 g | 0.0228 | 8.50 | 0.79 | 0.33 |
| Held out | 500 g | 0.0310 | 13.08 | 1.16 | 0.71 |

Fitting minimized squared errors in BT, mass loss and moisture on the 350 g profiles. Green bean mass, initial moisture and equivalent diameter come from the study. The 230 V heater and chamber constants are effective simulator values. The inlet plateau in the 350 g, 0.0228 kg/s profile has a 1.56 °C RMSE. The largest discrepancy on the 500 g profile is the first few seconds after charge. These measurements constrain bulk drying and temperature. A more detailed first-crack model would need independent crack timing or acoustic observations.

Run `roastsim_tests data spouted_bed` to check the fitted profiles, the held-out roast and the simulated fan step. The `spouted_bed_pilot_350g.json` scenario runs the 350 g profile at 0.0228 kg/s. Loading the preset sets the wall voltage to 230 V. To replay the profile in either app, set the room temperature to 20 °C and source resistance to 0 Ω.

The preset's `reference_roast` entry sets the bean and heater percentage for `roastsim_check`. Scenarios use the roaster's rated voltage by default and accept an explicit `env.line_voltage` override.
