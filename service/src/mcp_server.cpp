// MCP (Model Context Protocol) tools over the JSON command API. See
// docs/MCP.md for the tool list and how to connect an AI client.
#include "roastsim/mcp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "roastsim/host.hpp"

#ifndef RS_VERSION
#define RS_VERSION "0.0.0"
#endif

namespace roastsim {

namespace {

constexpr const char* kOrigin = "mcp";
// Newest first. A client asking for one of these gets it back; anything else
// gets the newest, and the client decides whether it can speak that.
const char* const kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr const char* kInstructions =
    "RoastSim is a coffee roasting simulator: a virtual roaster with realistic bean physics. "
    "Units: temperatures in degrees C, rate of rise (RoR) in C/min, weights in grams, controls in percent (0-100).\n"
    "A typical roast: list_presets, load_roaster (with a bean), set_power on, set_controls, preheat, charge, "
    "then watch with advance_time and get_state while adjusting heat and fan; after first crack (FCs) let the "
    "roast develop (usually 15-25% of the total time), drop, and read get_report.\n"
    "The simulation keeps running in real time between tool calls. For careful control, pause it "
    "(set_speed paused=true) and move the clock yourself with advance_time, which fast-forwards instantly. "
    "When Artisan is logging the roast, time is locked to real time and advance_time waits instead.\n"
    "The same roaster may also be open in the RoastSim app or web UI, and a person can change it there; "
    "read the state again before acting on old numbers.";

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

json::Value rnd(double v, int d = 1) {
    if (!std::isfinite(v)) return json::Value();
    double p = std::pow(10.0, d);
    return json::Value(std::round(v * p) / p);
}

json::Value obj() { return json::Value::object(); }

// ------------------------------------------------------------ tool outcomes

struct Outcome {
    json::Value data;           // object shown to the model (JSON)
    std::string error;          // non-empty: a tool error the model should read
    std::function<bool(json::Value& data)> poll;  // set: a real-time wait that fills `data` when done
};

Outcome okData(json::Value v) {
    Outcome o;
    o.data = std::move(v);
    return o;
}

Outcome toolError(std::string msg) {
    Outcome o;
    o.error = std::move(msg);
    return o;
}

// Runs a Host command; a failure becomes the tool error.
bool run(Host& host, json::Value cmd, json::Value& reply, std::string& err) {
    reply = host.command(cmd, kOrigin);
    if (reply.isObject() && reply.has("ok") && !reply.getBool("ok", false)) {
        err = reply.get("error", "command failed");
        return false;
    }
    return true;
}

json::Value command(const char* name) {
    json::Value c = obj();
    c["cmd"] = name;
    return c;
}

// ------------------------------------------------------------ state views

json::Value eventView(const SimEvent& e) {
    json::Value v = obj();
    v["type"] = e.type;
    if (!e.detail.empty()) v["detail"] = e.detail;
    if (e.t_roast >= 0 && e.type != "NOTE") v["t_roast_s"] = rnd(e.t_roast, 0);
    v["bt_C"] = rnd(e.bt);
    v["by"] = e.origin;
    return v;
}

// Events of the current roast: from the last CHARGE, or from the last cold
// reset / roaster load when nothing has been charged since.
std::vector<SimEvent> currentRoastEvents(Host& host) {
    auto all = host.eventsSince(0);
    std::size_t from = 0;
    for (std::size_t i = all.size(); i-- > 0;) {
        if (all[i].type == "CHARGE" || (all[i].type == "NOTE" && all[i].detail == "cold reset")) {
            from = i;
            break;
        }
    }
    return std::vector<SimEvent>(all.begin() + static_cast<std::ptrdiff_t>(from), all.end());
}

json::Value etaView(const json::Value& v) {
    double s = v.num(-1);
    if (s == -2) return json::Value("not reached at the current heat");
    if (s < 0) return json::Value();
    return json::Value(s);
}

json::Value summaryState(Host& host) {
    json::Value st = host.stateJson(false);
    json::Value v = obj();
    json::Value ro = obj();
    ro["id"] = st["roaster"]["id"];
    ro["name"] = st["roaster"]["name"];
    ro["kind"] = st["roaster"]["kind"];
    v["roaster"] = ro;
    v["bean"] = st["bean"];
    v["power"] = st["power"];
    v["phase"] = st["phase"];
    v["beans_in"] = st["bean_location"];
    double tr = st["t_roast"].num(-1);
    v["roast_time_s"] = tr >= 0 ? rnd(tr, 1) : json::Value();
    v["session_time_s"] = st["t"];

    json::Value temps = obj();
    for (const auto& p : st["probes"].items()) temps[p.get("id", "?") + "_C"] = p["value"];
    temps["RoR_BT_C_per_min"] = st["ror_bt"];
    temps["RoR_ET_C_per_min"] = st["ror_et"];
    v["temperatures"] = temps;
    json::Value faults = json::Value::array();
    for (const auto& p : st["probes"].items())
        if (p.getBool("fault", false)) faults.push(p["id"]);
    if (faults.size()) v["probe_faults"] = faults;

    json::Value eta = obj();
    eta["dry_end_s"] = etaView(st["eta_dry_s"]);
    eta["first_crack_s"] = etaView(st["eta_fc_s"]);
    if (!eta["dry_end_s"].isNull() || !eta["first_crack_s"].isNull()) v["eta_from_now"] = eta;

    json::Value ctl = obj();
    const json::Value& cs = st["controls"];
    for (const auto& k : cs.keys()) {
        json::Value c = obj();
        c["set"] = cs[k]["value"];
        if (cs[k]["actual"].num() != cs[k]["value"].num()) c["actual"] = cs[k]["actual"];
        ctl[k] = c.size() == 1 ? c["set"] : c;
    }
    v["controls_pct"] = ctl;

    const json::Value& m = st["machine"];
    json::Value mach = obj();
    mach["heater_W"] = m["heater_W"];
    mach["airflow_g_s"] = m["airflow_g_s"];
    if (m["drum_rpm"].num() > 0) mach["drum_rpm"] = m["drum_rpm"];
    if (m["stir_rpm"].num() > 0) mach["stir_rpm"] = m["stir_rpm"];
    if (!m["bed_state"].str().empty()) mach["bed_state"] = m["bed_state"];
    if (m["bean_env_C"].num(-1) >= 0) mach["bean_environment_C"] = m["bean_env_C"];
    v["machine"] = mach;

    const json::Value& b = st["beans"];
    if (b.getBool("present", false)) {
        json::Value beans = obj();
        beans["mass_g"] = b["mass_g"];
        beans["moisture_pct"] = b["moisture_pct"];
        beans["weight_loss_pct"] = b["weight_loss_pct"];
        beans["mean_C"] = b["T_mean"];
        beans["surface_C"] = b["T_surface"];
        beans["core_C"] = b["T_core"];
        beans["agtron_ground"] = b["agtron_ground"];
        beans["agtron_whole"] = b["agtron_whole"];
        beans["level"] = b["level"];
        beans["first_crack_pct"] = rnd(b["fc_frac"].num() * 100.0, 1);
        beans["second_crack_pct"] = rnd(b["sc_frac"].num() * 100.0, 1);
        if (b["scorch_frac"].num() > 0) beans["scorched_pct"] = rnd(b["scorch_frac"].num() * 100.0, 1);
        if (b["tipping_frac"].num() > 0) beans["tipped_pct"] = rnd(b["tipping_frac"].num() * 100.0, 1);
        v["beans"] = beans;
    } else {
        v["next_batch_g"] = st["next_batch_g"];
    }
    v["smoke"] = st["smoke"];
    v["alarms"] = st["alarms"];
    json::Value evs = json::Value::array();
    auto roast = currentRoastEvents(host);
    std::size_t first = roast.size() > 30 ? roast.size() - 30 : 0;
    for (std::size_t i = first; i < roast.size(); ++i) evs.push(eventView(roast[i]));
    v["roast_events"] = evs;
    json::Value clock = obj();
    clock["paused"] = st["paused"];
    clock["time_scale"] = st["time_scale"];
    clock["artisan_logging"] = st["logger_connected"];
    v["clock"] = clock;
    v["display_units"] = st["units"];
    return v;
}

// ------------------------------------------------------------ tools

using Handler = Outcome (*)(Host&, const json::Value& args);

struct Tool {
    const char* name;
    const char* title;
    const char* description;
    const char* schema;  // JSON Schema for the arguments
    bool readOnly;
    bool destructive;
    Handler handler;
    json::Value parsedSchema = json::Value();  // filled from `schema` on first use
};

Outcome getState(Host& host, const json::Value& a) {
    if (a.get("detail", "summary") != "full") return okData(summaryState(host));
    json::Value v = host.stateJson(true);
    json::Value evs = json::Value::array();
    for (const auto& e : currentRoastEvents(host)) evs.push(eventView(e));
    v["roast_events"] = evs;
    return okData(v);
}

Outcome listPresets(Host& host, const json::Value&) {
    json::Value l = host.command(command("list"), kOrigin);
    json::Value v = obj();
    json::Value rs = json::Value::array();
    for (const auto& r : l["roasters"].items()) {
        json::Value o = obj();
        o["id"] = r["id"];
        o["name"] = r["name"];
        o["kind"] = r["kind"];
        if (!r["description"].str().empty()) o["description"] = r["description"];
        o["batch_g"] = r["batch"];
        json::Value cs = json::Value::array();
        for (const auto& c : r["controls"].items()) cs.push(c["id"]);
        o["controls"] = cs;
        json::Value ps = json::Value::array();
        for (const auto& p : r["probes"].items()) ps.push(p["id"]);
        o["probes"] = ps;
        rs.push(o);
    }
    json::Value bs = json::Value::array();
    for (const auto& b : l["beans"].items()) {
        json::Value o = obj();
        o["id"] = b["id"];
        o["name"] = b["name"];
        if (!b["origin"].str().empty()) o["origin"] = b["origin"];
        if (!b["process"].str().empty()) o["process"] = b["process"];
        o["moisture_pct"] = rnd(b["moisture_pct"].num(), 1);
        o["density_g_l"] = rnd(b["density"].num(), 0);
        bs.push(o);
    }
    v["roasters"] = rs;
    v["beans"] = bs;
    json::Value st = host.stateJson(false);
    v["loaded_roaster"] = st["roaster"]["id"];
    v["loaded_bean"] = st["bean"]["id"];
    return okData(v);
}

Outcome loadRoaster(Host& host, const json::Value& a) {
    json::Value c = command("load");
    if (a.has("roaster")) c["roaster"] = a["roaster"];
    if (a.has("bean")) c["bean"] = a["bean"];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err + " (list_presets shows the ids)");
    json::Value v = obj();
    v["loaded"] = true;
    json::Value st = host.stateJson(true);
    v["roaster"] = st["roaster_def"];
    v["roaster"].erase("probes");
    v["bean"] = st["bean"];
    v["power"] = st["power"];
    v["note"] = "The machine starts cold and empty.";
    return okData(v);
}

Outcome selectBean(Host& host, const json::Value& a) {
    json::Value c = command("bean");
    c["id"] = a["bean"];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError("unknown bean '" + a.get("bean", "") + "' (list_presets shows the ids)");
    json::Value v = obj();
    v["bean"] = host.stateJson(false)["bean"];
    v["note"] = "Used for the next charge; beans already in the roaster are unchanged.";
    return okData(v);
}

Outcome setControls(Host& host, const json::Value& a) {
    json::Value c = command("set");
    json::Value cs = obj();
    for (const auto& k : a.keys()) cs[k] = a[k];
    if (!cs.size()) return toolError("give at least one control, e.g. {\"heat\": 80}");
    c["controls"] = cs;
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) {
        json::Value have = host.stateJson(false)["controls"];
        std::string names;
        for (const auto& k : have.keys()) names += (names.empty() ? "" : ", ") + k;
        return toolError(err + ". This roaster has: " + names);
    }
    json::Value st = host.stateJson(false);
    json::Value v = obj();
    json::Value out = obj();
    for (const auto& k : st["controls"].keys()) out[k] = st["controls"][k]["value"];
    v["controls_pct"] = out;
    if (!st.getBool("power", false)) v["note"] = "The power is off: the settings apply once set_power turns it on.";
    return okData(v);
}

