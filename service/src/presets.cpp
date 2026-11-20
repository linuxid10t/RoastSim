#include "roastsim/host.hpp"

#include <filesystem>

#include "roastsim/platform.hpp"
#include "sha1.hpp"

namespace roastsim {
namespace {

namespace fs = std::filesystem;
using json::Value;

Value error(const std::string& message) {
    Value r = Value::object();
    r["ok"] = false;
    r["error"] = message;
    return r;
}

struct RoasterType {
    const char* id;
    const char* name;
    const char* description;
    const char* definition;
};

// Starter definitions stand on their own. Unspecified physics use the model defaults.
constexpr RoasterType kRoasterTypes[] = {
    {"fluid_bed", "Fluid bed", "Roasts in moving hot air with separate heater and fan controls.", R"({
        "kind":"fluid_bed", "batch":{"min_g":80,"default_g":150,"max_g":225},
        "controls":[{"id":"heat","label":"Heater"},{"id":"fan","label":"Fan","default":90}],
        "fluid_bed":{"fluidize_flow_per_kg":0.027,"chamber_diameter_m":0.10,"chamber_height_m":0.20}
    })"},
    {"electric_drum", "Electric drum", "Uses an electric heater and a rotating drum.", R"({
        "kind":"drum", "batch":{"min_g":250,"default_g":500,"max_g":1000},
        "controls":[{"id":"heat","label":"Heater"},{"id":"fan","label":"Fan","default":30},
            {"id":"drum","label":"Drum speed","default":60}],
        "drum":{"heat_source":"electric","heat_max_W":2000,"nominal_V":120,"efficiency":0.85,
            "frac_drum":0.80,"frac_air":0.20,"drum_mass_kg":5,"drum_diameter_m":0.23,
            "drum_length_m":0.28,"double_wall":false,"loss_UA":1.5,"airflow_max":0.008,
            "airflow_min_frac":0.15,"rpm_max":65,"bt_probe_full_kg":0.50}
    })"},
    {"gas_drum", "Gas drum", "Uses a gas burner and a rotating drum.", R"({
        "kind":"drum", "batch":{"min_g":1000,"default_g":3000,"max_g":5000},
        "controls":[{"id":"heat","label":"Gas","slew_per_s":20},{"id":"fan","label":"Fan","default":40},
            {"id":"drum","label":"Drum speed","default":70}]
    })"},
    {"stir_pan", "Stir pan", "Roasts on a heated pan with a motorized stir arm.", R"({
        "kind":"stir_pan", "batch":{"min_g":100,"default_g":200,"max_g":300},
        "controls":[{"id":"heat","label":"Heater"},{"id":"stir","label":"Stir speed","default":80},
            {"id":"fan","label":"Dome fan"}]
    })"},
};

Value roasterTypes() {
    Value result = Value::object();
    result["ok"] = true;
    Value types = Value::array();
    for (const auto& type : kRoasterTypes) {
        Value entry = Value::object();
        entry["id"] = type.id;
        entry["name"] = type.name;
        entry["description"] = type.description;
        types.push(std::move(entry));
    }
    result["types"] = std::move(types);
    return result;
}

bool startRoaster(const std::string& id, RoasterDef& draft, std::string* why) {
    for (const auto& type : kRoasterTypes) {
        if (type.id != id) continue;
        Value definition = Value::parse(type.definition);
        definition["id"] = type.id;
        definition["name"] = type.name;
        definition["probes"] = Value::parse(R"([{"id":"BT","location":"bean_mass"},{"id":"ET","location":"exhaust_air"}])");
        return RoasterDef::fromJson(definition, draft, why);
    }
    if (why) *why = "Choose a basic roaster type.";
    return false;
}

class Form {
public:
    void text(const char* key, const char* label, const std::string& value, int limit) {
        Value field = entry(key, label, value);
        field["limit"] = limit;
        fields_.push(std::move(field));
    }

    void number(const char* key, const char* label, double value, double min, double max, bool whole = false) {
        Value field = entry(key, label, value);
        field["min"] = min;
        field["max"] = max;
        if (whole) field["whole"] = true;
        fields_.push(std::move(field));
    }

    void section(const char* title) {
        Value section = Value::object();
        section["title"] = title;
        section["fields"] = std::move(fields_);
        sections.push(std::move(section));
        fields_ = Value::array();
    }

    Value sections = Value::array();

private:
    static Value entry(const char* key, const char* label, Value value) {
        Value field = Value::object();
        field["key"] = key;
        field["label"] = label;
        field["value"] = std::move(value);
        return field;
    }

    Value fields_ = Value::array();
};

