#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>

#include "roastsim/audio.hpp"
#include "roastsim/host.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/roastsim.h"

using namespace roastsim;

struct rs_host {
    std::unique_ptr<Host> host;
};

namespace {
void copyStr(const std::string& s, char* out, size_t len) {
    if (!out || len == 0) return;
    size_t n = s.size() < len - 1 ? s.size() : len - 1;
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
}
}  // namespace

extern "C" {

int rs_api_version(void) { return RS_API_VERSION; }

rs_host* rs_host_create(const char* config_json, char* err, size_t errlen) {
    HostConfig cfg;
    if (config_json && *config_json) {
        std::string perr;
        json::Value v = json::Value::parse(config_json, &perr);
        if (!perr.empty()) {
            copyStr("config JSON: " + perr, err, errlen);
            return nullptr;
        }
        cfg = HostConfig::fromJson(v, cfg);
    }
    auto* h = new rs_host;
    h->host = std::make_unique<Host>(cfg);
    return h;
}

int rs_host_start(rs_host* h, char* err, size_t errlen) {
    if (!h) return 0;
    std::string e;
    if (!h->host->start(&e)) {
        copyStr(e, err, errlen);
        return 0;
    }
    return 1;
}

void rs_host_stop(rs_host* h) {
    if (h) h->host->stop();
}

void rs_host_destroy(rs_host* h) { delete h; }

size_t rs_host_command(rs_host* h, const char* json, const char* origin, char* out, size_t outlen) {
    if (!h || !json) return 0;
    std::string perr;
    json::Value cmd = json::Value::parse(json, &perr);
    json::Value reply;
    if (!perr.empty()) {
        reply = json::Value::object();
        reply["ok"] = false;
        reply["error"] = "bad JSON: " + perr;
    } else {
        reply = h->host->command(cmd, origin ? origin : "capi");
    }
    std::string s = reply.dump();
    copyStr(s, out, outlen);
    return s.size();
}

int rs_host_get_state(rs_host* h, rs_state* caller) {
    // Versioned struct: callers built against an older (shorter) rs_state get
    // the fields they know about. Fill a full copy, then copy struct_size bytes.
    if (!h || !caller || caller->struct_size < offsetof(rs_state, next_batch_g)) return 0;
    rs_state full{};
    rs_state* o = &full;
    Snapshot s = h->host->snapshot();
    std::uint64_t next = h->host->historyEnd();
    o->t_session = s.t_session;
    o->t_roast = s.t_roast;
    o->phase = static_cast<int>(s.phase);
    o->bean_location = static_cast<int>(s.bean_location);
    o->bt = s.bt;
    o->et = s.et;
    o->ror_bt = s.ror_bt;
    o->ror_et = s.ror_et;
    for (int i = 0; i < kControlCount && i < 8; ++i) {
        o->controls[i] = s.controls[i];
        o->actuators[i] = s.actuators[i];
        if (s.has_control[i]) o->control_mask |= 1u << i;
    }
    o->bean_mean_c = s.beans.T_mean;
    o->bean_surface_c = s.beans.T_surface;
    o->bean_core_c = s.beans.T_core;
    o->moisture_pct = s.beans.moisture_pct;
    o->weight_loss_pct = s.beans.weight_loss_pct;
    o->agtron_whole = s.beans.agtron_whole;
    o->agtron_ground = s.beans.agtron_ground;
    o->agtron_sd = s.beans.agtron_sd;
    o->fc_frac = s.beans.fc_frac;
    o->sc_frac = s.beans.sc_frac;
    o->smoke = s.smoke;
    for (int i = 0; i < 3; ++i) o->bean_rgb[i] = static_cast<float>(s.beans.rgb[i]);
    o->pops_fc = s.pops_fc;
    o->pops_sc = s.pops_sc;
    o->alarms = s.alarms;
    o->heater_w = s.tele.heater_W;
    o->airflow_g_s = s.tele.airflow_kg_s * 1000.0;
    o->inlet_c = s.tele.inlet_C;
    o->exhaust_c = s.tele.exhaust_C;
    o->wall_c = s.tele.wall_C;
    o->element_c = s.tele.element_C;
    o->agitation = s.tele.agitation;
    o->mix_quality = s.tele.mix_quality;
    o->time_scale = h->host->timeScale();
    o->paused = h->host->paused() ? 1 : 0;
    o->logger_connected = h->host->loggerConnected() ? 1 : 0;
    o->event_seq = s.last_event_seq;
    o->history_end = next;
    o->batch_g = s.batch_g;
    json::Value st = h->host->stateJson(false);
    o->next_batch_g = st.get("next_batch_g", s.batch_g);
    o->temp_unit = h->host->units();
    copyStr(h->host->weightUnits(), o->weight_unit, sizeof o->weight_unit);
    copyStr(h->host->altitudeUnits(), o->altitude_unit, sizeof o->altitude_unit);
    copyStr(s.tele.bed_state ? s.tele.bed_state : "", o->bed_state, sizeof o->bed_state);
    copyStr(s.beans.present ? roastLevelName(s.beans.agtron_ground) : "", o->roast_level, sizeof o->roast_level);
    o->bean_min_c = s.beans.T_min;
    o->bean_max_c = s.beans.T_max;
    o->expansion = s.beans.expansion;
    o->fill = s.tele.fill;
    o->bt_immersion = s.tele.bt_immersion;
    o->drum_rpm = s.tele.drum_rpm;
    o->stir_rpm = s.tele.stir_rpm;
    o->voltage_v = s.tele.voltage_V;
    o->chaff_g_s = s.chaff_rate_g_s;
    o->scorch_frac = s.beans.scorch_frac;
    o->tipping_frac = s.beans.tipping_frac;
    o->oil = s.beans.oil;
    o->ambient_c = s.env.ambient_C;
    o->line_voltage = s.env.line_voltage;
    o->altitude_m = s.env.altitude_m;
    o->humidity_rh = s.env.humidity_rh;
    std::uint32_t size = caller->struct_size;
    std::memcpy(caller, &full, std::min<std::size_t>(size, sizeof full));
    caller->struct_size = size;
    return 1;
}

int rs_host_set_control(rs_host* h, int control, double percent) {
    if (!h || control < 0 || control >= kControlCount) return 0;
    return h->host->setControl(static_cast<Control>(control), percent, "ui") ? 1 : 0;
}

int rs_host_get_history(rs_host* h, uint64_t from_index, rs_sample* out, int max, uint64_t* next_index) {
    if (!h || !out || max <= 0) return 0;
    std::uint64_t next = 0;
    auto v = h->host->historySince(from_index, &next, static_cast<std::size_t>(max));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const HistorySample& s = v[i];
        out[i] = rs_sample{s.t, s.t_roast, s.bt, s.et, s.ror_bt, s.ror_et, s.bean_true, s.heat, s.fan, s.drum};
    }
    if (next_index) *next_index = next;
    return static_cast<int>(v.size());
}