Outcome setPower(Host& host, const json::Value& a) {
    json::Value c = command("power");
    c["value"] = a.getBool("on", true);
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err);
    json::Value v = obj();
    v["power"] = r["power"];
    return okData(v);
}

Outcome preheat(Host& host, const json::Value& a) {
    json::Value c = command("preheat");
    c["target_C"] = a["target_C"];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err);
    json::Value v = obj();
    v["simulated_s"] = r["simulated_s"];
    v["note"] = "Fast-forwarded until ET held the target. The heat control is left where the preheat controller ended.";
    v["state"] = summaryState(host);
    return okData(v);
}

Outcome simpleAction(Host& host, json::Value c) {
    json::Value r;
    std::string err;
    if (!run(host, std::move(c), r, err)) return toolError(err);
    return okData(summaryState(host));
}

Outcome charge(Host& host, const json::Value& a) {
    json::Value c = command("charge");
    if (a.has("batch_g")) c["batch_g"] = a["batch_g"];
    return simpleAction(host, c);
}

Outcome drop(Host& host, const json::Value&) { return simpleAction(host, command("drop")); }
Outcome endCooling(Host& host, const json::Value&) { return simpleAction(host, command("cool_end")); }

Outcome markEvent(Host& host, const json::Value& a) {
    json::Value c = command("mark");
    c["event"] = a["event"];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err);
    json::Value v = obj();
    v["marked"] = a["event"];
    return okData(v);
}

