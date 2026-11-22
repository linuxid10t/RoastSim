# Controlling RoastSim with AI (MCP)

RoastSim has a built-in [Model Context Protocol](https://modelcontextprotocol.io) server. An AI assistant such as Claude can read the roaster and drive a whole roast with it: pick a roaster and bean, preheat, charge, adjust heat and fan, watch for first crack, drop, and review the report.

The assistant drives the same machine as everything else. If the Win32 app or web UI is open, you watch the roast there while the AI runs it, and Artisan can log it as usual. Every change the AI makes goes through the [command API](API.md) with origin `mcp`, so the event log shows which events the AI marked.

## Two ways to connect

| Transport | Address | Use it with |
|---|---|---|
| Streamable HTTP | `http://127.0.0.1:8080/mcp` | Any client that supports remote or HTTP MCP servers, such as Claude Code. Works whenever the Win32 app or `roastsimd` is running. |
| stdio | `roastsimd --mcp` | Clients that launch the server themselves, such as Claude Desktop. |

`roastsimd --mcp` first looks for a RoastSim that is already running on the HTTP port (8080, or `--http-port`). If it finds one, it passes messages to it, so the AI drives the simulator you have open. If none is running, it starts its own simulator with the usual web UI and Modbus servers, and logs a line saying so on stderr. To choose explicitly, use `--mcp-connect HOST:PORT` (always bridge, and fail if nothing is there) or `--mcp-embedded` (always run its own).

To watch an AI roast in the Win32 app, **start the app before the AI client**. If the client starts `roastsimd --mcp` first, `roastsimd` runs its own simulator and takes port 8080. An app opened later can't get the port, so it runs a separate simulator, and the window doesn't show the roast the AI is running.

### Claude Code

The repository's [`.mcp.json`](../.mcp.json) adds `roastsim` as a project server, so Claude Code offers it when you open this folder. Approve it once when Claude Code asks. It connects to the HTTP address, so start the Win32 app or `roastsimd` first. If RoastSim wasn't running when the session started, start it and reconnect the server from `/mcp`.

To use RoastSim from Claude Code in other folders, start RoastSim and run:

```powershell
claude mcp add --scope user --transport http roastsim http://127.0.0.1:8080/mcp
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

### Hermes Agent

[Hermes Agent](https://hermes-agent.nousresearch.com/docs/user-guide/features/mcp) reads MCP servers from `mcp_servers` in `~/.hermes/config.yaml`. If Hermes runs on the same machine as RoastSim, start RoastSim and point Hermes at the HTTP address:

```yaml
mcp_servers:
  roastsim:
    url: "http://127.0.0.1:8080/mcp"
```

To have Hermes start RoastSim itself, use stdio instead:

```yaml
mcp_servers:
  roastsim:
    command: "C:/path/to/RoastSim2026/build/roastsimd.exe"
    args: ["--mcp"]
```

Then run `/reload-mcp` in a Hermes chat, or restart Hermes. The tools appear with a server prefix, for example `mcp__roastsim__advance_time`. If Hermes runs in WSL2, see [Clients in WSL2](#clients-in-wsl2).

**Options:**
* Hermes's default tool timeout (`timeout`, 300 s) is longer than a real-time `advance_time` wait, so you don't need to change it.
* To limit what the assistant can do, filter the tools by their RoastSim names. Use `tools: {include: [get_state, get_roast_curve, get_report]}` for a read-only assistant. To block resets and raw commands, use `tools: {exclude: [reset_machine, roastsim_command]}`.

### DeepSeek Harness

[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness) (`dsh`) connects to each MCP server through one `@deepseek-ai/dsh-mcp-client` plugin entry. Add an entry to your profile's `cordis.patch.yml` (profiles live under `$DSH_HOME/profiles`). This entry uses the HTTP address:

```yaml
- id: mcp-roastsim
  name: '@deepseek-ai/dsh-mcp-client'
  config:
    serverName: roastsim
    transport: streamable-http
    url: http://127.0.0.1:8080/mcp
    toolCallTimeoutMs: 90000
```

For stdio, replace the `transport` and `url` lines with `transport: stdio`, `command:` the path to `roastsimd.exe`, and `args: ['--mcp']`.

* Keep `toolCallTimeoutMs`. The default of 60 s is too close to the 50 s a real-time `advance_time` wait can take.
* Restart `dsh` after editing the file. `dsh --profile web --dump-config` shows whether the entry loaded.
* The tools appear as `mcp__roastsim__<tool>`.
* `dsh` has no per-server tool allow or deny lists. To keep an agent away from tools such as `reset_machine`, use its agent-level tool guards.
* If `dsh` runs in WSL2, see [Clients in WSL2](#clients-in-wsl2).

### ZCode

[ZCode](https://zcode.z.ai/en/docs/mcp-services) reads servers from `mcp.servers` in `~/.zcode/cli/config.json` for all projects, or in `<project>/.zcode/config.json` for one project. This is the stdio entry:

```json
{
  "mcp": {
    "servers": {
      "roastsim": {
        "command": "C:/path/to/RoastSim2026/build/roastsimd.exe",
        "args": ["--mcp"],
        "env": {}
      }
    }
  }
}
```

To use the HTTP address instead, add it in **Settings » MCP Servers » New MCP Server**: choose type **HTTP** and enter `http://127.0.0.1:8080/mcp`.

* `"enable": false` in the entry turns the server off.
* If a `.zcode` config defines any servers, ZCode ignores `.agents/mcp.json` at the same level.
* ZCode doesn't read this repository's `.mcp.json`.

### LM Studio

[LM Studio](https://lmstudio.ai/docs/app/mcp) reads `mcp.json` (**Program » Install » Edit mcp.json**) in the same layout as Cursor:

```json
{
  "mcpServers": {
    "roastsim": {
      "command": "C:/path/to/RoastSim2026/build/roastsimd.exe",
      "args": ["--mcp"]
    }
  }
}
```

* **Choose a model built for tool use.** A roast takes dozens of tool calls, so use a large enough context window that the model keeps the start of the roast in view.
* **Pause the clock first.** Local models can take a while to answer, so have the model call `set_speed` with `paused: true` before it charges. Otherwise the roast runs on while the model thinks.
* **Tool call prompts:** LM Studio asks you to confirm tool calls. Allowing `get_state` and `advance_time` permanently saves many clicks.

### Clients in WSL2

The HTTP address only accepts connections from the same machine. Under WSL's default networking, `127.0.0.1` inside WSL is not the Windows machine, so a client in WSL2 can't reach a RoastSim running on Windows at that address. Use one of these instead:

* **stdio:** use the client's stdio entry with the Windows path to `roastsimd.exe`, for example `/mnt/c/path/to/RoastSim2026/build/roastsimd.exe`. WSL runs Windows programs directly. `roastsimd.exe` then runs on Windows, where it can reach the RoastSim app on `127.0.0.1:8080`.
* **Mirrored networking:** set `networkingMode=mirrored` under `[wsl2]` in `%UserProfile%\.wslconfig`. The HTTP address then works unchanged.

Avoid binding RoastSim to `0.0.0.0` just for WSL, because the simulator has no login (see [Security](#security)).

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
