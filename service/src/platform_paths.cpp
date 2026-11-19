#include <filesystem>

#include "roastsim/platform.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__HAIKU__)
#include <OS.h>
#include <image.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace roastsim::platform {

std::string executableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n == 0) return ".";
    return fs::path(std::wstring(buf, n)).parent_path().u8string();
#elif defined(__HAIKU__)
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (info.type == B_APP_IMAGE) return fs::path(info.name).parent_path().u8string();
    }
    return ".";
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    return fs::path(buf).parent_path().u8string();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return fs::path(buf).parent_path().u8string();
#endif
}

std::string findResourceDir(const std::string& name) {
    std::error_code ec;
    std::vector<fs::path> bases = {fs::u8path(executableDir()), fs::current_path(ec)};
    for (const auto& base : bases) {
        fs::path p = base;
        for (int up = 0; up < 4; ++up) {
            fs::path cand = p / name;
            if (fs::is_directory(cand, ec)) return cand.u8string();
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    return name;
}

}  // namespace roastsim::platform
