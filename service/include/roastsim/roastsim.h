/*
 * roastsim.h - stable C API for RoastSim front ends.
 *
 * Every native shell (Win32 today; GTK and Haiku BeAPI later) talks to the
 * simulator only through this header, so shells can be written in C, C++ or
 * anything with a C FFI, and the core can change without breaking them.
 *
 * Two levels:
 *   1. rs_host_command(): the full JSON command API (same as the WebSocket
 *      API, see docs/API.md). Flexible, used for setup and rare actions.
 *   2. Typed fast paths (rs_host_get_state / history / visual) for UIs that
 *      redraw many times a second. Structs carry struct_size for versioning:
 *      set it to sizeof(struct) before the call.
 *
 * Thread safety: all functions may be called from any thread.
 */
#ifndef ROASTSIM_H
#define ROASTSIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(RS_BUILD_SHARED)
#define RS_API __declspec(dllexport)
#elif defined(_WIN32) && defined(RS_USE_SHARED)
#define RS_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(RS_BUILD_SHARED)
#define RS_API __attribute__((visibility("default")))
#else
#define RS_API
#endif

#define RS_API_VERSION 1

/* Control indices (match the core's Control enum). */
enum {
    RS_CTL_HEAT = 0,
    RS_CTL_FAN = 1,
    RS_CTL_DRUM = 2,
    RS_CTL_STIR = 3,
    RS_CTL_VOLTAGE = 4,
    RS_CTL_COOLING = 5,
    RS_CTL_COUNT = 6
};

enum { RS_PHASE_IDLE = 0, RS_PHASE_ROASTING = 1, RS_PHASE_COOLING = 2, RS_PHASE_DONE = 3 };
enum { RS_KIND_FLUID_BED = 0, RS_KIND_DRUM = 1, RS_KIND_STIR_PAN = 2 };

typedef struct rs_host rs_host;

typedef struct rs_state {
    uint32_t struct_size;
    double t_session, t_roast;          /* t_roast < 0 when not charged */
    int phase;                          /* RS_PHASE_* */
    int bean_location;                  /* 0 none, 1 roaster, 2 cooling tray */
    double bt, et, ror_bt, ror_et;      /* probe readings, C; -1 = open probe */
    double controls[8];                 /* requested %, indexed by RS_CTL_* */
    double actuators[8];                /* actual % (after slew) */
    uint32_t control_mask;              /* bit i set if the roaster has control i */
    double bean_mean_c, bean_surface_c, bean_core_c;
    double moisture_pct, weight_loss_pct;
    double agtron_whole, agtron_ground, agtron_sd;
    double fc_frac, sc_frac;
    double smoke;                       /* 0..1 */
    float bean_rgb[3];
    uint64_t pops_fc, pops_sc;          /* monotonic crack counters */
    uint32_t alarms;                    /* bit set, see docs/API.md */
    double heater_w, airflow_g_s, inlet_c, exhaust_c, wall_c, element_c;
    double agitation, mix_quality;
    double time_scale;
    int paused;
    int logger_connected;
    uint64_t event_seq;                 /* highest event sequence number */
    uint64_t history_end;               /* index one past the newest history sample */
    double batch_g;
} rs_state;

typedef struct rs_sample {
    float t, t_roast, bt, et, ror_bt, ror_et, bean_true, heat, fan, drum;
} rs_sample;

typedef struct rs_visual_info {
    uint32_t struct_size;
    int kind;                           /* RS_KIND_* */
    int bean_location;
    float chamber_diameter, chamber_height;
    float agitation, mix_quality, angle_rad, fan, heat, element_glow, fill, smoke, chaff_airborne;
    uint64_t pops_fc, pops_sc, frame;
    int bean_count;                     /* parcels available */
} rs_visual_info;

typedef struct rs_visual_bean {
    float rgb[3];
    float size_mm, oil, temp_c, count, exposure, cracked, scorch;
} rs_visual_bean;

RS_API int rs_api_version(void);

/* config_json: NULL or a JSON object (see HostConfig / docs/API.md), e.g.
 *   {"roaster":"popper_modded","modbus_port":502,"http_port":8080,"bind_address":"127.0.0.1"}
 * On failure returns NULL and writes a message into err. */
RS_API rs_host* rs_host_create(const char* config_json, char* err, size_t errlen);
RS_API int rs_host_start(rs_host* h, char* err, size_t errlen);  /* 1 ok, 0 fail */
RS_API void rs_host_stop(rs_host* h);
RS_API void rs_host_destroy(rs_host* h);

/* Runs a JSON command, writes the JSON reply (NUL-terminated, truncated if
 * needed) into out, returns the full reply length (call again with a bigger
 * buffer if the return value >= outlen). origin labels the caller (e.g. "ui"). */
RS_API size_t rs_host_command(rs_host* h, const char* json, const char* origin, char* out, size_t outlen);

RS_API int rs_host_get_state(rs_host* h, rs_state* out);                         /* 1 ok */
RS_API int rs_host_set_control(rs_host* h, int control, double percent);         /* 1 ok */
/* Copies history samples [from_index, ...) into out; returns the count copied
 * and stores the next index to ask for. */
RS_API int rs_host_get_history(rs_host* h, uint64_t from_index, rs_sample* out, int max, uint64_t* next_index);
/* Fills info and up to max_beans parcels; returns the number of beans written. */
RS_API int rs_host_get_visual(rs_host* h, rs_visual_info* info, rs_visual_bean* beans, int max_beans);

/* ---------------------------------------------------------------- sound
 * Procedural roaster sound (fans, burner, drum, beans, first/second crack),
 * rendered from the live simulation; see docs/SOUND.md. Either let RoastSim
 * play it (rs_audio_play: Windows today) or pull samples into your own audio
 * callback with rs_audio_render (any platform). */
typedef struct rs_audio rs_audio;

RS_API rs_audio* rs_audio_create(rs_host* h, int sample_rate);
RS_API void rs_audio_destroy(rs_audio* a);
/* Mono float samples in [-1, 1]. Call from one thread at a time. */
RS_API void rs_audio_render(rs_audio* a, float* out, int frames);
/* Levels 0..1 (master, machine noises, cracks). */
RS_API void rs_audio_set_mix(rs_audio* a, float master, float machine, float cracks);
/* Plays through the platform's default output. 1 ok, 0 unsupported/failed (see err). */
RS_API int rs_audio_play(rs_audio* a, char* err, size_t errlen);
RS_API void rs_audio_pause(rs_audio* a);

#ifdef __cplusplus
}
#endif

#endif /* ROASTSIM_H */
