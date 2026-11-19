// Socket layer: Winsock on Windows, BSD sockets elsewhere (Linux, Haiku, BSD, macOS).
#ifdef _WIN32
#define FD_SETSIZE 512
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstring>

#include "roastsim/platform.hpp"

namespace roastsim::platform {

namespace {

#ifdef _WIN32
using native_t = SOCKET;
inline native_t nat(socket_t s) { return static_cast<native_t>(s); }
inline bool wouldBlock() {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
#else
using native_t = int;
inline native_t nat(socket_t s) { return static_cast<native_t>(s); }
inline bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

void setNonBlocking(native_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

void setNoDelay(native_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

}  // namespace

bool netInit() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void netShutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

socket_t listenTcp(const std::string& bindAddr, int port, std::string* err) {
    native_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        if (err) *err = "socket() failed";
        return kInvalidSocket;
    }
    int one = 1;
#ifdef _WIN32
    // Exclusive use, so two simulators can't silently share a port on Windows.
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    std::string a = bindAddr.empty() || bindAddr == "*" ? "0.0.0.0" : bindAddr;
    if (a == "localhost") a = "127.0.0.1";
    if (inet_pton(AF_INET, a.c_str(), &addr.sin_addr) != 1) {
        if (err) *err = "bad bind address '" + bindAddr + "'";
        closeSocket(static_cast<socket_t>(s));
        return kInvalidSocket;
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "cannot bind " + a + ":" + std::to_string(port) + " (port in use or needs privileges)";
        closeSocket(static_cast<socket_t>(s));
        return kInvalidSocket;
    }
    if (::listen(s, 16) != 0) {
        if (err) *err = "listen() failed";
        closeSocket(static_cast<socket_t>(s));
        return kInvalidSocket;
    }
    setNonBlocking(s);
    return static_cast<socket_t>(s);
}

socket_t acceptClient(socket_t listener, std::string* peer) {
    sockaddr_in addr;
#ifdef _WIN32
    int len = sizeof addr;
#else
    socklen_t len = sizeof addr;
#endif
    native_t c = ::accept(nat(listener), reinterpret_cast<sockaddr*>(&addr), &len);
#ifdef _WIN32
    if (c == INVALID_SOCKET) return kInvalidSocket;
#else
    if (c < 0) return kInvalidSocket;
#endif
    setNonBlocking(c);
    setNoDelay(c);
    if (peer) {
        char buf[64] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
        *peer = std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return static_cast<socket_t>(c);
}

socket_t connectTcp(const std::string& host, int port, std::string* err) {
    native_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        if (err) *err = "socket() failed";
        return kInvalidSocket;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    std::string h = host == "localhost" ? "127.0.0.1" : host;
    if (inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "cannot connect to " + h + ":" + std::to_string(port);
        closeSocket(static_cast<socket_t>(s));
        return kInvalidSocket;
    }
    setNonBlocking(s);
    setNoDelay(s);
    return static_cast<socket_t>(s);
}

long sendSome(socket_t s, const char* data, std::size_t len) {
#ifdef _WIN32
    int n = ::send(nat(s), data, static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
    ssize_t n = ::send(nat(s), data, len, MSG_NOSIGNAL);
#else
    ssize_t n = ::send(nat(s), data, len, 0);
#endif
    if (n >= 0) return static_cast<long>(n);
    return wouldBlock() ? 0 : -1;
}

long recvSome(socket_t s, char* buf, std::size_t len) {
#ifdef _WIN32
    int n = ::recv(nat(s), buf, static_cast<int>(len), 0);
#else
    ssize_t n = ::recv(nat(s), buf, len, 0);
#endif
    if (n > 0) return static_cast<long>(n);
    if (n == 0) return 0;
    return wouldBlock() ? -1 : -2;
}

void closeSocket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(nat(s));
#else
    ::close(nat(s));
#endif
}

int pollSockets(std::vector<PollItem>& items, int timeoutMs) {
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    native_t maxfd = 0;
    int count = 0;
    for (auto& it : items) {
        it.readable = it.writable = false;
        if (it.s == kInvalidSocket) continue;
        if (count >= FD_SETSIZE) break;
        FD_SET(nat(it.s), &rd);
        if (it.wantWrite) FD_SET(nat(it.s), &wr);
        if (nat(it.s) > maxfd) maxfd = nat(it.s);
        ++count;
    }
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (count == 0) {
        // select() with no sockets is an error on Windows; just sleep.
#ifdef _WIN32
        Sleep(static_cast<DWORD>(timeoutMs));
#else
        select(0, nullptr, nullptr, nullptr, &tv);
#endif
        return 0;
    }
    int r = ::select(static_cast<int>(maxfd + 1), &rd, &wr, nullptr, &tv);
    if (r <= 0) return r;
    for (auto& it : items) {
        if (it.s == kInvalidSocket) continue;
        it.readable = FD_ISSET(nat(it.s), &rd) != 0;
        it.writable = it.wantWrite && FD_ISSET(nat(it.s), &wr) != 0;
    }
    return r;
}

}  // namespace roastsim::platform
