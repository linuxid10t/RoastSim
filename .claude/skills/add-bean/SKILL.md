---
name: add-bean
description: Add a new green coffee (bean) preset to the RoastSim simulator, as a JSON file in data/beans/, from a coffee description, importer spec sheet, or rough notes, then validate it with reference roasts. Use this whenever someone wants to add, create, model, tweak or "make a preset for" a green coffee, origin, lot, process (washed, natural, honey, anaerobic, wet-hulled, decaf) or variety in RoastSim. This includes "can the simulator roast a Kenya AA", "add the Ethiopian I just bought", or pasting a spec sheet with moisture, density and screen size.
---

# Add a green coffee preset to RoastSim

A bean preset is one JSON file in `data/beans/` that tells the bean-parcel model how this coffee behaves: how much water it has to lose, how big and dense the beans are, when they crack, how fast they brown, and how uneven the lot is. No C++ changes are needed. The catalog loads every `*.json` in that folder at startup.

The goal is a preset that a roaster would recognise: a dense, wet, high-grown Kenya should crack later than the reference Colombia, a natural should brown a bit faster, and a decaf should run noticeably fast. Getting the *direction and size of those differences* right matters more than any single number.

## Workflow

### 1. Collect what is known

Pull these from the user's message, spec sheet or notes, and keep track of which values were **given** and which you **estimate**:

origin and region · process · variety · altitude · screen size or grade · moisture % · density (g/L) · water activity · crop year / age · anything they've noticed when roasting it (e.g. "cracks late", "lots of chaff", "runs away after first crack").

Estimating missing values is fine and expected; that is what `references/bean-fields.md` is for. Ask the user only when something important is ambiguous and no reasonable default exists, such as a lot that is "half natural, half washed". Don't stall a simple request with questions.

### 2. Choose an id and check it is free

Use lowercase snake_case, `<origin>_<region-or-lot>_<process>`, e.g. `guatemala_huehuetenango_washed`. The file name must be `<id>.json`. List `data/beans/` first. A duplicate id silently replaces the other preset, which is never what anyone wants.

### 3. Write the file

Read `references/bean-fields.md` for every field, its meaning, typical ranges, and how to derive it from spec-sheet data. Start from the closest existing preset in `data/beans/` (a washed coffee from a similar altitude is usually the best template). Then change what this coffee does differently.

Put your assumptions into `notes`, briefly. A later reader, human or agent, should be able to tell given values from guesses:

```json
{ "id": "rwanda_nyamasheke_honey", "name": "Rwanda Nyamasheke, honey",
  "origin": "Rwanda", "process": "honey",
  "notes": "Red Bourbon, ~1,600 m. Given: moisture 11.2 %, screen 15. Estimated: density (altitude), fc_temp, chaff, kinetics_scale (honey).",
  "moisture_pct": 11.2, "density": 725, "bean_mass_mg": 130, "size_rel_sd": 0.11,
  "fc_temp": 196, "sc_temp": 224, "chaff_frac": 0.011, "kinetics_scale": 1.03,
  "green_rgb": [0.48, 0.52, 0.36] }
```

The example only shows the shape of a file. Take every value from the user's coffee and the reference tables, not from here.

Leave out `kinetics` unless there is a specific reason (see the reference). The defaults are calibrated, and overriding them casually breaks the comparison with every other preset.

### 4. Validate with reference roasts

Build the checker if it isn't built yet, then run it on the new bean. See "Running the tools" below for the build command.

```
./build/roastsim_check.exe --data data --bean <id>      # Windows (Git Bash); on Linux/Haiku drop ".exe"
```

This first validates every preset file (JSON syntax, unknown keys, duplicate ids, out-of-range values). It then roasts the new bean next to the reference `colombia_washed` on three different machines (popper, home drum, stir pan) with identical settings, dropping at 20 % development time ratio.

Read the comparison rather than the absolute numbers:

| If the coffee is… | expect vs. colombia_washed | driven by |
|---|---|---|
| wetter (higher `moisture_pct`) | later DRY and FCs, more weight loss | drying |
| bigger beans (`bean_mass_mg`) | later FCs | less surface per gram |
| high grown / dense | somewhat later FCs, higher FC BT (so 1–3 Agtron darker at a fixed 20 % DTR, because the drop is hotter), bigger whole-versus-ground Agtron gap. Lower moisture can cancel the later crack. | `density`, `fc_temp` |
| natural / honey / anaerobic | similar FCs, lower Agtron (darker) at drop, more weight loss | `kinetics_scale` |
| decaf | earlier FCs, clearly darker at the same time | `fc_temp`, `kinetics_scale` |
| small or ungraded | earlier FCs; larger `agtron_sd` | size fields |
| wet-hulled | longer drying (later DRY), higher spread | moisture, size spread |

Real lots mix these. A dense but dry coffee can land at the same FC time as the reference. Check that each effect is explained by a field you set, rather than expecting every row to move. Differences under about ±10 s in FC time and ±1 in `agtron_sd` are noise. If something moves the wrong way, or far too much (FC shifted by more than ~90 s on the popper), revisit the field you changed most. The checker also prints warnings ('!' lines). Treat validation **errors** as blocking; warnings need a judgement call and a sentence in your reply.

### 5. Report back

Tell the user, briefly:
- the file you created and its id
- a small table of the values, marking each *given* or *estimated*, with the reason for estimates
- what the reference roasts showed compared with the Colombia reference (FC time, colour, loss)

The preset appears in the simulator after a restart. Rebuilding copies `data/` next to the executables, and `roastsimd --data data` reads the source folder directly.

## Running the tools

On Windows the toolchain is MSYS2 UCRT64 and is not on PATH by default. In the Bash tool:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja      # only if build/ doesn't exist yet
cmake --build build --target roastsim_check
./build/roastsim_check.exe --data data --bean <id>
```

On Linux or Haiku, drop the PATH line and the `.exe`. Add `--json` for machine-readable output.

For a closer look at one roast (for example, the user asks "how does it behave in the 5 kg drum?"), use `--roaster <id> --bean <bean>`. For a full custom profile, write a scenario script like those in `scripts/` and run `build/roastsim_script <script.json> --data data`.

## Scope

- Only `data/beans/*.json` should change for a new bean. If the coffee genuinely can't be expressed with the fields (e.g. the user wants roast-level-dependent flavour notes), say so instead of editing core code.
- Don't change other presets to make the new one "fit". The reference bean anchors every comparison.