// --- advance_time

struct Advance {
    double seconds = 30;
    std::string until;
    bool hasBt = false;
    double btTarget = 0;
    bool rising = true;
    bool stopOnAlarm = true;
    double t0 = 0;
    std::uint64_t seq0 = 0;
    double wall0 = 0;
    std::string mode;
    std::string note;
};

// Returns why the advance stops ("" = keep going).
std::string advanceCheck(Host& host, const Advance& ad, const Snapshot& s, bool* reached) {
    for (const auto& e : host.eventsSince(ad.seq0)) {
        if (!ad.until.empty() && e.type == ad.until) {
            *reached = true;
            return "event " + e.type;
        }
        if (ad.stopOnAlarm && e.type == "ALARM") return "alarm: " + e.detail;
    }
    if (ad.hasBt && (ad.rising ? s.bt >= ad.btTarget : s.bt <= ad.btTarget)) {
        *reached = true;
        return std::string("BT ") + (ad.rising ? "rose to " : "fell to ") + json::formatNumber(ad.btTarget, 1) + " C";
    }
    if (s.t_session - ad.t0 >= ad.seconds - 1e-6) {
        *reached = ad.until.empty() && !ad.hasBt;
        return "time";
    }
    return "";
}

json::Value advanceResult(Host& host, const Advance& ad, const std::string& why, bool reached) {
    Snapshot s = host.snapshot();
    json::Value v = obj();
    v["mode"] = ad.mode;
    v["simulated_s"] = rnd(s.t_session - ad.t0, 1);
    v["stopped_because"] = why;
    if (!ad.until.empty() || ad.hasBt) v["condition_met"] = reached;
    json::Value evs = json::Value::array();
    for (const auto& e : host.eventsSince(ad.seq0)) evs.push(eventView(e));
    v["new_events"] = evs;
    if (!ad.note.empty()) v["note"] = ad.note;
    v["state"] = summaryState(host);
    return v;
}

