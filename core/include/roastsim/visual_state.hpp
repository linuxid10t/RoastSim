// Render-facing state for the future 3D view (and the 2D bean previews today).
//
// Contract: the simulation owns all physics; a renderer only *reads* a
// VisualState snapshot (thread-safe copy from the host) and is free to invent
// purely cosmetic motion (bean trajectories, smoke particles) seeded from these
// fields. Nothing a renderer does feeds back into the simulation, so the 3D
// view can run at its own frame rate and be absent entirely (headless).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "roastsim/roaster_def.hpp"

namespace roastsim {

struct VisualBean {
    float rgb[3];        // surface colour (linear-ish sRGB 0..1)
    float size_mm;       // equivalent diameter now
    float oil;           // 0..1 surface oil (specular)
    float temp_C;        // surface temperature (heat-shimmer / glow effects)
    float count;         // beans represented by this parcel
    float exposure;      // current mixing state: >1 = near the heat (hot zone)
    float cracked;       // 0..1 fraction past first crack (split crease)
    float scorch;        // 0..1 scorch marks
};

struct VisualGeometry {
    // Machine-family specific dimensions, metres.
    float chamber_diameter = 0;  // fluid bed chamber / drum / pan
    float chamber_height = 0;    // fluid bed height / drum length / dome height
};

struct VisualState {
    std::uint64_t frame = 0;     // increases every simulation step
    double t_session = 0;
    RoasterKind kind = RoasterKind::FluidBed;
    FluidBedAirPattern air_pattern = FluidBedAirPattern::Upflow;
    std::string roaster_id;
    VisualGeometry geometry;
    BeanLocation bean_location = BeanLocation::None;
    float agitation = 0;         // fluid bed lift ratio / drum speed ratio / stir fraction
    float mix_quality = 1;
    float angle_rad = 0;         // drum or stir-arm rotation
    float fan = 0;               // 0..1
    float heat = 0;              // 0..1 power fraction delivered
    float element_glow = 0;      // 0..1 from element temperature (dull red ~ 500 C)
    float fill = 0;              // bean volume / chamber volume
    float smoke = 0;             // 0..1 density of smoke being produced
    float chaff_airborne = 0;    // grams/second of chaff being shed
    std::uint64_t pops_fc = 0;   // monotonic crack counters (spawn audio/particles on change)
    std::uint64_t pops_sc = 0;
    std::uint32_t alarms = 0;
    std::vector<VisualBean> beans;  // one entry per parcel
};

}  // namespace roastsim