Value beanForm(const BeanSpec& b) {
    Form f;
    f.text("name", "Name", b.name, 160);
    f.text("origin", "Origin", b.origin, 160);
    f.text("process", "Process", b.process, 100);
    f.number("moisture_pct", "Green moisture (%)", std::round(1e8 * b.moisture_wb) / 1e6, 4, 20);
    f.number("density", "Bulk density (g/L)", b.density, 100, 1500);
    f.number("bean_mass_mg", "Average bean mass (mg)", b.bean_mass_mg, 10, 1000);
    f.text("notes", "Notes", b.notes, 500);
    f.section("Bean details");
    f.number("fc_temp", "First crack core temperature (°C)", b.fc_temp, 120, 295);
    f.number("sc_temp", "Second crack core temperature (°C)", b.sc_temp, 125, 300);
    f.number("fc_sd", "First crack temperature spread (°C)", b.fc_sd, 0, 20);
    f.number("sc_sd", "Second crack temperature spread (°C)", b.sc_sd, 0, 20);
    f.number("size_rel_sd", "Relative bean size spread (0–1)", b.size_rel_sd, 0, 0.5);
    f.number("kinetics_scale", "Browning rate multiplier", b.kinetics_scale, 0.1, 5);
    f.number("crack_loudness", "Crack volume multiplier", b.crack_loudness, 0, 4);
    f.section("Roast behavior");
    return f.sections;
}

Value roasterForm(const RoasterDef& r) {
    Form f;
    f.text("name", "Name", r.name, 160);
    f.text("description", "Description", r.description, 500);
    f.number("batch.min_g", "Minimum batch (g)", r.batch.min_g, 1, 100000);
    f.number("batch.default_g", "Default batch (g)", r.batch.default_g, 1, 100000);
    f.number("batch.max_g", "Maximum batch (g)", r.batch.max_g, 1, 100000);
    switch (r.kind) {
        case RoasterKind::FluidBed:
            f.number("fluid_bed.nominal_V", "Rated wall voltage (V)", r.nominalLineVoltage(), 50, 260);
            f.number("fluid_bed.heater_W", "Heater power (W)", r.fluid.heater_W, 1, 1000000);
            f.section("Capacity and power");
            f.number("fluid_bed.fan_max_flow", "Maximum airflow (kg/s)", r.fluid.fan_max_flow, 0.00001, 10);
            f.number("fluid_bed.fan_tau", "Fan response time (s)", r.fluid.fan_tau, 0.05, 120);
            f.number("fluid_bed.chamber_diameter_m", "Chamber diameter (m)", r.fluid.chamber_diameter_m, 0.01, 10);
            f.number("fluid_bed.chamber_height_m", "Chamber height (m)", r.fluid.chamber_height_m, 0.01, 10);
            f.number("fluid_bed.element_C", "Element heat capacity (J/K)", r.fluid.element_C, 1, 100000);
            f.number("fluid_bed.h_ref", "Full-fan heat transfer (W/m²K)", r.fluid.h_ref, 1, 1000);
            f.number("fluid_bed.fluidize_flow_per_kg", "Lift airflow (kg/s per kg)", r.fluid.fluidize_flow_per_kg, 0.00001, 10);
            break;
        case RoasterKind::Drum:
            f.number("drum.nominal_V", "Rated wall voltage (V)", r.nominalLineVoltage(), 50, 260);
            f.number("drum.heat_max_W", r.drum.heat_source == "gas" ? "Burner power (W)" : "Heater power (W)",
                     r.drum.heat_max_W, 1, 1000000);
            f.section("Capacity and power");
            f.number("drum.airflow_max", "Maximum airflow (kg/s)", r.drum.airflow_max, 0.00001, 10);
            f.number("drum.drum_diameter_m", "Drum diameter (m)", r.drum.drum_diameter_m, 0.01, 10);
            f.number("drum.drum_length_m", "Drum length (m)", r.drum.drum_length_m, 0.01, 10);
            f.number("drum.drum_mass_kg", "Drum mass (kg)", r.drum.drum_mass_kg, 0.1, 10000);
            f.number("drum.rpm_max", "Maximum drum speed (RPM)", r.drum.rpm_max, 1, 250);
            f.number("drum.h_contact", "Contact heat transfer (W/m²K)", r.drum.h_contact, 0, 10000);
            break;
        case RoasterKind::StirPan:
            f.number("stir_pan.nominal_V", "Rated wall voltage (V)", r.nominalLineVoltage(), 50, 260);
            f.number("stir_pan.heater_W", "Heater power (W)", r.pan.heater_W, 1, 1000000);
            f.section("Capacity and power");
            f.number("stir_pan.fan_max_flow", "Maximum airflow (kg/s)", r.pan.fan_max_flow, 0, 10);
            f.number("stir_pan.pan_diameter_m", "Pan diameter (m)", r.pan.pan_diameter_m, 0.01, 10);
            f.number("stir_pan.pan_C", "Pan heat capacity (J/K)", r.pan.pan_C, 10, 1000000);
            f.number("stir_pan.stir_rpm_max", "Maximum stir speed (RPM)", r.pan.stir_rpm_max, 1, 250);
            f.number("stir_pan.h_contact", "Contact heat transfer (W/m²K)", r.pan.h_contact, 0, 10000);
            break;
    }
    f.section("Machine tuning");
    const auto* heat = r.control(Control::Heat);
    const auto* fan = r.control(Control::Fan);
    const bool heatLevels = heat && heat->supportsLevels(), fanLevels = fan && fan->supportsLevels();
    if (heatLevels)
        f.number("controls.heat.levels", "Heat levels (0 = percent)", heat->levels, 0, kMaxControlLevels, true);
    if (fanLevels)
        f.number("controls.fan.levels", "Fan levels (0 = percent)", fan->levels, 0, kMaxControlLevels, true);
    if (heatLevels || fanLevels) f.section("Control levels");
    const auto& bt = r.probes[static_cast<std::size_t>(r.probeIndex("BT"))];
    const auto& et = r.probes[static_cast<std::size_t>(r.probeIndex("ET"))];
    f.number("probes.BT.tau_air_s", "BT response in air (s)", bt.tau_air_s, 0.05, 120);
    f.number("probes.BT.tau_bean_s", "BT response in beans (s)", bt.tau_bean_s, 0.05, 120);
    f.number("probes.BT.offset", "BT offset (°C)", bt.offset, -50, 50);
    f.number("probes.ET.tau_air_s", "ET response in air (s)", et.tau_air_s, 0.05, 120);
    f.number("probes.ET.offset", "ET offset (°C)", et.offset, -50, 50);
    f.section("Probe response");
    return f.sections;
}

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