Outcome advanceTime(Host& host, const json::Value& a) {
    auto ad = std::make_shared<Advance>();
    ad->seconds = a.get("seconds", 30.0);
    if (!(ad->seconds > 0 && ad->seconds <= 3600)) return toolError("seconds must be in (0, 3600]");
    ad->until = a.get("until", "");
    ad->stopOnAlarm = a.getBool("stop_on_alarm", true);
    Snapshot s = host.snapshot();
    ad->t0 = s.t_session;
    ad->seq0 = s.last_event_seq;
    ad->wall0 = now();
    if (a.has("until_bt_C")) {
        ad->hasBt = true;
        ad->btTarget = a["until_bt_C"].num();
        ad->rising = s.bt < ad->btTarget;
    }
    if (!ad->until.empty() && ad->until != "CHARGE" && ad->until != "NOTE") {
        for (const auto& e : currentRoastEvents(host))
            if (e.type == ad->until)
                return toolError(ad->until + " already happened in this roast (at " +
                                 json::formatNumber(e.t_roast, 0) + " s); nothing to wait for");
    }

    bool logger = host.loggerConnected() && host.config().lock_realtime_with_logger;
    if (!a.getBool("realtime", false) && !logger) {
        ad->mode = "fast-forward";
        std::string why;
        bool reached = false;
        while (why.empty()) {
            double left = ad->seconds - (host.snapshot().t_session - ad->t0);
            json::Value c = command("step");
            c["seconds"] = std::max(0.05, std::min(1.0, left));
            host.command(c, kOrigin);
            why = advanceCheck(host, *ad, host.snapshot(), &reached);
        }
        return okData(advanceResult(host, *ad, why, reached));
    }

    ad->mode = "real-time";
    if (logger) ad->note = "Artisan is logging, so the clock runs at 1x real time and cannot be fast-forwarded.";
    if (host.paused()) {
        ad->note = "The simulation is paused, so no time passes in real-time mode. Unpause with set_speed, "
                   "or call advance_time without realtime to fast-forward.";
        return okData(advanceResult(host, *ad, "paused", false));
    }
    Outcome o;
    o.poll = [&host, ad](json::Value& data) {
        bool reached = false;
        std::string why = advanceCheck(host, *ad, host.snapshot(), &reached);
        if (why.empty() && now() - ad->wall0 >= McpServer::kMaxWallWait)
            why = "wall-clock limit for one call; call advance_time again to keep waiting";
        if (why.empty()) return false;
        data = advanceResult(host, *ad, why, reached);
        return true;
    };
    return o;
}

// --- curve

