# Physics model

The aim is "behaves like the real machine for the reasons the real machine does". It is not CFD, but every effect a roaster notices comes from a physical mechanism rather than a scripted curve.

## Beans (`core/src/bean.cpp`)

A batch is **N parcels** (default 200). Each parcel stands for a group of similar beans and has:

* **Two thermal nodes:** surface (35 % of mass) and core, coupled by `G = 7·k·d`. Fast heating makes a surface–core gradient, which drives tipping and the gap between whole-bean and ground Agtron.
* **Heat capacity** of dry matter `cp = 1100 + 3.5·T J/kg·K`, plus the water still present.
* **Drying** (Schwartzberg form): `dX/dt = −A (X−Xeq)² / d² · exp(−B/T)`, with latent heat 2.5 MJ/kg taken from both nodes. This is the endothermic "drying plateau".
* **Exothermic pyrolysis:** `dξ/dt = k0 e^(−Ea/RT) (1−ξ)`, `H = 60 kJ/kg`. Small below 190 °C, significant in development, and runaway territory past second crack.
* **Development integrals** (seconds-equivalent at 200 °C, Arrhenius `Ea/R = 10 000 K`) for the surface and the core. Agtron = `18 + 105·e^(−D/196)`. Whole-bean colour uses the surface integral; ground uses the mass-weighted mean.
* **Yellowing integral** at 150 °C drives green → straw colour before browning.
* **Cracks:** each parcel has its own first- and second-crack threshold (bean variety ± spread, shifted by its moisture and size). The fraction of beans past the threshold (σ = 1.6 °C inside a parcel) gives Poisson-distributed pops, which feed the sounds and the 3D view. Each crack also takes a small endothermic flash (moisture/CO₂ burst), part of the RoR "crash".
* **Mass:** water loss plus organic loss `10 %·(1 − e^(−D/400))`. Expansion grows with development and cracks. Oil appears with second crack or deep development. Chaff detaches with drying and first crack.
* **Defects:** *scorch* accumulates while the contact hot-spot temperature (`spot_T`) exceeds 240 °C. *Tipping* accumulates while the surface–core gradient exceeds 38 °C and the beans are still wet.

## Mixing

Each parcel carries two Ornstein–Uhlenbeck processes (convective exposure, contact exposure) with unit variance and correlation time `τ_mix` from the machine. Good mixing (short τ) averages out, so beans stay even. Poor mixing (stalled fluid bed, stopped stir arm, very slow drum) keeps the same beans near the heat, which shows up as colour spread, scorching, and an unhelpful BT probe. A static per-parcel bias (position or shape) scaled by the roaster's `unevenness` models persistent differences such as popper hot zones.

## Machines (`core/src/machine_*.cpp`)

**Fluid bed (air roaster, popper):** the heater element has thermal mass (about a 10 s lag), heats the inlet air (effectiveness ε), and the air crosses the bean bed with NTU = ΣhA/ṁcp. Bypass is folded into an effective h that grows with flow^0.6. The chamber wall is a node that loses heat to ambient. Agitation is airflow divided by the airflow needed to lift the batch. That requirement falls as beans lose mass and expand, so a fan setting that barely lifts green beans can blow roasted ones out (`beans_ejecting`). Below about 0.7 the bed stalls. Air mass flow scales with air density (altitude, intake temperature). *Stock wiring:* heater and motor share the variac output; the line voltage sags through the source impedance; a bimetal thermostat on the inlet air cycles the heater; a one-shot thermal fuse can blow. *Independent wiring:* SSR duty with optional time-proportioning (ET ripple) and a fan that stalls below a minimum %.

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
