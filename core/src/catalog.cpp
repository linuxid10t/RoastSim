#include "roastsim/catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace roastsim {

namespace {
template <typename Preset>
void putPreset(std::vector<Preset>& presets, Preset preset) {
    auto existing = std::find_if(presets.begin(), presets.end(), [&](const auto& p) { return p.id == preset.id; });
    if (existing == presets.end()) presets.push_back(std::move(preset));
    else *existing = std::move(preset);
}
}

void Catalog::putRoaster(RoasterDef def) { putPreset(roasters_, std::move(def)); }
void Catalog::putBean(BeanSpec spec) { putPreset(beans_, std::move(spec)); }

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool Catalog::addRoasterJson(const std::string& text, std::string* err) {
    std::string perr;
    json::Value v = json::Value::parse(text, &perr);
    if (!perr.empty()) {
        if (err) *err = "JSON: " + perr;
        return false;
    }
    RoasterDef d;
    if (!RoasterDef::fromJson(v, d, err)) return false;
    putRoaster(std::move(d));
    return true;
}

bool Catalog::addBeanJson(const std::string& text, std::string* err) {
    std::string perr;
    json::Value v = json::Value::parse(text, &perr);
    if (!perr.empty()) {
        if (err) *err = "JSON: " + perr;
        return false;
    }
    if (!v.isObject() || v.get("id", "").empty()) {
        if (err) *err = "bean definition needs an \"id\"";
        return false;
    }
    putBean(BeanSpec::fromJson(v));
    return true;
}

bool Catalog::loadDir(const std::string& dataDir) {
    dir_ = dataDir;
    auto loadKind = [&](const char* sub, bool roaster) {
        std::error_code ec;
        fs::path p = fs::u8path(dataDir) / sub;
        if (!fs::is_directory(p, ec)) {
            errors_.push_back("missing directory " + p.u8string());
            return;
        }
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(p, ec))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::string text, err;
            if (!readTextFile(f.u8string(), text)) {
                errors_.push_back("cannot read " + f.u8string());
                continue;
            }
            bool ok = roaster ? addRoasterJson(text, &err) : addBeanJson(text, &err);
            if (!ok) errors_.push_back(f.filename().u8string() + ": " + err);
        }
    };
    loadKind("roasters", true);
    loadKind("beans", false);
    return !roasters_.empty() && !beans_.empty();
}

const RoasterDef* Catalog::roaster(const std::string& id) const {
    for (const auto& r : roasters_)
        if (r.id == id) return &r;
    return nullptr;
}

const BeanSpec* Catalog::bean(const std::string& id) const {
    for (const auto& b : beans_)
        if (b.id == id) return &b;
    return nullptr;
}

json::Value Catalog::listJson() const {
    json::Value v = json::Value::object();
    json::Value rs = json::Value::array();
    for (const auto& r : roasters_) rs.push(r.describe());
    json::Value bs = json::Value::array();
    for (const auto& b : beans_) bs.push(b.toJson());
    v["roasters"] = rs;
    v["beans"] = bs;
    return v;
}

}  // namespace roastsim
