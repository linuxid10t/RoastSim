// Loads roaster and bean definitions from a data directory:
//   <data>/roasters/*.json   <data>/beans/*.json
// Uses only std::filesystem / std::ifstream (portable to Haiku's GCC).
#pragma once

#include <string>
#include <vector>

#include "roastsim/bean.hpp"
#include "roastsim/roaster_def.hpp"

namespace roastsim {

class Catalog {
public:
    bool loadDir(const std::string& dataDir);  // false if nothing could be loaded
    bool addRoasterJson(const std::string& text, std::string* err);
    bool addBeanJson(const std::string& text, std::string* err);

    const RoasterDef* roaster(const std::string& id) const;
    const BeanSpec* bean(const std::string& id) const;
    const std::vector<RoasterDef>& roasters() const { return roasters_; }
    const std::vector<BeanSpec>& beans() const { return beans_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::string& dataDir() const { return dir_; }

    json::Value listJson() const;

private:
    std::vector<RoasterDef> roasters_;
    std::vector<BeanSpec> beans_;
    std::vector<std::string> errors_;
    std::string dir_;
};

bool readTextFile(const std::string& path, std::string& out);

}  // namespace roastsim
