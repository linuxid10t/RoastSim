#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "roastsim/platform.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <cstdio>
#elif defined(__HAIKU__)
#include <OS.h>
#include <image.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace roastsim::platform {

namespace {
class PresetDirectoryLock {
public:
    explicit PresetDirectoryLock(const fs::path& path) {
#ifdef _WIN32
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED region{};
        locked = file_ != INVALID_HANDLE_VALUE &&
                 LockFileEx(file_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region);
#else
        file_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
        struct flock region{};
        region.l_type = F_WRLCK;
        region.l_whence = SEEK_SET;
        region.l_len = 1;
        locked = file_ >= 0 && fcntl(file_, F_SETLK, &region) == 0;
#endif
    }

    ~PresetDirectoryLock() {
#ifdef _WIN32
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (file_ >= 0) close(file_);
#endif
    }

    bool locked = false;

private:
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int file_ = -1;
#endif
};
}

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

std::string userPresetDir() {
    fs::path base;
#if defined(_WIN32)
    if (const wchar_t* dir = _wgetenv(L"LOCALAPPDATA"); dir && *dir) base = dir;
#else
    const char* userHome = std::getenv("HOME");
    if (userHome && *userHome) {
#if defined(__HAIKU__)
        base = fs::u8path(userHome) / "config/settings";
#elif defined(__APPLE__)
        base = fs::u8path(userHome) / "Library/Application Support";
#else
        base = fs::u8path(userHome) / ".local/share";
#endif
    }
#if !defined(__HAIKU__) && !defined(__APPLE__)
    if (const char* dir = std::getenv("XDG_DATA_HOME"); dir && *dir && fs::u8path(dir).is_absolute())
        base = fs::u8path(dir);
#endif
#endif
    if (base.empty()) return (fs::u8path(executableDir()) / "presets").u8string();
    return (base / "RoastSim" / "presets").u8string();
}

bool writePreset(const std::string& path, const std::string& contents, const std::string* previous, std::string* err) {
    auto fail = [&](const char* reason) { if (err) *err = reason; return false; };
    const fs::path target = fs::u8path(path), folder = target.parent_path();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return fail("Could not create the custom preset folder.");
    static std::mutex processLock;
    std::lock_guard<std::mutex> guard(processLock);
    PresetDirectoryLock lock(folder / ".write.lock");
    if (!lock.locked) return fail("Could not lock the custom preset folder. Check permissions or try again.");
    const bool exists = fs::exists(target, ec);
    if (ec) return fail("Could not check the saved preset.");
    if (previous) {
        std::ifstream current(target, std::ios::binary);
        std::ostringstream text;
        if (current) text << current.rdbuf();
        if (!current || current.bad() || text.str() != *previous) return fail("This preset changed on disk. Reopen it before saving.");
    } else if (exists) return fail("Another copy was saved with this name. Try saving again.");

    const fs::path temp = folder / ".preset.tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    if (out) {
        fs::rename(temp, target, ec);
        if (!ec) return true;
    }
    fs::remove(temp, ec);
    return fail("Could not write the custom preset file.");
}

void setBinaryStdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

}  // namespace roastsim::platform
