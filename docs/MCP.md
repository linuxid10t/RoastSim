# Controlling RoastSim with AI (MCP)

RoastSim has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server. An AI assistant such as Claude can read the roaster and drive a whole roast with it: pick a roaster and bean, preheat, charge, adjust heat and fan, watch for first crack, drop, and review the report.

The assistant drives the same machine as everything else. If the Win32 app or web UI is open, you watch the roast there while the AI runs it, and Artisan can log it as usual. Every change the AI makes goes through the [command API](API.md) with origin `mcp`, so the event log shows which events the AI marked.

## Two ways to connect

| Transport | Address | Use it with |
|---|---|---|
| Streamable HTTP | `http://127.0.0.1:8080/mcp` | Any client that supports remote or HTTP MCP servers, such as Claude Code. Works whenever the Win32 app or `roastsimd` is running. |
| stdio | `roastsimd --mcp` | Clients that launch the server themselves, such as Claude Desktop. |

`roastsimd --mcp` first looks for a RoastSim that is already running on the HTTP port (8080, or `--http-port`). If it finds one, it passes messages to it, so the AI drives the simulator you have open. If none is running, it starts its own simulator with the usual web UI and Modbus servers, and logs a line saying so on stderr. To choose explicitly, use `--mcp-connect HOST:PORT` (always bridge, and fail if nothing is there) or `--mcp-embedded` (always run its own).

### Claude Code

Start RoastSim, then run:

```powershell
claude mcp add --transport http roastsim http://127.0.0.1:8080/mcp
```

### Claude Desktop

Claude Desktop starts local servers itself. Add this to `claude_desktop_config.json` (**Settings » Developer » Edit Config**), with the path to your build:

```json
{
  "mcpServers": {
    "roastsim": {
      "command": "C:\\path\\to\\RoastSim2026\\build\\roastsimd.exe",
      "args": ["--mcp"]
    }
  }
}
```

Restart Claude Desktop. If the Win32 app is open, the AI drives it. Otherwise `roastsimd` runs a simulator in the background, and you can watch it at `http://localhost:8080/`.

### Other clients

Any MCP client works. Use the HTTP address for clients that support it, or run `roastsimd --mcp` as a stdio command. To try the tools by hand, the [MCP Inspector](https://github.com/modelcontextprotocol/inspector) can connect to `http://127.0.0.1:8080/mcp` with the **Streamable HTTP** transport.

## Tools

| Tool | What it does |
|---|---|
| `get_state` | Temperatures, rate of rise, controls, bean colour and moisture, events in this roast, estimated time to dry end and first crack, alarms, and the clock. Use `detail: "full"` for every field. |
| `list_presets` | Roasters (with controls and batch limits) and beans |
| `load_roaster` | Load a roaster and, optionally, a bean. The machine starts cold and empty. |
| `select_bean` | Choose the coffee for the next charge |
| `set_controls` | `heat`, `fan`, `drum`, `stir`, `voltage`, `cooling` in percent |
| `set_power` | Master power switch |
| `preheat` | Fast-forward the empty machine until ET holds a target |
| `charge` / `drop` / `end_cooling` | Beans in, beans out, end cooling |
| `mark_event` | Mark DRY, FCs, FCe, SCs or SCe by hand, or add a note |
| `advance_time` | Let time pass: some seconds, or until an event (`until: "FCs"`) or a BT temperature (`until_bt_C`). Returns the new events and the state. |
| `get_roast_curve` | The roast as a table every N seconds: time, BT, ET, RoR, heat and fan, plus events |
| `get_report` | Report for the last dropped roast: times, DTR, Agtron, weight loss and defects |
| `set_environment` | Ambient temperature, humidity, wall voltage, supply resistance, altitude, gas pressure |
| `inject_probe_fault` | Open, short, spikes, drift, stuck or noisy thermocouple, or none to clear it |
| `set_speed` | Pause or resume the clock and set the speed (1-120x) |
| `reset_machine` | `empty` (discard beans), `cold` (back to ambient), `repair` (replace a blown fuse) |
| `roastsim_command` | Any [API command](API.md), for everything else, such as units, presets or history |

Values are in °C, grams and percent, whatever units the UI shows. `get_state` reports the UI's units as `display_units`, so the assistant can answer in the units you use.

## Time

The simulator runs in real time, and keeps running while the AI is thinking between tool calls. For a careful roast, ask the AI to pause the clock (`set_speed` with `paused: true`) and move it with `advance_time`. This fast-forwards the simulation immediately: 12 minutes of roasting take well under a second.

When Artisan is logging, the clock is locked to real time, so `advance_time` waits instead of fast-forwarding. You can also ask for a real-time wait (`realtime: true`), for example to watch the roast at normal speed. A wait returns after at most 50 seconds of wall time, because clients time out long tool calls, and the AI then calls it again.

`advance_time` stops early when a new alarm goes off (scorching, a stalled bed, a blown fuse, fire risk and so on) unless `stop_on_alarm` is false. This gives the AI a chance to react.

## Security

The MCP endpoint has the same access as the web UI and `/api/command`: anyone who can reach the HTTP port can drive the simulator. By default RoastSim listens only on `127.0.0.1`. The MCP endpoint rejects requests that come from web pages on other sites (it checks the `Origin` header), so a page you browse to can't drive your roaster. There is no authentication, so think twice before setting `bind_address` to `0.0.0.0`.

## Protocol details

* MCP protocol versions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05. The server answers with the client's version when it knows it.
* Tools only (no resources or prompts). Tool results carry JSON as text and as `structuredContent`.
* HTTP: `POST /mcp` with one JSON-RPC message, answered with `application/json`. A notification gets `202 Accepted`. There is no SSE stream and no session ID, so `GET /mcp` returns `405`.
* stdio: one JSON-RPC message per line. Only protocol messages are written to stdout; logs go to stderr.
* Implementation: [`service/src/mcp_server.cpp`](../service/src/mcp_server.cpp) (transport-free), the `/mcp` route in `web_server.cpp`, and `--mcp` in `apps/roastsimd/main.cpp`.