std::string presetId(const std::string& name) {
    std::string id = "custom_";
    for (unsigned char ch : name) {
        if (id.size() >= 60) break;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) id += static_cast<char>(ch);
        else if (id.back() != '_') id += '_';
    }
    if (id == "custom_") return "custom_preset";
    if (id.back() == '_') id.pop_back();
    return id;
}

bool validUserId(const std::string& id) {
    return id.size() > 7 && id.size() <= 80 && id.compare(0, 7, "custom_") == 0 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}

std::string revision(const Value& definition) { return detail::base64(detail::sha1(definition.dump())); }

void applyField(Value& definition, const std::string& key, const Value& value) {
    const auto dot = key.find('.');
    const auto group = key.substr(0, dot);
    if (group == "probes" || group == "controls") {
        const auto property = key.find('.', dot + 1);
        const auto id = key.substr(dot + 1, property - dot - 1);
        Value entries = Value::array();
        for (auto entry : definition[group].items()) {
            std::string entryId = entry.get("id", "");
            Control control;
            if (group == "controls" && controlFromName(entryId, control)) entryId = controlName(control);
            if (entryId == id) entry[key.substr(property + 1)] = value;
            entries.push(std::move(entry));
        }
        definition[group] = std::move(entries);
    } else if (dot != std::string::npos) definition[group][key.substr(dot + 1)] = value;
    else definition[key] = value;
    if (key == "moisture_pct") definition.erase("moisture_wb");
}

}  // namespace

void Host::loadUserPresets() {
    userRoasters_.clear();
    userBeans_.clear();
    for (bool roaster : {true, false}) {
        auto& ids = roaster ? userRoasters_ : userBeans_;
        fs::path folder = fs::u8path(cfg_.presets_dir) / (roaster ? "roasters" : "beans");
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) continue;
        fs::directory_iterator entry(folder, ec), end;
        for (; !ec && entry != end; entry.increment(ec)) {
            const auto& path = entry->path();
            const std::string id = path.stem().u8string();
            if (path.extension() != ".json" || !validUserId(id) || !entry->is_regular_file(ec) ||
                (roaster ? catalog_.roaster(id) != nullptr : catalog_.bean(id) != nullptr)) continue;
            std::string text, why;
            bool loaded = readTextFile(path.u8string(), text) && Value::parse(text).get("id", "") == id &&
                          (roaster ? catalog_.addRoasterJson(text, &why) : catalog_.addBeanJson(text, &why));
            if (loaded) ids.insert(id);
            else messages_.push_back("warning: could not load preset " + path.filename().u8string());
        }
        if (ec) messages_.push_back("warning: could not list presets in " + folder.u8string());
    }
    ++catalogRevision_;
}