Outcome roastCurve(Host& host, const json::Value& a) {
    double interval = std::max(1.0, a.get("interval_s", 30.0));
    double windowMin = std::max(1.0, a.get("minutes", 15.0));
    // Window: from 30 s before the last CHARGE, else the last `minutes`.
    double chargeT = -1;
    auto roastEvs = currentRoastEvents(host);
    if (!roastEvs.empty() && roastEvs.front().type == "CHARGE") chargeT = roastEvs.front().t_session;
    std::uint64_t next = 0;
    auto hist = host.historySince(0, &next);
    if (hist.empty()) return toolError("no history yet");
    double from = chargeT >= 0 ? chargeT - 30.0 : static_cast<double>(hist.back().t) - windowMin * 60.0;
    bool roastClock = chargeT >= 0;

    json::Value rows = json::Value::array();
    double lastBucket = -1e18;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const auto& h = hist[i];
        if (h.t < from) continue;
        double x = roastClock ? static_cast<double>(h.t) - chargeT : static_cast<double>(h.t);
        double bucket = std::floor(x / interval);
        bool last = i + 1 == hist.size();
        if (bucket == lastBucket && !last) continue;
        lastBucket = bucket;
        json::Value r = json::Value::array();
        r.push(rnd(x, 0));
        r.push(rnd(h.bt, 1));
        r.push(rnd(h.et, 1));
        r.push(rnd(h.ror_bt, 1));
        r.push(rnd(h.heat, 0));
        r.push(rnd(h.fan, 0));
        rows.push(r);
        ++kept;
    }
    // Keep replies small: thin evenly to at most 400 rows.
    if (kept > 400) {
        json::Value thin = json::Value::array();
        double stride = static_cast<double>(kept) / 400.0;
        for (double k = 0; k < static_cast<double>(kept); k += stride) thin.push(rows[static_cast<std::size_t>(k)]);
        rows = thin;
    }
    json::Value v = obj();
    v["clock"] = roastClock ? "seconds since CHARGE (negative = before charge)" : "session seconds (nothing charged)";
    v["columns"] = "time_s, BT_C, ET_C, RoR_BT_C_per_min, heat_pct, fan_pct";
    v["rows"] = rows;
    json::Value evs = json::Value::array();
    for (const auto& e : roastEvs) evs.push(eventView(e));
    v["events"] = evs;
    return okData(v);
}

Outcome report(Host& host, const json::Value&) {
    json::Value r = host.command(command("report"), kOrigin);
    if (!r.getBool("valid", false))
        return toolError("no roast report yet: it is available once a charged roast has been dropped");
    r.erase("ok");
    r.erase("valid");
    return okData(r);
}

Outcome setEnvironment(Host& host, const json::Value& a) {
    json::Value c = command("env");
    for (const auto& k : a.keys()) c[k] = a[k];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err);
    json::Value v = obj();
    v["environment"] = r["env"];
    return okData(v);
}

Outcome probeFault(Host& host, const json::Value& a) {
    json::Value c = command("fault");
    c["probe"] = a["probe"];
    c["type"] = a["type"];
    if (a.has("param")) c["param"] = a["param"];
    json::Value r;
    std::string err;
    if (!run(host, c, r, err)) return toolError(err);
    json::Value v = obj();
    v["probe"] = a["probe"];
    v["fault"] = a["type"];
    return okData(v);
}

Outcome setSpeed(Host& host, const json::Value& a) {
    json::Value r;
    std::string err;
    if (a.has("time_scale")) {
        json::Value c = command("time_scale");
        c["value"] = a["time_scale"];
        if (!run(host, c, r, err)) return toolError(err);
    }
    if (a.has("paused")) {
        json::Value c = command("pause");
        c["value"] = a["paused"];
        if (!run(host, c, r, err)) return toolError(err);
    }
    json::Value v = obj();
    v["paused"] = host.paused();
    v["time_scale"] = host.timeScale();
    if (host.loggerConnected() && host.config().lock_realtime_with_logger)
        v["note"] = "Artisan is logging, so the clock runs at 1x until it disconnects.";
    return okData(v);
}

Outcome resetMachine(Host& host, const json::Value& a) {
    std::string mode = a.get("mode", "");
    const char* cmd = mode == "empty" ? "empty" : mode == "cold" ? "reset" : mode == "repair" ? "repair" : nullptr;
    if (!cmd) return toolError("mode must be empty, cold or repair");
    return simpleAction(host, command(cmd));
}

Outcome rawCommand(Host& host, const json::Value& a) {
    const json::Value& c = a["command"];
    if (!c.isObject() || c.get("cmd", "").empty()) return toolError("command must be an object with \"cmd\"");
    std::string name = c.get("cmd", "");
    if (name == "subscribe" || name == "audio" || name == "audio_mix")
        return toolError("'" + name + "' only works on a WebSocket connection");
    json::Value r = host.command(c, kOrigin);
    if (r.isObject() && r.has("ok") && !r.getBool("ok", false)) return toolError(r.get("error", "command failed"));
    return okData(r);
}

// ------------------------------------------------------------ tool table

