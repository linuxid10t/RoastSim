// Serial ports. Windows: COMx via the Win32 comm API (pair with a virtual null-
// modem such as com0com so Artisan can open the other end). POSIX (Linux,
// Haiku, macOS): termios on a tty, or "pty" to create a pseudo-terminal whose
// slave path Artisan can open directly; no extra drivers needed.
#include "roastsim/platform.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace roastsim::platform {

#ifdef _WIN32

struct SerialPort::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
};

SerialPort::SerialPort() : impl_(new Impl) {}
SerialPort::~SerialPort() {
    close();
    delete impl_;
}

bool SerialPort::open(const std::string& name, int baud, std::string* err) {
    close();
    std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot open " + name + " (does the port exist? for Artisan, create a virtual pair with com0com)";
        return false;
    }
    DCB dcb;
    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    SetCommState(h, &dcb);
    impl_->h = h;
    desc_ = name;
    return true;
}

void SerialPort::close() {
    if (impl_->h != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->h);
        impl_->h = INVALID_HANDLE_VALUE;
    }
}

bool SerialPort::isOpen() const { return impl_->h != INVALID_HANDLE_VALUE; }

long SerialPort::read(char* buf, std::size_t len, int timeoutMs) {
    if (!isOpen()) return -1;
    COMMTIMEOUTS to;
    ZeroMemory(&to, sizeof to);
    to.ReadIntervalTimeout = 20;
    to.ReadTotalTimeoutConstant = static_cast<DWORD>(timeoutMs);
    to.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(impl_->h, &to);
    DWORD n = 0;
    if (!ReadFile(impl_->h, buf, static_cast<DWORD>(len), &n, nullptr)) return -1;
    return static_cast<long>(n);
}

long SerialPort::write(const char* data, std::size_t len) {
    if (!isOpen()) return -1;
    DWORD n = 0;
    if (!WriteFile(impl_->h, data, static_cast<DWORD>(len), &n, nullptr)) return -1;
    return static_cast<long>(n);
}

#else  // POSIX

struct SerialPort::Impl {
    int fd = -1;
};

SerialPort::SerialPort() : impl_(new Impl) {}
SerialPort::~SerialPort() {
    close();
    delete impl_;
}

namespace {
speed_t baudConst(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
#ifdef B115200
        case 115200: return B115200;
#endif
        default: return B9600;
    }
}
}  // namespace

bool SerialPort::open(const std::string& name, int baud, std::string* err) {
    close();
    int fd = -1;
    if (name == "pty") {
        fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
            if (err) *err = "cannot create pseudo-terminal";
            if (fd >= 0) ::close(fd);
            return false;
        }
        const char* slave = ptsname(fd);
        desc_ = slave ? slave : "pty";
    } else {
        fd = ::open(name.c_str(), O_RDWR | O_NOCTTY);
        if (fd < 0) {
            if (err) *err = "cannot open " + name;
            return false;
        }
        desc_ = name;
    }
    termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, baudConst(baud));
        cfsetospeed(&tio, baudConst(baud));
        tio.c_cflag |= CLOCAL | CREAD;
        tcsetattr(fd, TCSANOW, &tio);
    }
    impl_->fd = fd;
    return true;
}

void SerialPort::close() {
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
}

bool SerialPort::isOpen() const { return impl_->fd >= 0; }

long SerialPort::read(char* buf, std::size_t len, int timeoutMs) {
    if (impl_->fd < 0) return -1;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(impl_->fd, &rd);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(impl_->fd + 1, &rd, nullptr, nullptr, &tv);
    if (r <= 0) return 0;
    ssize_t n = ::read(impl_->fd, buf, len);
    if (n < 0) return (errno == EAGAIN || errno == EINTR || errno == EIO) ? 0 : -1;  // EIO: pty slave not open yet
    return static_cast<long>(n);
}

long SerialPort::write(const char* data, std::size_t len) {
    if (impl_->fd < 0) return -1;
    ssize_t n = ::write(impl_->fd, data, len);
    return n < 0 ? -1 : static_cast<long>(n);
}

#endif

}  // namespace roastsim::platform