Value Host::presetCommand(const Value& cmd) {
    const std::string action = cmd.get("cmd", "");
    if (action == "preset_types") return roasterTypes();
    const std::string kind = cmd.get("kind", ""), id = cmd.get("id", ""), mode = cmd.get("mode", "");
    if (kind != "roaster" && kind != "bean") return error("Choose a bean or roaster preset.");
    if (mode != "create" && mode != "edit") return error("Choose Create or Edit.");
    const bool roaster = kind == "roaster";
    const bool chooseType = roaster && mode == "create";
    std::lock_guard<std::mutex> lock(mu_);
    auto& customIds = roaster ? userRoasters_ : userBeans_;
    const bool custom = !chooseType && customIds.count(id) != 0;
    RoasterDef starter;
    const RoasterDef* r = nullptr;
    if (chooseType) {
        std::string why;
        if (!startRoaster(cmd.get("roaster_type", ""), starter, &why)) return error(why);
        r = &starter;
    } else if (roaster) {
        if (!custom) return error("Use New roaster to create an editable preset.");
        r = catalog_.roaster(id);
    }
    const BeanSpec* b = roaster ? nullptr : catalog_.bean(id);
    if (!r && !b) return error("That preset is no longer available.");
    const Value& original = r ? r->raw : b->raw;
    const Value sections = r ? roasterForm(*r) : beanForm(*b);
    if (action == "preset_form") {
        Value result = Value::object();
        result["ok"] = true;
        result["custom"] = custom;
        result["revision"] = revision(original);
        result["sections"] = sections;
        return result;
    }

    const bool replace = mode == "edit" && custom;
    const Value& changes = cmd["changes"];
    if (!changes.isObject()) return error("Preset changes must be an object.");
    if (cmd.get("revision", "") != revision(original)) return error("This preset changed. Reopen it before saving.");
    Value definition = original;
    std::size_t applied = 0;
    for (const auto& section : sections.items()) {
        for (const auto& field : section["fields"].items()) {
            const auto key = field.get("key", "");
            if (!changes.has(key)) continue;
            const auto label = field.get("label", "");
            const auto& value = changes[key];
            if (field["value"].isString()) {
                if (!value.isString() || value.str().size() > field.get("limit", 0.0))
                    return error(label + " is too long or invalid.");
                if (value.str().find('\0') != std::string::npos) return error(label + " contains an invalid character.");
                applyField(definition, key, trim(value.str()));
            } else {
                if (!value.isNumber() || !std::isfinite(value.num()) || value.num() < field.get("min", 0.0) ||
                    value.num() > field.get("max", 0.0))
                    return error(label + " is outside its allowed range.");
                if (field.getBool("whole", false) && std::trunc(value.num()) != value.num())
                    return error(label + " must be a whole number.");
                applyField(definition, key, value);
            }
            ++applied;
        }
    }
    if (applied != changes.keys().size()) return error("This preset has a field that cannot be edited here.");
    const std::string name = definition.get("name", "");
    if (name.empty() || (mode == "create" && !changes.has("name"))) return error("Enter a preset name.");

    const fs::path folder = fs::u8path(cfg_.presets_dir) / (roaster ? "roasters" : "beans");
    std::string saveId = id;
    if (!replace) {
        const std::string base = presetId(name);
        saveId = base;
        unsigned suffix = 2;
        std::error_code ec;
        while ((roaster ? catalog_.roaster(saveId) != nullptr : catalog_.bean(saveId) != nullptr) ||
               fs::exists(folder / (saveId + ".json"), ec)) saveId = base + "_" + std::to_string(suffix++);
        if (ec) return error("Cannot check the preset folder.");
    }
    definition["id"] = saveId;
    RoasterDef newRoaster;
    BeanSpec newBean;
    std::string why;
    if (roaster) {
        if (!RoasterDef::fromJson(definition, newRoaster, &why)) return error(why);
        const auto& batch = newRoaster.batch;
        if (batch.min_g > batch.default_g || batch.default_g > batch.max_g)
            return error("The default batch must be between the minimum and maximum.");
    } else {
        newBean = BeanSpec::fromJson(definition);
        if (newBean.fc_temp >= newBean.sc_temp) return error("Second crack must reach a higher temperature than first crack.");
    }

    const auto path = (folder / (saveId + ".json")).u8string();
    std::string previous;
    if (replace && (!readTextFile(path, previous) || revision(Value::parse(previous)) != revision(original)))
        return error("The saved preset changed on disk. Restart RoastSim before editing it again.");
    if (!platform::writePreset(path, definition.dump(2) + "\n", replace ? &previous : nullptr, &why)) return error(why);
    if (roaster) catalog_.putRoaster(std::move(newRoaster));
    else {
        if (replace && sim_.bean().id == saveId) sim_.setBean(newBean);
        catalog_.putBean(std::move(newBean));
    }
    customIds.insert(saveId);
    ++catalogRevision_;
    Value result = Value::object();
    result["ok"] = true;
    result["id"] = saveId;
    result["name"] = name;
    return result;
}

}  // namespace roastsim