std::vector<Tool>& tools() {
    static std::vector<Tool> t = [] {
        std::vector<Tool> v = {
            {"get_state", "Roaster state",
             "Current state of the simulated roaster: temperatures (BT bean probe, ET environment), rate of rise, "
             "controls, bean colour (Agtron) and moisture, events of this roast (CHARGE, TP, DRY, FCs, ...), "
             "estimated time to dry end and first crack, alarms, and whether the clock is paused.",
             R"J({"type":"object","properties":{"detail":{"type":"string","enum":["summary","full"],
                 "description":"summary (default) or full: every field including the roaster definition"}}})J",
             true, false, getState},
            {"list_presets", "List roasters and beans",
             "The roaster machines and green coffees that can be loaded, with their ids, controls and batch sizes.",
             R"J({"type":"object","properties":{}})J", true, false, listPresets},
            {"load_roaster", "Load a roaster",
             "Load a roaster preset (and optionally a bean). This replaces the machine with a cold, empty one, "
             "discarding any roast in progress.",
             R"J({"type":"object","properties":{
                 "roaster":{"type":"string","description":"roaster id from list_presets (default: keep the current one)"},
                 "bean":{"type":"string","description":"bean id from list_presets (default: keep the current one)"}}})J",
             false, true, loadRoaster},
            {"select_bean", "Choose the green coffee",
             "Choose the green coffee for the next charge without resetting the machine.",
             R"J({"type":"object","properties":{"bean":{"type":"string","description":"bean id from list_presets"}},
                 "required":["bean"]})J",
             false, false, selectBean},
            {"set_controls", "Set roaster controls",
             "Set one or more controls in percent (0-100). Which ones exist depends on the roaster: heat "
             "(burner/heater power), fan (airflow), drum (drum speed), stir (stir-arm speed), voltage (variac on "
             "a stock popper), cooling (cooling tray, 0 or 100). Numbered controls snap to their nearest level.",
             R"J({"type":"object","properties":{
                 "heat":{"type":"number","minimum":0,"maximum":100},
                 "fan":{"type":"number","minimum":0,"maximum":100},
                 "drum":{"type":"number","minimum":0,"maximum":100},
                 "stir":{"type":"number","minimum":0,"maximum":100},
                 "voltage":{"type":"number","minimum":0,"maximum":100},
                 "cooling":{"type":"number","minimum":0,"maximum":100}}})J",
             false, false, setControls},
            {"set_power", "Power switch",
             "Switch the roaster's master power on or off. While off every actuator is at 0 and the machine "
             "cools; the control settings are kept.",
             R"J({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})J", false, false, setPower},
            {"preheat", "Preheat",
             "Fast-forward the empty, powered-on machine with a temperature controller until ET holds the target.",
             R"J({"type":"object","properties":{"target_C":{"type":"number","minimum":40,"maximum":450,
                 "description":"target environment temperature in C"}},"required":["target_C"]})J",
             false, false, preheat},
            {"charge", "Charge beans",
             "Drop the green beans into the roaster: starts the roast clock (CHARGE).",
             R"J({"type":"object","properties":{"batch_g":{"type":"number","exclusiveMinimum":0,
                 "description":"batch size in grams (default: the roaster's default or the last batch)"}}})J",
             false, false, charge},
            {"drop", "Drop beans",
             "End the roast: beans go to the cooling tray (DROP). The roast report becomes available.",
             R"J({"type":"object","properties":{}})J", false, false, drop},
            {"end_cooling", "End cooling",
             "End cooling now (COOL event). Otherwise it ends by itself once the beans are below 40 C.",
             R"J({"type":"object","properties":{}})J", false, false, endCooling},
            {"mark_event", "Mark an event",
             "Mark a roast event by hand: DRY, FCs, FCe, SCs or SCe (stops the simulator auto-detecting that one), "
             "or any other text as a note.",
             R"J({"type":"object","properties":{"event":{"type":"string",
                 "description":"DRY, FCs, FCe, SCs, SCe, or free text for a note"}},"required":["event"]})J",
             false, false, markEvent},
            {"advance_time", "Let time pass",
             "Let the roast run for some simulated seconds, or until an event or BT temperature, then return what "
             "happened and the new state. Fast-forwards instantly (works while paused) unless Artisan is logging "
             "or realtime is true; real-time waits return after at most 50 s of wall time, so call again to keep "
             "waiting. Stops early on any alarm unless stop_on_alarm is false.",
             R"J({"type":"object","properties":{
                 "seconds":{"type":"number","exclusiveMinimum":0,"maximum":3600,
                     "description":"most simulated seconds to advance (default 30)"},
                 "until":{"type":"string","enum":["TP","DRY","FCs","FCe","SCs","SCe","CHARGE","DROP","COOL"],
                     "description":"stop as soon as this event happens"},
                 "until_bt_C":{"type":"number","description":"stop when BT reaches this temperature (rising or falling)"},
                 "realtime":{"type":"boolean","description":"wait in real time instead of fast-forwarding"},
                 "stop_on_alarm":{"type":"boolean","description":"stop on a new alarm (default true)"}}})J",
             false, false, advanceTime},
            {"get_roast_curve", "Roast curve",
             "The roast so far as a table sampled every interval_s: time, BT, ET, BT rate of rise, heat and fan, "
             "plus this roast's events. Starts 30 s before CHARGE; with nothing charged, covers the last minutes.",
             R"J({"type":"object","properties":{
                 "interval_s":{"type":"number","minimum":1,"description":"seconds between rows (default 30)"},
                 "minutes":{"type":"number","minimum":1,"description":"window when nothing is charged (default 15)"}}})J",
             true, false, roastCurve},
            {"get_report", "Roast report",
             "Report for the last dropped roast: TP, dry end, first crack and drop times and temperatures, "
             "development time ratio, Agtron colour, weight and moisture loss, defects and notes.",
             R"J({"type":"object","properties":{}})J", true, false, report},
            {"set_environment", "Set environment",
             "Change the room and supply: ambient temperature, humidity, wall voltage, supply resistance, altitude, "
             "gas pressure (fraction of nominal).",
             R"J({"type":"object","properties":{
                 "ambient_C":{"type":"number"},"humidity_rh":{"type":"number","minimum":0,"maximum":100},
                 "line_voltage":{"type":"number","minimum":0},"source_ohms":{"type":"number","minimum":0},
                 "altitude_m":{"type":"number"},"gas_pressure":{"type":"number","minimum":0}}})J",
             false, false, setEnvironment},
            {"inject_probe_fault", "Inject a probe fault",
             "Simulate a thermocouple fault on a probe (for training and testing loggers), or clear it with none. "
             "param: short = reading in C, drift = C per second; otherwise unused.",
             R"J({"type":"object","properties":{
                 "probe":{"type":"string","description":"probe id, e.g. BT or ET"},
                 "type":{"type":"string","enum":["none","open","short","spikes","drift","stuck","noisy"]},
                 "param":{"type":"number"}},"required":["probe","type"]})J",
             false, false, probeFault},
            {"set_speed", "Clock speed",
             "Pause or resume the simulation clock and set its speed (1 = real time, up to 120x). While Artisan "
             "is logging the clock runs at 1x regardless.",
             R"J({"type":"object","properties":{
                 "paused":{"type":"boolean"},
                 "time_scale":{"type":"number","exclusiveMinimum":0,"maximum":120}}})J",
             false, false, setSpeed},
            {"reset_machine", "Reset the machine",
             "empty: discard the beans, keep the machine hot. cold: discard the beans, cool the machine to ambient, "
             "switch it off, put the controls back to the roaster's defaults and clear the history. repair: replace a blown thermal fuse.",
             R"J({"type":"object","properties":{"mode":{"type":"string","enum":["empty","cold","repair"]}},
                 "required":["mode"]})J",
             false, true, resetMachine},
            {"roastsim_command", "Raw RoastSim command",
             "Send any command of RoastSim's JSON API (docs/API.md), e.g. {\"cmd\":\"units\",\"temperature\":\"F\"}, "
             "{\"cmd\":\"history\",\"since\":0}, {\"cmd\":\"preset_types\"}, {\"cmd\":\"server\"}. Use the other "
             "tools when one fits.",
             R"J({"type":"object","properties":{"command":{"type":"object","description":"an object with \"cmd\""}},
                 "required":["command"]})J",
             false, true, rawCommand},
        };
        for (auto& tool : v) {
            tool.parsedSchema = json::Value::parse(tool.schema);
            tool.parsedSchema["additionalProperties"] = false;
        }
        return v;
    }();
    return t;
}