int rs_host_get_visual(rs_host* h, rs_visual_info* info, rs_visual_bean* beans, int max_beans) {
    if (!h) return 0;
    VisualState v = h->host->visual();
    if (info && info->struct_size >= sizeof(rs_visual_info)) {
        info->kind = static_cast<int>(v.kind);
        info->bean_location = static_cast<int>(v.bean_location);
        info->chamber_diameter = v.geometry.chamber_diameter;
        info->chamber_height = v.geometry.chamber_height;
        info->agitation = v.agitation;
        info->mix_quality = v.mix_quality;
        info->angle_rad = v.angle_rad;
        info->fan = v.fan;
        info->heat = v.heat;
        info->element_glow = v.element_glow;
        info->fill = v.fill;
        info->smoke = v.smoke;
        info->chaff_airborne = v.chaff_airborne;
        info->pops_fc = v.pops_fc;
        info->pops_sc = v.pops_sc;
        info->frame = v.frame;
        info->bean_count = static_cast<int>(v.beans.size());
    }
    int n = 0;
    if (beans) {
        for (; n < max_beans && n < static_cast<int>(v.beans.size()); ++n) {
            const VisualBean& b = v.beans[static_cast<std::size_t>(n)];
            rs_visual_bean& o = beans[n];
            for (int i = 0; i < 3; ++i) o.rgb[i] = b.rgb[i];
            o.size_mm = b.size_mm;
            o.oil = b.oil;
            o.temp_c = b.temp_C;
            o.count = b.count;
            o.exposure = b.exposure;
            o.cracked = b.cracked;
            o.scorch = b.scorch;
        }
    }
    return n;
}

// ---------------------------------------------------------------- sound
}  // extern "C"

struct rs_audio {
    std::unique_ptr<AudioSynth> synth;
    platform::AudioOutput out;
};

extern "C" {

rs_audio* rs_audio_create(rs_host* h, int sample_rate) {
    if (!h || sample_rate < 8000) return nullptr;
    auto* a = new rs_audio;
    a->synth = std::make_unique<AudioSynth>(*h->host, sample_rate);
    return a;
}

void rs_audio_destroy(rs_audio* a) {
    if (!a) return;
    a->out.stop();
    delete a;
}

void rs_audio_render(rs_audio* a, float* out, int frames) {
    if (a && out && frames > 0) a->synth->render(out, frames);
}

void rs_audio_set_mix(rs_audio* a, float master, float machine, float cracks) {
    if (!a) return;
    AudioMix m;
    m.master = master;
    m.machine = machine;
    m.cracks = cracks;
    a->synth->setMix(m);
}

int rs_audio_play(rs_audio* a, char* err, size_t errlen) {
    if (!a) return 0;
    if (a->out.running()) return 1;
    std::string e;
    auto fill = [](void* user, float* out, int frames) { static_cast<AudioSynth*>(user)->render(out, frames); };
    if (!a->out.start(a->synth->sampleRate(), fill, a->synth.get(), &e)) {
        copyStr(e, err, errlen);
        return 0;
    }
    return 1;
}

void rs_audio_pause(rs_audio* a) {
    if (a) a->out.stop();
}

}  // extern "C"
