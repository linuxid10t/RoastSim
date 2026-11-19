// Private service-layer declarations.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "roastsim/host.hpp"
#include "roastsim/platform.hpp"

namespace roastsim {

// Modbus TCP: consumes complete MBAP frames from `in`, appends replies to `out`.
// Returns false if the stream is corrupt and the connection should be dropped.
bool modbusProcess(Host& host, std::string& in, std::string& out);

// One select() loop serving the Modbus listener and the HTTP/WebSocket listener.
class NetServer {
public:
    explicit NetServer(Host& host) : host_(host) {}
    ~NetServer() { stop(); }
    bool start(std::string* err, std::vector<std::string>& messages);
    void stop();
    json::Value info() const;

private:
    struct Impl;
    void loop();

    Host& host_;
    platform::socket_t modbusListener_ = platform::kInvalidSocket;
    platform::socket_t httpListener_ = platform::kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex infoMu_;
    int modbusClients_ = 0, httpClients_ = 0, wsUiClients_ = 0, wsArtisanClients_ = 0;
    std::string modbusStatus_ = "off", httpStatus_ = "off";
};

// TC4 / aArtisanQ emulation on a serial port.
class Tc4Serial {
public:
    explicit Tc4Serial(Host& host) : host_(host) {}
    ~Tc4Serial() { stop(); }
    bool start(const std::string& port, int baud, std::string* err);
    void stop();
    std::string status() const;

    // Exposed for tests: process one command line, return reply ("" = none).
    std::string handleLine(const std::string& line);

private:
    void loop();

    Host& host_;
    platform::SerialPort port_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string status_ = "off";
    // aArtisanQ state
    int chanMap_[4] = {1, 2, 0, 0};  // logical chan -> physical TC (1-based, 0 = off)
    bool pidOn_ = false;
    double pidSv_ = 0, kp_ = 5, ki_ = 0.15, kd_ = 0, pidI_ = 0, pidPrev_ = 0;
    int pidChan_ = 2;
    double lastPidT_ = -1;
};

}  // namespace roastsim
