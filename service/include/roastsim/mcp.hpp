// Model Context Protocol (MCP) server: lets an AI assistant drive the roaster.
//
// Transport-free: it turns one JSON-RPC 2.0 message into a reply. Two
// transports use it (see docs/MCP.md):
//   * Streamable HTTP: POST /mcp on the web server (every front end that runs
//     the web server, so the Win32 app and roastsimd).
//   * stdio: `roastsimd --mcp`, one JSON-RPC message per line.
//
// Every tool goes through Host::command() with origin "mcp", like any other
// front end. Waiting tools (advance_time in real time) return a Pending that
// the transport polls without blocking its thread.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "roastsim/json.hpp"

namespace roastsim {

class Host;

class McpServer {
public:
    // Polled by the transport until it returns true; `reply` is then the
    // JSON-RPC response to send. Poll every ~100 ms.
    using Pending = std::function<bool(json::Value& reply)>;

    struct Result {
        json::Value reply;  // null: nothing to send (a notification or a client response)
        Pending pending;    // set: the reply comes later from polling this
    };

    explicit McpServer(Host& host);

    // One JSON-RPC message (already parsed). Never throws.
    Result handle(const json::Value& message);

    // JSON-RPC error for a message that could not be parsed.
    static json::Value parseError(const std::string& detail);

    // Longest real-time wait one advance_time call may take (wall seconds);
    // clients time tool calls out, so longer waits return partial progress.
    static constexpr double kMaxWallWait = 50.0;

private:
    Host& host_;
};

}  // namespace roastsim
