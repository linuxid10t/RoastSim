// Private header: concrete machine models.
#pragma once

#include "roastsim/machine.hpp"

namespace roastsim {

class FluidBedMachine : public MachineModel {
public:
    explicit FluidBedMachine(const RoasterDef& def);
    void reset(const Environment& env) override;
    void step(double dt, MachineInputs& in) override;
    double probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const override;
    void repair() override;

private:
    double ambient_ = 20;
    double T_el_ = 20, T_wall_ = 20, T_in_ = 20, T_out_ = 20, T_airEff_ = 20;
    double flow_ = 0;
    double pwmPhase_ = 0;
    bool thermostatOpen_ = false;
    bool fuseBlown_ = false;
    double fuseTimer_ = 0;
};

class DrumMachine : public MachineModel {
public:
    explicit DrumMachine(const RoasterDef& def);
    void reset(const Environment& env) override;
    void step(double dt, MachineInputs& in) override;
    double probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const override;

private:
    double ambient_ = 20;
    double P_src_ = 0;
    double T_in_ = 20, T_air_ = 20, T_drum_ = 20, T_outer_ = 20;
    double flow_ = 0;
    double angle_ = 0;
};

class StirPanMachine : public MachineModel {
public:
    explicit StirPanMachine(const RoasterDef& def);
    void reset(const Environment& env) override;
    void step(double dt, MachineInputs& in) override;
    double probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const override;

private:
    double ambient_ = 20;
    double T_pan_ = 20, T_dome_ = 20;
    double angle_ = 0;
    double pwmPhase_ = 0;
    bool thermostatOpen_ = false;
};

}  // namespace roastsim
