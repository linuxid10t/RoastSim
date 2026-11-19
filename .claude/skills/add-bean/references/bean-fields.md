# Bean preset fields

Defaults are in `core/include/roastsim/bean.hpp` (`BeanSpec`, `Kinetics`). Any field you omit takes its default. Unknown keys are ignored by the simulator but flagged by `roastsim_check`.

## Identity

| Field | Notes |
|---|---|
| `id` | snake_case, equal to the file name without `.json` |
| `name` | what the UI shows: "Origin Region, process" |
| `origin`, `process` | free text (`washed`, `natural`, `honey`, `anaerobic natural`, `wet-hulled`, `decaf`, …) |
| `notes` | short; say which values were given and which estimated |

## Physical fields

| Field | Default | Typical range | What it does in the model |
|---|---|---|---|
| `moisture_pct` | 11.0 | 9.5–12.5 (wet-hulled 12–13.5, old crop 9–10) | Water to evaporate. More water means a longer drying phase, later DRY and FC, and more weight loss. |
| `moisture_sd` | 0.004 | 0.003–0.008 (fraction, not %) | Bean-to-bean moisture variation, which adds unevenness. Raise it for poorly dried or mixed lots. |
| `density` | 700 | 640–760 | **Use the spec sheet's free-settled density in g/L.** Relative to 700, denser beans conduct heat to the core more slowly (∝ (700/ρ)³) and dry a little more slowly (∝ 700/ρ). The result is a later first crack, a higher BT at crack, and a bigger whole-versus-ground colour gap; lower density gives the opposite. Bean size comes from `bean_mass_mg` only. It also sets the pile volume (drum fill, probe immersion) unless `bulk_density` is given. Altitude-driven crack temperature still goes in `fc_temp`. Density's own effect on FC time is modest (roughly ±10–30 s across 650–760 g/L on a popper). |
| `bean_mass_mg` | 150 | 110–200 | Mass of one green bean. Bigger beans heat more slowly (less area per gram), so FC comes later. |
| `size_rel_sd` | 0.10 | 0.07 (graded AA/screen 18) – 0.16 (ungraded heirloom) | Relative spread of bean size, which shows as colour spread (`agtron_sd`). |
| `fc_temp` | 196 | 193–199 | Bean *core* temperature at which a bean cracks. The BT probe usually reads a few degrees off this (machine dependent). |
| `fc_sd` | 1.8 | 1.5–2.5 | Spread of crack temperatures between bean groups: how drawn-out first crack sounds. |
| `sc_temp` | 224 | `fc_temp` + 26–30 | Second-crack core temperature. |
| `sc_sd` | 2.2 | 2–3 | |
| `chaff_frac` | 0.010 | washed 0.008–0.010 · natural 0.011–0.014 · honey 0.010–0.012 · wet-hulled 0.005–0.007 · decaf 0.003–0.005 | Silverskin mass fraction: chaff output, and smoke in stir pans. |
| `kinetics_scale` | 1.0 | washed 1.0 · honey 1.02–1.05 · natural 1.04–1.08 · anaerobic 1.06–1.10 · decaf 1.12–1.18 | Multiplies browning/development speed. Above 1 means darker colour sooner and more weight loss for the same profile. |
| `bulk_density` | = `density` | leave out | Pile volume (drum fill, probe immersion). Defaults to `density`; set it only if you have a separate packed-pile figure. |
| `green_rgb` | [0.46, 0.52, 0.34] | see below | Green colour (0–1 sRGB) for the bean views and the future 3D renderer. |

### Green colour guide

| Coffee | `green_rgb` |
|---|---|
| fresh washed, high grown (blue-green) | [0.42, 0.52, 0.36] |
| typical washed | [0.46, 0.52, 0.34] |
| natural / honey (yellower) | [0.50, 0.52, 0.37] |
| wet-hulled (dark blue-green) | [0.36, 0.48, 0.40] |
| past crop / aged (pale, yellow) | [0.56, 0.54, 0.40] |
| decaf (already brownish) | [0.55, 0.47, 0.32] |

## Deriving values from a spec sheet

**Screen size → `bean_mass_mg`** (flat beans, arabica):

| Screen | 14 | 15 | 16 | 17 | 18 | 19+ | peaberry |
|---|---|---|---|---|---|---|---|
| mg | 115 | 130 | 145 | 160 | 175 | 190 | 140 (and `size_rel_sd` ≤ 0.09) |

For a range such as "16/17" or "15+", take the midpoint of the listed screens (16/17 → 152 mg; "15+" → treat as 15–17 → 145 mg). Kenyan grades: AA ≈ screen 18, AB ≈ 15–16. Central American SHB/SHG and "EP" lots are usually 16–17+. Robusta beans are smaller and rounder (≈ 120 mg).

**Altitude → density and `fc_temp`** when density is not given:

| Altitude (m) | density g/L | `fc_temp` |
|---|---|---|
| < 900 (low grown, e.g. much of Brazil) | 650–680 | 194 |
| 900–1,400 | 680–710 | 195–196 |
| 1,400–1,800 | 710–740 | 196–197 |
| > 1,800 | 740–770 | 197–198 |

Then adjust `fc_temp`: about +1 °C for moisture above 11.5 %, about −1 °C for naturals, −2 to −3 °C for decaf.

**Water activity** (aw) is not a field. High aw (> 0.60) means the coffee is "fresher/wetter": nudge `moisture_pct` up a little if moisture wasn't given. Low aw (< 0.50) suggests older or drier coffee.

**Robusta / canephora:** `moisture_pct` ~11–12, `bean_mass_mg` ~120, `chaff_frac` ~0.012, `kinetics_scale` 0.95 (less sugar, slower browning), `fc_temp` 198–199, `green_rgb` [0.50, 0.50, 0.38]. Note in `notes` that the model is tuned on arabica.

## `kinetics` overrides (rarely needed)

`"kinetics": { ... }` can override any `Kinetics` constant (drying, exotherm, Agtron mapping, crack energy, defect thresholds). They are calibrated **globally**; changing them for one bean makes it incomparable with the rest. Use them only when the user asks for a deliberately unusual behaviour, and explain it in `notes`. Examples:

* `"tipping_dT": 32`: a soft, brittle lot that tips easily.
* `"scorch_T": 230`: a delicate decaf that scorches on hot drum walls.

Prefer `kinetics_scale`, `moisture_pct`, `fc_temp` and size fields, which cover nearly every real difference between coffees.