const Tool* findTool(const std::string& name) {
    for (const auto& t : tools())
        if (name == t.name) return &t;
    return nullptr;
}

// Checks arguments against the (simple) schemas above; returns "" when valid.
std::string validate(const json::Value& schema, const json::Value& args) {
    const json::Value& props = schema["properties"];
    for (const auto& k : args.keys()) {
        if (!props.has(k)) {
            std::string names;
            for (const auto& p : props.keys()) names += (names.empty() ? "" : ", ") + p;
            return "unknown argument '" + k + "'" + (names.empty() ? " (this tool takes none)" : " (expected: " + names + ")");
        }
        const json::Value& p = props[k];
        const json::Value& v = args[k];
        std::string type = p.get("type", "");
        bool good = type == "number"    ? v.isNumber()
                    : type == "boolean" ? v.isBool()
                    : type == "string"  ? v.isString()
                    : type == "object"  ? v.isObject()
                                        : true;
        if (!good) return "'" + k + "' must be a " + type;
        if (p.has("enum")) {
            bool found = false;
            for (const auto& e : p["enum"].items()) found |= e.str() == v.str();
            if (!found) {
                std::string names;
                for (const auto& e : p["enum"].items()) names += (names.empty() ? "" : ", ") + e.str();
                return "'" + k + "' must be one of " + names;
            }
        }
        if (v.isNumber()) {
            double x = v.num();
            if (!std::isfinite(x)) return "'" + k + "' must be finite";
            if (p.has("minimum") && x < p["minimum"].num()) return "'" + k + "' must be >= " + json::formatNumber(p["minimum"].num());
            if (p.has("maximum") && x > p["maximum"].num()) return "'" + k + "' must be <= " + json::formatNumber(p["maximum"].num());
            if (p.has("exclusiveMinimum") && x <= p["exclusiveMinimum"].num())
                return "'" + k + "' must be > " + json::formatNumber(p["exclusiveMinimum"].num());
        }
    }
    for (const auto& r : schema["required"].items())
        if (!args.has(r.str())) return "missing required argument '" + r.str() + "'";
    return "";
}

