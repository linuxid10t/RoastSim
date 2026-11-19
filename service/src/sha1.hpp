#pragma once
#include <string>

namespace roastsim::detail {
std::string sha1(const std::string& msg);    // 20 raw bytes
std::string base64(const std::string& in);
}  // namespace roastsim::detail
