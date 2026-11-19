// Thin OS abstraction for the service layer. Everything OS-specific in the
// project lives behind this header (plus the native UI shells):
//   platform_net.cpp     Winsock (Windows) / BSD sockets (Linux, Haiku, macOS)
//   platform_serial.cpp  Win32 COM ports / POSIX termios + pseudo-terminals
//   platform_paths.cpp   executable location (Win32, /proc, Haiku image info)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace roastsim::platform {

// ---------------------------------------------------------------- sockets
using socket_t = std::intptr_t;
constexpr socket_t kInvalidSocket = -1;

bool netInit();
void netShutdown();
socket_t listenTcp(const std::string& bindAddr, int port, std::string* err);
socket_t acceptClient(socket_t listener, std::string* peer);
// Blocking connect, then switches the socket to non-blocking (clients, tests).
socket_t connectTcp(const std::string& host, int port, std::string* err);
// > 0 bytes sent; 0 would block; < 0 error
long sendSome(socket_t s, const char* data, std::size_t len);
// > 0 bytes read; 0 orderly close; -1 would block; -2 error
long recvSome(socket_t s, char* buf, std::size_t len);
void closeSocket(socket_t s);

struct PollItem {
    socket_t s = kInvalidSocket;
    bool wantWrite = false;
    bool readable = false;
    bool writable = false;
};
// select()-based wait; returns number of ready sockets, 0 on timeout, < 0 on error.
int pollSockets(std::vector<PollItem>& items, int timeoutMs);

// ---------------------------------------------------------------- serial
class SerialPort {
public:
    SerialPort();
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // name: "COM7" (Windows), "/dev/ttyUSB0" (POSIX), or "pty" (POSIX: create a
    // pseudo-terminal; its slave path is reported by description()).
    bool open(const std::string& name, int baud, std::string* err);
    void close();
    bool isOpen() const;
    // Returns bytes read (0 on timeout), -1 on error.
    long read(char* buf, std::size_t len, int timeoutMs);
    long write(const char* data, std::size_t len);
    std::string description() const { return desc_; }

private:
    struct Impl;
    Impl* impl_;
    std::string desc_;
};

// ---------------------------------------------------------------- paths
std::string executableDir();
// Finds a directory named `name` next to the executable, in the working
// directory, or a few levels up (for running from a build tree).
std::string findResourceDir(const std::string& name);

}  // namespace roastsim::platform
