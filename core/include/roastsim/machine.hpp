// Machine models: the heat sources, air paths and thermal masses of each
// roaster family. A machine computes, every step, the heat flowing into each
// bean parcel (Parcel::q_in) and the local hot-spot temperature (Parcel::spot_T),
// then advances its own thermal nodes. Bean physics lives in BeanBatch.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "roastsim/bean.hpp"
#include "roastsim/roaster_def.hpp"
#include "roastsim/rng.hpp"
#include "roastsim/types.hpp"

namespace roastsim {

struct MachineTelemetry {
    double heater_W = 0;        // power currently delivered by the heat source
    double voltage_V = 0;       // voltage at the appliance (electric machines)
    double airflow_kg_s = 0;
    double inlet_C = 0;         // air entering the bean chamber
    double exhaust_C = 0;       // air leaving
    double air_C = 0;           // representative chamber/drum/dome air
    double wall_C = 0;          // drum shell / pan / chamber wall
    double element_C = 0;       // heater element (or flame region)
    double agitation = 0;       // fluid bed: airflow / fluidization requirement
    double mix_quality = 1;     // 0 = beans static, 1 = well mixed
    double fill = 0;            // bean volume / chamber volume
    double drum_rpm = 0;
    double stir_rpm = 0;
    double angle_rad = 0;       // drum or stir-arm angle (for the 3D view)
    double bt_immersion = 0;    // how buried the BT probe is (0..1)
    // Temperature the beans are being driven towards: every heat source
    // (hot air, drum wall, pan, radiation) weighted by how strongly it couples
    // to the beans. Used to project time to DRY / first crack; -1 = no beans.
    double bean_env_C = -1;
    std::uint32_t alarms = 0;
    const char* bed_state = "empty";
};

struct MachineInputs {
    const Environment* env = nullptr;
    double u[kControlCount] = {};  // actuator commands, 0..1
    BeanBatch* beans = nullptr;    // beans in the machine, or nullptr
    Rng* rng = nullptr;
};

class MachineModel {
public:
    explicit MachineModel(const RoasterDef& def) : def_(def) {}
    virtual ~MachineModel() = default;

    virtual void reset(const Environment& env) = 0;        // cold machine at ambient
    virtual void step(double dt, MachineInputs& in) = 0;
    // Temperature a probe at `loc` is exposed to, and how immersed in beans it is.
    virtual double probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const = 0;
    virtual void repair() {}  // e.g. replace a blown thermal fuse

    const MachineTelemetry& telemetry() const { return tel_; }
    const RoasterDef& def() const { return def_; }

protected:
    const RoasterDef& def_;
    MachineTelemetry tel_;
};

std::unique_ptr<MachineModel> createMachine(const RoasterDef& def);

// Shared helper: per-parcel exposure multiplier from the mixing state.
inline double exposure(const Parcel& p, double sigmaDyn, double sigmaStatic, double floor = 0.05) {
    return std::max(floor, 1.0 + sigmaDyn * p.z_conv + sigmaStatic * p.bias);
}

}  // namespace roastsim