// ------------------------------------------------------------ JSON-RPC

json::Value rpcResult(const json::Value& id, json::Value result) {
    json::Value v = obj();
    v["jsonrpc"] = "2.0";
    v["id"] = id;
    v["result"] = std::move(result);
    return v;
}

json::Value rpcError(const json::Value& id, int code, const std::string& msg) {
    json::Value v = obj();
    v["jsonrpc"] = "2.0";
    v["id"] = id;
    json::Value e = obj();
    e["code"] = code;
    e["message"] = msg;
    v["error"] = e;
    return v;
}

json::Value toolResult(const Outcome& o) {
    json::Value r = obj();
    json::Value content = json::Value::array();
    json::Value text = obj();
    text["type"] = "text";
    text["text"] = o.error.empty() ? o.data.dump() : o.error;
    content.push(text);
    r["content"] = content;
    if (o.error.empty() && o.data.isObject()) r["structuredContent"] = o.data;
    r["isError"] = !o.error.empty();
    return r;
}

json::Value toolList() {
    json::Value arr = json::Value::array();
    for (const auto& t : tools()) {
        json::Value o = obj();
        o["name"] = t.name;
        o["title"] = t.title;
        o["description"] = t.description;
        o["inputSchema"] = t.parsedSchema;
        json::Value ann = obj();
        ann["title"] = t.title;
        ann["readOnlyHint"] = t.readOnly;
        if (!t.readOnly) ann["destructiveHint"] = t.destructive;
        ann["openWorldHint"] = false;
        o["annotations"] = ann;
        arr.push(o);
    }
    json::Value r = obj();
    r["tools"] = arr;
    return r;
}

json::Value initializeResult(const json::Value& params) {
    std::string asked = params.get("protocolVersion", "");
    std::string version = kProtocolVersions[0];
    for (const char* v : kProtocolVersions)
        if (asked == v) version = v;
    json::Value r = obj();
    r["protocolVersion"] = version;
    json::Value caps = obj();
    json::Value tl = obj();
    tl["listChanged"] = false;
    caps["tools"] = tl;
    r["capabilities"] = caps;
    json::Value info = obj();
    info["name"] = "roastsim";
    info["title"] = "RoastSim coffee roaster simulator";
    info["version"] = RS_VERSION;
    r["serverInfo"] = info;
    r["instructions"] = kInstructions;
    return r;
}

}  // namespace

McpServer::McpServer(Host& host) : host_(host) { tools(); }

json::Value McpServer::parseError(const std::string& detail) {
    return rpcError(json::Value(), -32700, "parse error: " + detail);
}

McpServer::Result McpServer::handle(const json::Value& msg) {
    Result res;
    if (msg.isArray()) {
        res.reply = rpcError(json::Value(), -32600, "JSON-RPC batches are not supported; send one message at a time");
        return res;
    }
    if (!msg.isObject() || msg.get("jsonrpc", "") != "2.0") {
        res.reply = rpcError(msg["id"], -32600, "not a JSON-RPC 2.0 message");
        return res;
    }
    const std::string method = msg.get("method", "");
    const bool isRequest = msg.has("id") && !msg["id"].isNull();
    if (method.empty()) return res;   // a response from the client (we never ask it anything)
    if (!isRequest) return res;       // notifications: initialized, cancelled, ...
    const json::Value& id = msg["id"];
    const json::Value& params = msg["params"];

    if (method == "initialize") {
        res.reply = rpcResult(id, initializeResult(params));
    } else if (method == "ping") {
        res.reply = rpcResult(id, obj());
    } else if (method == "tools/list") {
        res.reply = rpcResult(id, toolList());
    } else if (method == "tools/call") {
        const std::string name = params.get("name", "");
        const Tool* tool = findTool(name);
        if (!tool) {
            res.reply = rpcError(id, -32602, "unknown tool '" + name + "'");
            return res;
        }
        json::Value args = params["arguments"].isObject() ? params["arguments"] : obj();
        Outcome o;
        std::string bad = validate(tool->parsedSchema, args);
        if (!bad.empty()) o = toolError(bad);
        else {
            try {
                o = tool->handler(host_, args);
            } catch (const std::exception& e) {
                o = toolError(std::string("internal error: ") + e.what());
            }
        }
        if (o.poll) {
            auto poll = o.poll;
            json::Value rid = id;
            res.pending = [poll, rid](json::Value& reply) {
                json::Value data;
                if (!poll(data)) return false;
                reply = rpcResult(rid, toolResult(okData(std::move(data))));
                return true;
            };
        } else {
            res.reply = rpcResult(id, toolResult(o));
        }
    } else {
        res.reply = rpcError(id, -32601, "method not found: " + method);
    }
    return res;
}

}  // namespace roastsim
