// RoastSim native Windows shell.
//
// Talks to the simulator only through the C API (roastsim.h) plus the JSON
// helper for parsing replies, so it doubles as the reference for the GTK and
// Haiku BeAPI shells. Hosts the same servers as roastsimd (Modbus, WebSocket,
// web UI, optional TC4 serial), so Artisan can connect while you watch here.
//
// Layout: the left pane holds only what you touch during a roast (roaster and
// bean, charge/drop, the machine's controls) plus the live readout. Machine
// chores (preheat, reset, repair, probe faults), the environment, simulation
// speed and sound live in menus and dialogs; a status bar along the bottom
// shows power, speed, environment and connections (click the speed or
// environment part to change them).
//
// Painting: the main window clips its children and only the graph/bean area
// (right side) is repainted on the refresh timer; the readout is a small
// double-buffered custom control and text controls are only updated when their
// text changes. That keeps the left pane from flickering.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "roastsim/catalog.hpp"
#include "roastsim/json.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/roastsim.h"
#include "resource.h"

using roastsim::json::Value;

namespace {

// ---------------------------------------------------------------- ids
enum : int {
    IDC_ROASTER = 101, IDC_BEAN, IDC_BATCH, IDC_BATCHUNIT, IDC_CHARGE, IDC_DROP, IDC_MARKFC, IDC_POWER, IDC_COOL,
    IDC_READOUT, IDC_STATUSBAR, IDC_DESC, IDC_PHASES,
    IDC_SLIDER0 = 200, IDC_SLABEL0 = 220, IDC_SVALUE0 = 240,
    IDM_EXIT = 1001, IDM_WEB, IDM_FOLDER, IDM_COOLEND, IDM_EMPTY, IDM_REPORT,
    IDM_MARK_DRY = 1020, IDM_MARK_FCS, IDM_MARK_FCE, IDM_MARK_SCS, IDM_MARK_SCE,
    IDM_FAULT_BT_OPEN = 1040, IDM_FAULT_BT_SPIKES, IDM_FAULT_BT_DRIFT, IDM_FAULT_BT_NOISY, IDM_FAULT_ET_OPEN,
    IDM_FAULT_CLEAR, IDM_ABOUT = 1060,
    IDM_POWER = 1070, IDM_PREHEAT, IDM_RESET, IDM_REPAIR, IDM_RELOAD,
    IDM_ENV = 1080, IDM_ENV_AMBIENT, IDM_ENV_VOLTS, IDM_ENV_ALT,
    IDM_TEMP_C = 1100, IDM_TEMP_F, IDM_W_G, IDM_W_KG, IDM_W_OZ, IDM_W_LB, IDM_ALT_M, IDM_ALT_FT,
    IDM_SPEED0 = 1200,  // + index into kSpeeds
    IDM_SOUND = 1220, IDM_VOL0,  // + index into kVolumes
};
enum : UINT_PTR { TIMER_UI = 1 };
constexpr UINT WM_APP_ROAST_DONE = WM_APP + 1;  // posted when COOL arrives: power off, show report

constexpr int kSliderCount = 5;  // heat, fan, drum, stir, voltage
const int kSliderControls[kSliderCount] = {RS_CTL_HEAT, RS_CTL_FAN, RS_CTL_DRUM, RS_CTL_STIR, RS_CTL_VOLTAGE};

// Simulation speeds (0 = paused) and sound volumes offered in the menus.
constexpr int kSpeedCount = 7;
const double kSpeeds[kSpeedCount] = {0, 1, 2, 5, 10, 30, 60};
constexpr int kVolumeCount = 4;
const int kVolumes[kVolumeCount] = {25, 50, 75, 100};

// Status bar parts; clicking SB_SPEED or SB_ENV opens the matching menu/dialog.
enum { SB_STATE, SB_SPEED, SB_ENV, SB_MODBUS, SB_WEB, SB_LOGGER, SB_COUNT };

// ---------------------------------------------------------------- helpers
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n + 1), L'\0');
    GetWindowTextW(h, &w[0], n + 1);
    w.resize(static_cast<size_t>(n));
    return w;
}

// Avoids needless repaints (and flicker) when the text didn't change.
void setText(HWND h, const std::wstring& text) {
    if (getText(h) != text) SetWindowTextW(h, text.c_str());
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 1023, f, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

std::wstring mmss(double s) {
    if (s < 0) return L"--:--";
    int t = static_cast<int>(s);
    return fmt(L"%02d:%02d", t / 60, t % 60);
}

// ---------------------------------------------------------------- app state
struct EventMark {
    std::string type, detail;
    double t_roast, t;
    double bt;  // C at the event, -1 if the probe was open
};

// Roast phases shown in the progress panel and as a band on the graph.
constexpr int kPhaseCount = 3;
const wchar_t* const kPhaseNames[kPhaseCount] = {L"Drying", L"Maillard", L"Development"};
const COLORREF kPhaseColors[kPhaseCount] = {RGB(118, 168, 82), RGB(220, 150, 50), RGB(150, 92, 52)};

// Events the Mark button steps through, in roast order.
constexpr int kMarkCount = 5;
const char* const kMarkEvents[kMarkCount] = {"DRY", "FCs", "FCe", "SCs", "SCe"};
const wchar_t* const kMarkLabels[kMarkCount] = {L"Mark DRY", L"Mark FC start", L"Mark FC end", L"Mark SC start",
                                                L"Mark SC end"};

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    rs_host* host = nullptr;
    HFONT font = nullptr, mono = nullptr, small = nullptr, heading = nullptr, big = nullptr;
    double dpi = 1.0;
    HACCEL accel = nullptr;

    HWND roasterCombo, beanCombo, batchEdit, batchUnit, readout, statusBar, desc, phasesPanel, markButton;
    std::wstring phasesKey;  // last painted progress-panel content
    int nextMark = -1;       // index into kMarkEvents the Mark button sends, -1 = none
    HWND coolCheck, powerCheck;
    HWND sliders[kSliderCount], sliderLabels[kSliderCount], sliderValues[kSliderCount];
    bool dragging[kSliderCount] = {};
    HMENU menuBar = nullptr, speedMenu = nullptr;
    std::wstring readoutText;
    std::wstring statusText[SB_COUNT];
    int statusH = 0;

    // Section headings painted on the left pane (positions set by layout()).
    struct Heading { int y; const wchar_t* text; };
    std::vector<Heading> headings;

    bool soundOn = true;
    int volume = 75;  // percent

    std::vector<std::string> roasterIds, beanIds;
    Value roasterDef;
    std::string currentRoaster;

    rs_state st{};
    // The graph covers one session: it starts when the machine is switched on
    // (or at CHARGE, if beans go into a machine that is still off) and ends
    // with the roast report (COOL), when the machine switches off. Start and
    // end come from the simulator's own event times, not from when we notice.
    // Session-clock seconds; graphStart < 0 = not started, graphEnd < 0 = still recording.
    double graphStart = -1, graphEnd = -1;
    std::string unitKey;         // detects unit changes made anywhere (menu, web UI, Artisan TC4)
    double preheatC = 200;       // preheat target, kept in C
    bool unitsShown = false;
    std::vector<rs_sample> hist;
    uint64_t histNext = 0;
    int lastPhase = -1;
    std::vector<EventMark> events;
    uint64_t eventSeq = 0;
    rs_audio* audio = nullptr;   // procedural roaster sound (docs/SOUND.md)

    std::vector<rs_visual_bean> vbeans;
    rs_visual_info vinfo{};
    std::vector<float> beanX, beanY, beanPhase;
    int leftWidth = 400;
};
App g;
void applySound();

int S(int v) { return static_cast<int>(std::lround(v * g.dpi)); }

// ---------------------------------------------------------------- units
// The simulator always reports C, grams and metres; the shared unit setting
// (Units menu, saved to roastsim.json) only changes how values are shown/typed.
bool isF() { return g.st.temp_unit == 'F'; }
double T(double c) { return isF() ? c * 1.8 + 32.0 : c; }
double fromT(double v) { return isF() ? (v - 32.0) / 1.8 : v; }
double R(double perMinC) { return isF() ? perMinC * 1.8 : perMinC; }  // rate of rise
const wchar_t* TU() { return isF() ? L"°F" : L"°C"; }
std::string wUnit() { return g.st.weight_unit[0] ? g.st.weight_unit : "g"; }
double W(double grams) {
    std::string u = wUnit();
    if (u == "kg") return grams / 1000.0;
    if (u == "oz") return grams / 28.349523125;
    if (u == "lb") return grams / 453.59237;
    return grams;
}
double fromW(double v) {
    std::string u = wUnit();
    if (u == "kg") return v * 1000.0;
    if (u == "oz") return v * 28.349523125;
    if (u == "lb") return v * 453.59237;
    return v;
}
std::wstring fmtW(double grams) {  // value only, sensible precision per unit
    std::string u = wUnit();
    double v = W(grams);
    if (u == "g") return fmt(L"%.0f", v);
    if (u == "oz") return fmt(L"%.1f", v);
    return fmt(L"%.2f", v);
}
bool isFt() { return std::strcmp(g.st.altitude_unit, "ft") == 0; }
double A(double m) { return isFt() ? m / 0.3048 : m; }
double fromA(double v) { return isFt() ? v * 0.3048 : v; }
std::wstring probeText(double c) { return c == -1.0 ? std::wstring(L"  open") : fmt(L"%6.1f", T(c)); }

// ---------------------------------------------------------------- api helpers
Value command(const Value& cmd) {
    std::string in = cmd.dump();
    std::vector<char> out(1 << 16);
    size_t n = rs_host_command(g.host, in.c_str(), "ui", out.data(), out.size());
    if (n >= out.size()) {
        out.resize(n + 1);
        rs_host_command(g.host, in.c_str(), "ui", out.data(), out.size());
    }
    return Value::parse(out.data());
}

Value cmd1(const char* c) {
    Value v = Value::object();
    v["cmd"] = c;
    return command(v);
}

void showError(const Value& r) {
    if (!r.getBool("ok", true)) MessageBoxW(g.wnd, widen(r.get("error", "failed")).c_str(), L"RoastSim", MB_ICONWARNING);
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

// ---------------------------------------------------------------- readout control
// A read-only multi-line text panel painted through a memory DC, so updating it
// several times a second never flashes.
LRESULT CALLBACK readoutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            FillRect(mem, &rc, GetSysColorBrush(COLOR_WINDOW));
            FrameRect(mem, &rc, GetSysColorBrush(COLOR_3DSHADOW));
            HGDIOBJ oldFont = SelectObject(mem, g.mono);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
            RECT text{rc.left + S(6), rc.top + S(4), rc.right - S(4), rc.bottom - S(2)};
            DrawTextW(mem, g.readoutText.c_str(), -1, &text, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldFont);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void setReadout(const std::wstring& text) {
    if (text == g.readoutText) return;
    g.readoutText = text;
    InvalidateRect(g.readout, nullptr, FALSE);
}

// ---------------------------------------------------------------- roast progress
// Roast time of the first event of a type in the current roast, -1 if none yet.
double eventTime(const char* type) {
    for (const auto& e : g.events)
        if (e.type == type) return e.t_roast;
    return -1;
}

// Everything the progress panel shows, worked out from the roast's events.
// After DROP the numbers freeze at the drop.
struct PhaseView {
    bool roast = false;          // beans charged in this roast
    bool dropped = false;
    double end = 0;              // roast time now, or at DROP
    double start[kPhaseCount] = {-1, -1, -1};  // phase start (roast s), -1 = not reached
    double dur[kPhaseCount] = {-1, -1, -1};
    int active = -1;             // phase in progress, -1 when none (before charge / after drop)
    const wchar_t* sinceName = L"dry end";
    double since = -1;           // seconds since the latest of TP/DRY/FCs/SCs
    int sinceIdx = -1;           // which of them, for the colour
};

PhaseView phaseView() {
    PhaseView v;
    // Events belong to the current graph, so a finished roast stays on show
    // until the machine is switched on again.
    double drop = eventTime("DROP");
    if (eventTime("CHARGE") < 0 || (drop < 0 && g.st.t_roast < 0)) return v;
    v.roast = true;
    v.dropped = drop >= 0;
    v.end = v.dropped ? drop : g.st.t_roast;
    double dry = eventTime("DRY"), fcs = eventTime("FCs");
    v.start[0] = 0;
    v.start[1] = dry;
    v.start[2] = fcs >= 0 && dry >= 0 ? fcs : -1;
    for (int i = 0; i < kPhaseCount; ++i) {
        if (v.start[i] < 0) continue;
        double next = i + 1 < kPhaseCount && v.start[i + 1] >= 0 ? v.start[i + 1] : v.end;
        v.dur[i] = std::max(0.0, next - v.start[i]);
        if (!v.dropped) v.active = i;
    }
    // The newest event wins: second crack replaces first crack, and so on.
    const struct { const char* type; const wchar_t* name; int phase; } steps[] = {
        {"TP", L"turning point", 0}, {"DRY", L"dry end", 1}, {"FCs", L"first crack", 2}, {"SCs", L"second crack", 2}};
    for (const auto& s : steps) {
        double t = eventTime(s.type);
        if (t < 0 || t > v.end) continue;
        v.since = v.end - t;
        v.sinceName = s.name;
        v.sinceIdx = s.phase;
    }
    return v;
}

std::wstring pctText(const PhaseView& v, int i) {
    return v.dur[i] >= 0 && v.end > 0 ? fmt(L"%.0f%%", v.dur[i] / v.end * 100.0) : std::wstring();
}

void drawText(HDC dc, HFONT f, COLORREF c, int x, int y, const std::wstring& s) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

int textWidth(HDC dc, HFONT f, const std::wstring& s) {
    SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    return sz.cx;
}

void paintPhases(HDC dc, RECT rc) {
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &rc, GetSysColorBrush(COLOR_3DSHADOW));
    SetBkMode(dc, TRANSPARENT);
    const PhaseView v = phaseView();
    const COLORREF text = GetSysColor(COLOR_WINDOWTEXT), grey = RGB(128, 122, 115);
    const int pad = S(10), w = rc.right - rc.left, half = w / 2;

    // Top: roast time and time since the latest event, both large.
    drawText(dc, g.small, grey, pad, S(6), v.dropped ? L"Roast time (dropped)" : L"Roast time");
    drawText(dc, g.big, text, pad, S(20), v.roast ? mmss(v.end) : L"--:--");
    std::wstring cap = std::wstring(L"Since ") + v.sinceName + (v.dropped && v.since >= 0 ? L" (to drop)" : L"");
    drawText(dc, g.small, grey, half, S(6), cap);
    COLORREF sinceColor = v.sinceIdx >= 0 ? kPhaseColors[v.sinceIdx] : grey;
    drawText(dc, g.big, v.since >= 0 ? sinceColor : grey, half, S(20), v.since >= 0 ? mmss(v.since) : L"--:--");

    // Bottom: one cell per phase with a colour bar, duration and share of the roast.
    const int top = S(64), cellW = (w - 2 * pad) / kPhaseCount;
    for (int i = 0; i < kPhaseCount; ++i) {
        int x = pad + i * cellW;
        bool reached = v.dur[i] >= 0;
        HBRUSH br = CreateSolidBrush(reached ? kPhaseColors[i] : RGB(225, 222, 215));
        RECT bar{x, top, x + cellW - S(8), top + (i == v.active ? S(5) : S(3))};
        FillRect(dc, &bar, br);
        DeleteObject(br);
        drawText(dc, g.small, i == v.active ? text : grey, x, top + S(8), kPhaseNames[i]);
        std::wstring d = reached ? mmss(v.dur[i]) : L"--:--";
        drawText(dc, g.heading, reached ? text : grey, x, top + S(24), d);
        if (reached)
            drawText(dc, g.font, grey, x + textWidth(dc, g.heading, d) + S(6), top + S(24), pctText(v, i));
    }
}

LRESULT CALLBACK phasesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            HGDIOBJ oldFont = SelectObject(mem, g.font);
            paintPhases(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldFont);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Repaints the panel only when something it shows has changed.
void updatePhases() {
    PhaseView v = phaseView();
    std::wstring key = fmt(L"%d%d%d%d|%s|%s|", v.roast, v.dropped, v.active, v.sinceIdx, mmss(v.end).c_str(),
                           mmss(v.since).c_str()) + v.sinceName;
    for (int i = 0; i < kPhaseCount; ++i) key += L"|" + mmss(v.dur[i]) + pctText(v, i);
    if (key == g.phasesKey) return;
    g.phasesKey = key;
    InvalidateRect(g.phasesPanel, nullptr, FALSE);
}

// The Mark button always offers the next event of the roast not yet marked
// (by you, Artisan or the simulator's own detection).
void updateMarkButton() {
    int next = -1;
    for (int i = 0; i < kMarkCount && next < 0; ++i)
        if (eventTime(kMarkEvents[i]) < 0) next = i;
    bool roasting = g.st.phase == RS_PHASE_ROASTING;
    setText(g.markButton, next >= 0 ? kMarkLabels[next] : L"All marked");
    bool enable = roasting && next >= 0;
    if ((IsWindowEnabled(g.markButton) != 0) != enable) EnableWindow(g.markButton, enable);
    g.nextMark = roasting ? next : -1;
}

void setStatus(int part, const std::wstring& text) {
    if (g.statusText[part] == text) return;
    g.statusText[part] = text;
    SendMessageW(g.statusBar, SB_SETTEXTW, static_cast<WPARAM>(part), reinterpret_cast<LPARAM>(text.c_str()));
}

// ---------------------------------------------------------------- roaster setup
// After a cold reset or roaster load (both clear the simulator's history):
// drop the graph; the next refresh starts a new one if the machine is on.
void reloadHistory() {
    g.hist.clear();
    g.histNext = 0;
    g.events.clear();
    g.eventSeq = 0;
    g.graphStart = g.graphEnd = -1;
}

// Begins a new graph at session time t (-1 = none until the machine is on).
void startGraph(double t) {
    g.graphStart = t;
    g.graphEnd = -1;
    g.events.clear();
    if (t < 0) return;
    auto keep = std::find_if(g.hist.begin(), g.hist.end(), [t](const rs_sample& s) { return s.t >= t - 1.0; });
    g.hist.erase(g.hist.begin(), keep);  // samples of earlier graphs
}

void invalidateRight() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    rc.left = S(g.leftWidth);
    InvalidateRect(g.wnd, &rc, FALSE);
}

void applyRoasterDef() {
    Value st = Value::object();
    st["cmd"] = "state";
    st["full"] = true;
    Value s = command(st);
    g.roasterDef = s["roaster_def"];
    g.currentRoaster = s["roaster"].get("id", "");
    setText(g.desc, widen(g.roasterDef.get("description", "")));
    for (int i = 0; i < kSliderCount; ++i) {
        ShowWindow(g.sliders[i], SW_HIDE);
        ShowWindow(g.sliderLabels[i], SW_HIDE);
        ShowWindow(g.sliderValues[i], SW_HIDE);
    }
    const char* names[] = {"heat", "fan", "drum", "stir", "voltage"};
    for (const auto& c : g.roasterDef["controls"].items()) {
        std::string id = c.get("id", "");
        for (int i = 0; i < kSliderCount; ++i) {
            if (id != names[i]) continue;
            SendMessageW(g.sliders[i], TBM_SETRANGE, TRUE,
                         MAKELPARAM(static_cast<int>(c.get("min", 0.0)), static_cast<int>(c.get("max", 100.0))));
            SendMessageW(g.sliders[i], TBM_SETPAGESIZE, 0, 10);
            setText(g.sliderLabels[i], widen(c.get("label", id.c_str())));
            ShowWindow(g.sliders[i], SW_SHOW);
            ShowWindow(g.sliderLabels[i], SW_SHOW);
            ShowWindow(g.sliderValues[i], SW_SHOW);
        }
    }
    SetWindowTextW(g.batchEdit, fmtW(s.get("next_batch_g", 100.0)).c_str());
    for (size_t i = 0; i < g.roasterIds.size(); ++i)
        if (g.roasterIds[i] == g.currentRoaster) SendMessageW(g.roasterCombo, CB_SETCURSEL, i, 0);
    std::string bean = s["bean"].get("id", "");
    for (size_t i = 0; i < g.beanIds.size(); ++i)
        if (g.beanIds[i] == bean) SendMessageW(g.beanCombo, CB_SETCURSEL, i, 0);
    reloadHistory();
    g.beanX.clear();
    invalidateRight();
}

void populateLists() {
    Value l = cmd1("list");
    SendMessageW(g.roasterCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.beanCombo, CB_RESETCONTENT, 0, 0);
    g.roasterIds.clear();
    g.beanIds.clear();
    for (const auto& r : l["roasters"].items()) {
        g.roasterIds.push_back(r.get("id", ""));
        SendMessageW(g.roasterCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(r.get("name", "")).c_str()));
    }
    for (const auto& b : l["beans"].items()) {
        g.beanIds.push_back(b.get("id", ""));
        SendMessageW(g.beanCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(b.get("name", "")).c_str()));
    }
}

// Relabels and refills every unit-dependent control (called when the unit
// setting changes, from here or from another front end).
void applyUnits() {
    setText(g.batchUnit, widen(wUnit()));
    SetWindowTextW(g.batchEdit, fmtW(g.st.next_batch_g > 0 ? g.st.next_batch_g : g.st.batch_g).c_str());
    invalidateRight();
}

int speedIndex() {
    if (g.st.paused) return 0;
    int sel = 1;
    for (int i = 1; i < kSpeedCount; ++i)
        if (std::fabs(kSpeeds[i] - g.st.time_scale) < 0.01) sel = i;
    return sel;
}

std::wstring envSummary() {
    return fmt(L"%.0f %s · %.0f V · %.0f %s", T(g.st.ambient_c), TU(), g.st.line_voltage, A(g.st.altitude_m),
               isFt() ? L"ft" : L"m");
}

// Menus show live state (checks, radio marks, current environment values);
// refreshed whenever a menu is about to open.
void updateMenus() {
    HMENU m = g.menuBar;
    std::string w = wUnit();
    CheckMenuRadioItem(m, IDM_TEMP_C, IDM_TEMP_F, isF() ? IDM_TEMP_F : IDM_TEMP_C, MF_BYCOMMAND);
    int wid = w == "kg" ? IDM_W_KG : w == "oz" ? IDM_W_OZ : w == "lb" ? IDM_W_LB : IDM_W_G;
    CheckMenuRadioItem(m, IDM_W_G, IDM_W_LB, wid, MF_BYCOMMAND);
    CheckMenuRadioItem(m, IDM_ALT_M, IDM_ALT_FT, isFt() ? IDM_ALT_FT : IDM_ALT_M, MF_BYCOMMAND);
    CheckMenuRadioItem(m, IDM_SPEED0, IDM_SPEED0 + kSpeedCount - 1, IDM_SPEED0 + speedIndex(), MF_BYCOMMAND);
    int vi = 0;
    for (int i = 0; i < kVolumeCount; ++i)
        if (kVolumes[i] == g.volume) vi = i;
    CheckMenuRadioItem(m, IDM_VOL0, IDM_VOL0 + kVolumeCount - 1, IDM_VOL0 + vi, MF_BYCOMMAND);
    CheckMenuItem(m, IDM_SOUND, MF_BYCOMMAND | (g.soundOn ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_POWER, MF_BYCOMMAND | (g.st.powered ? MF_CHECKED : MF_UNCHECKED));
    // Each event can be marked once per roast; marked ones show a check.
    for (int i = 0; i < kMarkCount; ++i) {
        bool done = eventTime(kMarkEvents[i]) >= 0;
        CheckMenuItem(m, IDM_MARK_DRY + i, MF_BYCOMMAND | (done ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(m, IDM_MARK_DRY + i,
                       MF_BYCOMMAND | (!done && g.st.phase == RS_PHASE_ROASTING ? MF_ENABLED : MF_GRAYED));
    }
    auto relabel = [&](int id, const std::wstring& text) {
        ModifyMenuW(m, id, MF_BYCOMMAND | MF_STRING, id, text.c_str());
    };
    relabel(IDM_ENV_AMBIENT, fmt(L"Room temperature:  %.0f %s", T(g.st.ambient_c), TU()));
    relabel(IDM_ENV_VOLTS, fmt(L"Mains voltage:  %.0f V", g.st.line_voltage));
    relabel(IDM_ENV_ALT, fmt(L"Altitude:  %.0f %s", A(g.st.altitude_m), isFt() ? L"ft" : L"m"));
}

// ---------------------------------------------------------------- layout
void layoutStatusBar() {
    SendMessageW(g.statusBar, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g.statusBar, &sb);
    g.statusH = sb.bottom - sb.top;
    RECT rc;
    GetClientRect(g.wnd, &rc);
    // Fixed widths for the first parts, the connection parts share the rest.
    const int fixed[] = {S(170), S(150), S(190)};
    int edges[SB_COUNT], x = 0;
    for (int i = 0; i < 3; ++i) edges[i] = (x += fixed[i]);
    int rest = std::max(S(300), static_cast<int>(rc.right) - x);
    edges[SB_MODBUS] = x + rest * 3 / 10;
    edges[SB_WEB] = x + rest * 7 / 10;
    edges[SB_LOGGER] = -1;
    SendMessageW(g.statusBar, SB_SETPARTS, SB_COUNT, reinterpret_cast<LPARAM>(edges));
}

void layout() {
    layoutStatusBar();
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int bottom = rc.bottom - g.statusH;
    const int pad = S(10), w = S(g.leftWidth) - 2 * pad, rowH = S(24), gap = S(6), headH = S(24);
    int y = S(6);
    auto place = [&](HWND h, int x, int yy, int ww, int hh) { MoveWindow(h, x, yy, ww, hh, FALSE); };
    g.headings.clear();
    auto heading = [&](const wchar_t* text) {
        g.headings.push_back({y, text});
        y += headH;
    };

    heading(L"Roaster and beans");
    place(g.roasterCombo, pad, y, w, S(300));
    y += rowH + gap;
    place(g.beanCombo, pad, y, w, S(300));
    y += rowH + gap;
    place(g.desc, pad, y, w, S(44));
    y += S(44) + gap;

    heading(L"Roast");
    const int editW = S(56), unitW = S(26);
    int bx = pad + editW + S(4) + unitW + gap;
    int bw = (pad + w - bx - 2 * gap) / 3;
    place(g.batchEdit, pad, y, editW, rowH);
    place(g.batchUnit, pad + editW + S(4), y + S(4), unitW, rowH - S(4));
    place(GetDlgItem(g.wnd, IDC_CHARGE), bx, y, bw, rowH);
    place(GetDlgItem(g.wnd, IDC_DROP), bx + bw + gap, y, bw, rowH);
    place(GetDlgItem(g.wnd, IDC_MARKFC), bx + 2 * (bw + gap), y, pad + w - (bx + 2 * (bw + gap)), rowH);
    y += rowH + gap;
    place(g.powerCheck, pad, y, S(110), rowH);
    place(g.coolCheck, pad + S(120), y, w - S(120), rowH);
    y += rowH + gap;

    heading(L"Roast progress");
    place(g.phasesPanel, pad, y, w, S(118));
    y += S(118) + gap * 2;

    bool anySlider = false;
    for (int i = 0; i < kSliderCount; ++i) anySlider |= IsWindowVisible(g.sliders[i]) != 0;
    if (anySlider) heading(L"Controls");
    for (int i = 0; i < kSliderCount; ++i) {
        if (!IsWindowVisible(g.sliders[i])) continue;
        place(g.sliderLabels[i], pad, y + S(4), S(90), rowH);
        place(g.sliders[i], pad + S(92), y, w - S(92) - S(66), S(30));
        place(g.sliderValues[i], pad + w - S(62), y + S(4), S(62), rowH);
        y += S(32);
    }
    y += gap;

    heading(L"Readings");
    place(g.readout, pad, y, w, std::max(S(60), bottom - y - pad));
    InvalidateRect(g.wnd, nullptr, TRUE);  // resize/relayout only: repaint everything once
}

// ---------------------------------------------------------------- refresh
void syncSliders() {
    for (int i = 0; i < kSliderCount; ++i) {
        if (!IsWindowVisible(g.sliders[i])) continue;
        double v = g.st.controls[kSliderControls[i]];
        if (!g.dragging[i] && GetFocus() != g.sliders[i] &&
            SendMessageW(g.sliders[i], TBM_GETPOS, 0, 0) != std::lround(v))
            SendMessageW(g.sliders[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(v)));
        double act = g.st.actuators[kSliderControls[i]];
        setText(g.sliderValues[i], std::fabs(act - v) > 0.5 ? fmt(L"%.0f→%.0f", act, v) : fmt(L"%.0f%%", v));
    }
    LRESULT cool = g.st.controls[RS_CTL_COOLING] >= 50 ? BST_CHECKED : BST_UNCHECKED;
    if (SendMessageW(g.coolCheck, BM_GETCHECK, 0, 0) != cool) SendMessageW(g.coolCheck, BM_SETCHECK, cool, 0);
    LRESULT pwr = g.st.powered ? BST_CHECKED : BST_UNCHECKED;
    if (SendMessageW(g.powerCheck, BM_GETCHECK, 0, 0) != pwr) SendMessageW(g.powerCheck, BM_SETCHECK, pwr, 0);
}

std::wstring alarmText(uint32_t a) {
    static const struct { uint32_t bit; const wchar_t* name; } names[] = {
        {1u << 0, L"thermostat open"}, {1u << 1, L"FUSE BLOWN"}, {1u << 2, L"beans ejecting"},
        {1u << 3, L"bed stalled"}, {1u << 4, L"scorching"}, {1u << 5, L"heavy smoke"},
        {1u << 6, L"FIRE RISK"}, {1u << 7, L"overheat"}, {1u << 8, L"centrifuging"}, {1u << 9, L"probe fault"},
    };
    std::wstring s;
    for (const auto& n : names)
        if (a & n.bit) s += (s.empty() ? L"" : L", ") + std::wstring(n.name);
    return s.empty() ? L"none" : s;
}

void showReport() {
    Value r = cmd1("report");
    if (!r.getBool("valid", false)) {
        MessageBoxW(g.wnd, L"No roast to report yet. Charge some beans first.", L"Roast report", MB_ICONINFORMATION);
        return;
    }
    std::wstring wu = widen(wUnit());
    std::wstring t;
    t += widen(r.get("roaster", "")) + L"\n" + widen(r.get("bean", "")) + L"\n\n";
    t += L"Batch " + fmtW(r.get("batch_g", 0.0)) + L" " + wu + L"  ->  " + fmtW(r.get("out_g", 0.0)) + L" " + wu +
         fmt(L"   (loss %.1f%%, moisture %.1f%%)\n", r.get("weight_loss_pct", 0.0), r.get("moisture_pct", 0.0));
    t += L"TP " + mmss(r.get("tp_s", -1.0)) + fmt(L" at %.0f%s", T(r.get("tp_bt", 0.0)), TU()) + L"   DRY " +
         mmss(r.get("dry_s", -1.0)) + L"\n";
    t += L"FCs " + mmss(r.get("fcs_s", -1.0)) + fmt(L" at %.0f%s", T(r.get("fcs_bt", 0.0)), TU()) + L"   DROP " +
         mmss(r.get("drop_s", -1.0)) + fmt(L" at %.0f%s\n", T(r.get("drop_bt", 0.0)), TU());
    t += fmt(L"Development %.0f s  (DTR %.1f%%)\n", r.get("dev_time_s", 0.0), r.get("dtr_pct", 0.0));
    t += fmt(L"Agtron whole %.1f / ground %.1f  (spread %.1f)\n", r.get("agtron_whole", 0.0), r.get("agtron_ground", 0.0),
             r.get("agtron_sd", 0.0));
    t += L"Roast level: " + widen(r.get("roast_level", "")) + L"\n";
    t += fmt(L"RoR max %.1f, at FC %.1f, at drop %.1f %s/min\n\n", R(r.get("max_ror", 0.0)), R(r.get("ror_at_fcs", 0.0)),
             R(r.get("ror_at_drop", 0.0)), TU());
    for (const auto& n : r["notes"].items()) t += L"• " + widen(n.str()) + L"\n";
    MessageBoxW(g.wnd, t.c_str(), L"Roast report", MB_ICONINFORMATION);
}

std::wstring buildReadout() {
    const rs_state& s = g.st;
    // Roast time and phase are in the progress panel and status bar.
    const wchar_t* tu = TU();
    std::wstring r;
    r += L"BT " + probeText(s.bt) + L" " + tu + fmt(L"   RoR %6.1f %s/min\r\n", R(s.ror_bt), tu);
    r += L"ET " + probeText(s.et) + L" " + tu + fmt(L"   RoR %6.1f %s/min\r\n", R(s.ror_et), tu);
    r += L"\r\n";
    if (s.bean_location != 0) {
        r += fmt(L"Beans  mean %.1f  surface %.1f  core %.1f\r\n", T(s.bean_mean_c), T(s.bean_surface_c), T(s.bean_core_c));
        r += fmt(L"       spread %.1f – %.1f %s\r\n", T(s.bean_min_c), T(s.bean_max_c), tu);
        double out = s.batch_g * (1.0 - s.weight_loss_pct / 100.0);
        r += fmt(L"Moisture %.2f%%   loss %.1f%%   ", s.moisture_pct, s.weight_loss_pct) + fmtW(out) + L" " +
             widen(wUnit()) + L"\r\n";
        r += fmt(L"Agtron %.0f whole / %.0f ground (±%.1f)\r\n", s.agtron_whole, s.agtron_ground, s.agtron_sd);
        r += L"Level  " + widen(s.roast_level) + fmt(L"   size ×%.2f\r\n", s.expansion);
        r += fmt(L"First crack %.0f%%   second %.0f%%\r\n", s.fc_frac * 100, s.sc_frac * 100);
        if (s.scorch_frac > 0.005 || s.tipping_frac > 0.005)
            r += fmt(L"Scorched %.0f%%   tipped %.0f%%\r\n", s.scorch_frac * 100, s.tipping_frac * 100);
    } else {
        r += L"Beans  none\r\n";
    }
    r += L"\r\n";
    if (!s.powered) r += L"Power  OFF - heater, fan and motor do nothing\r\n";
    r += L"Bed    " + widen(s.bed_state) + fmt(L"   agitation %.2f   mixing %.0f%%\r\n", s.agitation, s.mix_quality * 100);
    if (s.bean_location == 1) r += fmt(L"       fill %.0f%%   BT probe buried %.0f%%\r\n", s.fill * 100, s.bt_immersion * 100);
    if (s.drum_rpm > 0.1) r += fmt(L"Drum   %.0f rpm\r\n", s.drum_rpm);
    if (s.stir_rpm > 0.1) r += fmt(L"Stir   %.1f rpm\r\n", s.stir_rpm);
    r += fmt(L"Heater %.0f W", s.heater_w) + (s.voltage_v > 1 ? fmt(L" @ %.0f V", s.voltage_v) : std::wstring()) +
         fmt(L"   air %.2f g/s\r\n", s.airflow_g_s);
    r += fmt(L"Inlet %.0f   exhaust %.0f %s\r\n", T(s.inlet_c), T(s.exhaust_c), tu);
    r += fmt(L"Wall %.0f   element %.0f %s\r\n", T(s.wall_c), T(s.element_c), tu);
    r += fmt(L"Smoke %.0f%%   chaff %.2f g/s\r\n", s.smoke * 100, s.chaff_g_s);
    r += L"Alarms: " + alarmText(s.alarms) + L"\r\n";
    return r;
}

void refresh() {
    g.st.struct_size = sizeof(rs_state);
    if (!rs_host_get_state(g.host, &g.st)) return;
    std::string key = std::string(1, g.st.temp_unit) + g.st.weight_unit + "|" + g.st.altitude_unit;
    if (key != g.unitKey || !g.unitsShown) {
        g.unitKey = key;
        g.unitsShown = true;
        applyUnits();
    }
    // History: the shell keeps its own copy of every sample since power-on (the
    // simulator trims pre-charge history at CHARGE). Reload only when the
    // simulator's history was cleared (cold reset or roaster load, from any front end).
    if (g.st.history_end < g.histNext) {
        g.hist.clear();
        g.histNext = 0;
        g.graphStart = g.graphEnd = -1;
    }
    rs_sample buf[512];
    uint64_t next = g.histNext;
    for (;;) {
        int n = rs_host_get_history(g.host, next, buf, 512, &next);
        if (n <= 0) break;
        g.hist.insert(g.hist.end(), buf, buf + n);
    }
    g.histNext = next;
    if (g.hist.size() > 20000) g.hist.erase(g.hist.begin(), g.hist.begin() + 5000);

    if (g.st.event_seq != g.eventSeq) {
        Value c = Value::object();
        c["cmd"] = "events";
        c["since"] = static_cast<double>(g.eventSeq);
        Value r = command(c);
        for (const auto& e : r["events"].items()) {
            std::string type = e.get("type", "");
            double t = e.get("t", 0.0);
            // A cold reset or roaster load (from any front end) wipes the
            // simulator's history: start a new graph if the machine is on.
            std::string detail = e.get("detail", "");
            if (type == "NOTE" && detail == "cold reset") {
                if (g.graphStart < 0 || t > g.graphStart) startGraph(g.st.powered ? t : -1);
                continue;
            }
            // Switching on starts a new graph at the moment of the switch,
            // unless a roast is in progress on the current one (a mid-roast
            // power cut keeps the graph going).
            if (type == "NOTE" && detail == "power on" &&
                (g.graphStart < 0 || g.graphEnd >= 0 || eventTime("CHARGE") < 0))
                startGraph(t);
            // Charging a machine that is still switched off starts the graph
            // at CHARGE, so switching on afterwards continues that roast.
            if (type == "CHARGE" && (g.graphStart < 0 || g.graphEnd >= 0)) startGraph(t);
            // Only events of the current graph (a refetch can replay older ones).
            if (g.graphStart < 0 || t < g.graphStart || g.graphEnd >= 0) continue;
            if (type == "CHARGE") g.events.clear();
            g.events.push_back({type, e.get("detail", ""), e.get("t_roast", -1.0), t, e.get("bt", -1.0)});
            if (type == "COOL") {
                // The roast is over: freeze the graph here, then power off and report.
                g.graphEnd = t;
                PostMessageW(g.wnd, WM_APP_ROAST_DONE, 0, 0);
            }
        }
        g.eventSeq = g.st.event_seq;
    }
    // Machine on but no graph (switched on before the last cold reset, or
    // before this window opened): the graph starts with the oldest sample.
    if (g.st.powered && g.graphStart < 0) startGraph(g.hist.empty() ? g.st.t_session : g.hist.front().t);

    syncSliders();
    updatePhases();
    updateMarkButton();
    setReadout(buildReadout());

    const wchar_t* phases[] = {L"empty", L"roasting", L"cooling", L"done"};
    setStatus(SB_STATE, g.st.powered ? fmt(L"Power on · %s", phases[std::clamp(g.st.phase, 0, 3)]) : L"POWER OFF");
    std::wstring speed = g.st.paused ? L"Paused" : g.st.time_scale == 1.0 ? L"Real time" : fmt(L"Speed %gx", g.st.time_scale);
    if (g.st.logger_connected && !g.st.paused) speed = L"Real time (logger)";
    setStatus(SB_SPEED, speed + L"  ▾");
    setStatus(SB_ENV, envSummary());

    static int slow = 0;
    if (++slow % 5 == 1) {
        // Another front end (web UI, Artisan, console) may have loaded a different roaster.
        Value cur = cmd1("state");
        if (cur["roaster"].get("id", "") != g.currentRoaster) {
            applyRoasterDef();
            layout();
        }
        Value info = cmd1("server");
        setStatus(SB_MODBUS, L"Modbus " + widen(info["net"].get("modbus", "off")));
        setStatus(SB_WEB, L"Web/WS " + widen(info["net"].get("http", "off")));
        setStatus(SB_LOGGER, g.st.logger_connected ? L"Logger connected" : L"No logger");
    }

    g.vinfo.struct_size = sizeof(rs_visual_info);
    g.vbeans.resize(400);
    int nb = rs_host_get_visual(g.host, &g.vinfo, g.vbeans.data(), 400);
    g.vbeans.resize(static_cast<size_t>(std::max(0, nb)));
    invalidateRight();
}

// ---------------------------------------------------------------- painting
void paintGraph(HDC dc, RECT area) {
    HBRUSH bg = CreateSolidBrush(RGB(250, 249, 246));
    FillRect(dc, &area, bg);
    DeleteObject(bg);
    SelectObject(dc, g.small);
    SetBkMode(dc, TRANSPARENT);

    const int l = area.left + S(48), r = area.right - S(52), t = area.top + S(26), b = area.bottom - S(26);
    if (r <= l + 20 || b <= t + 20) return;
    // The graph runs from power-on (graphStart) to the roast report (graphEnd).
    // Before CHARGE the axis is time since power-on; from CHARGE on it is roast
    // time, with the pre-charge part (preheat) at negative times.
    const bool started = g.graphStart >= 0;
    const double tEnd = g.graphEnd >= 0 ? g.graphEnd : g.st.t_session;  // session clock
    double chargeAt = -1;  // session time of CHARGE in this graph
    for (const auto& e : g.events)
        if (e.type == "CHARGE") chargeAt = e.t;
    const bool roast = started && chargeAt >= 0;
    const double origin = roast ? chargeAt : g.graphStart;
    auto inGraph = [&](double ts) { return started && ts >= g.graphStart - 0.5 && ts <= tEnd + 0.5; };
    auto tx = [&](const rs_sample& s) { return static_cast<double>(s.t) - origin; };
    double x0 = 0, x1 = 5 * 60.0;
    if (roast) {
        x0 = std::min(0.0, std::floor((g.graphStart - chargeAt) / 30.0) * 30.0);
        x1 = std::max(12.0 * 60.0, std::ceil((tEnd - chargeAt + 60) / 120.0) * 120.0);
    } else if (started) {
        x1 = std::max(x1, std::ceil((tEnd - g.graphStart + 30) / 60.0) * 60.0);
    }
    // Axes in display units: C 0-250 by 50 / F 0-500 by 100; RoR 0-50 C/min / 0-90 F/min.
    const double tStep = isF() ? 100.0 : 50.0, rorMax = isF() ? 90.0 : 50.0, rorStep = isF() ? 15.0 : 10.0;
    double tMax = isF() ? 500.0 : 250.0;
    for (const auto& s : g.hist)
        if (inGraph(s.t)) tMax = std::max<double>(tMax, std::max(T(s.bt), T(s.et)) + 10);
    tMax = std::ceil(tMax / tStep) * tStep;
    auto X = [&](double x) { return l + static_cast<int>((x - x0) / (x1 - x0) * (r - l)); };
    auto Y = [&](double v) { return b - static_cast<int>(v / tMax * (b - t)); };
    auto YR = [&](double v) { return b - static_cast<int>(std::clamp(v, -5.0, rorMax) / rorMax * (b - t)); };

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(225, 222, 215));
    HGDIOBJ oldPen = SelectObject(dc, grid);
    SetTextColor(dc, RGB(110, 105, 95));
    for (double v = 0; v <= tMax; v += tStep) {
        MoveToEx(dc, l, Y(v), nullptr);
        LineTo(dc, r, Y(v));
        std::wstring s = fmt(L"%.0f", v);
        TextOutW(dc, area.left + S(8), Y(v) - S(7), s.c_str(), static_cast<int>(s.size()));
    }
    for (double v = 0; v <= rorMax; v += rorStep) {
        std::wstring s = fmt(L"%.0f", v);
        TextOutW(dc, r + S(6), YR(v) - S(7), s.c_str(), static_cast<int>(s.size()));
    }
    TextOutW(dc, area.left + S(8), t - S(18), TU(), static_cast<int>(wcslen(TU())));
    std::wstring rorUnit = std::wstring(TU()) + L"/min";
    TextOutW(dc, r - S(4), t - S(18), rorUnit.c_str(), static_cast<int>(rorUnit.size()));
    // Time grid: the smallest step that keeps the labels from crowding.
    double step = 600;
    for (double s : {30.0, 60.0, 120.0, 300.0})
        if ((x1 - x0) / s <= (r - l) / S(50)) { step = s; break; }
    for (double x = std::ceil(x0 / step) * step; x <= x1; x += step) {
        MoveToEx(dc, X(x), t, nullptr);
        LineTo(dc, X(x), b);
        std::wstring s = x < 0 ? L"-" + mmss(-x) : mmss(x);
        TextOutW(dc, X(x) - S(14), b + S(6), s.c_str(), static_cast<int>(s.size()));
    }
    SelectObject(dc, oldPen);
    DeleteObject(grid);

    if (!started) {
        const wchar_t* msg = L"Switch the machine on (Power) to start the graph";
        drawText(dc, g.heading, RGB(140, 132, 122), (l + r - textWidth(dc, g.heading, msg)) / 2, (t + b) / 2 - S(10), msg);
        SelectObject(dc, g.small);
    }

    HBRUSH heatBrush = CreateSolidBrush(RGB(244, 214, 190));
    int colW = std::max(1, static_cast<int>(std::ceil((r - l) / (x1 - x0))) + 1);
    for (const auto& s : g.hist) {
        double x = tx(s);
        if (!inGraph(s.t) || x < x0 || x > x1) continue;
        RECT bar{X(x), b - static_cast<int>(s.heat / 100.0 * (b - t) * 0.18), X(x) + colW, b};
        FillRect(dc, &bar, heatBrush);
    }
    DeleteObject(heatBrush);

    auto series = [&](COLORREF c, int width, auto get, bool ror) {
        HPEN p = CreatePen(PS_SOLID, width, c);
        HGDIOBJ old = SelectObject(dc, p);
        bool first = true;
        for (const auto& s : g.hist) {
            double x = tx(s);
            if (!inGraph(s.t) || x < x0 || x > x1) { first = true; continue; }
            double v = get(s);
            if (!ror && v <= -0.5) { first = true; continue; }
            int py = ror ? YR(R(v)) : Y(T(v));
            if (first) MoveToEx(dc, X(x), py, nullptr);
            else LineTo(dc, X(x), py);
            first = false;
        }
        SelectObject(dc, old);
        DeleteObject(p);
    };
    series(RGB(200, 60, 40), S(2), [](const rs_sample& s) { return static_cast<double>(s.et); }, false);
    series(RGB(70, 110, 200), S(2), [](const rs_sample& s) { return static_cast<double>(s.bt); }, false);
    if (roast) {
        series(RGB(230, 150, 40), 1, [](const rs_sample& s) { return s.t_roast > 20 ? static_cast<double>(s.ror_bt) : -5.0; }, true);
        // No beans before CHARGE: skip (-1) rather than drawing 0 C (= 32 F).
        series(RGB(150, 120, 90), 1, [](const rs_sample& s) { return s.t_roast >= 0 ? static_cast<double>(s.bean_true) : -1.0; }, false);
    }

    // Phase band along the top of the plot: drying, Maillard, development.
    const PhaseView pv = phaseView();
    if (roast && pv.roast) {
        for (int i = 0; i < kPhaseCount; ++i) {
            if (pv.dur[i] < 0) continue;
            int a = std::max(l, X(pv.start[i])), z = std::min(r, X(pv.start[i] + pv.dur[i]));
            if (z <= a) continue;
            HBRUSH br = CreateSolidBrush(kPhaseColors[i]);
            RECT band{a, t, z, t + S(5)};
            FillRect(dc, &band, br);
            DeleteObject(br);
        }
    }

    // Event lines, each labelled with its roast time and BT so nothing has to
    // be read off the axes. Labels go in the first row where they don't
    // overlap the previous label.
    HPEN ev = CreatePen(PS_DOT, 1, RGB(90, 80, 70));
    oldPen = SelectObject(dc, ev);
    constexpr int kRows = 4;
    int rowEnd[kRows];
    std::fill(rowEnd, rowEnd + kRows, INT_MIN);
    HBRUSH labelBg = CreateSolidBrush(RGB(250, 249, 246));
    for (const auto& e : g.events) {
        if (e.type == "ALARM" || e.type == "NOTE") continue;
        double x = e.t - origin;
        if (!inGraph(e.t) || x < x0 || x > x1) continue;
        int px = X(x);
        MoveToEx(dc, px, t + S(5), nullptr);
        LineTo(dc, px, b);
        std::wstring name = widen(e.type), detail = x < 0 ? L"-" + mmss(-x) : mmss(x);
        if (e.bt != -1.0 && e.type != "COOL") detail += fmt(L"  %.0f%s", T(e.bt), TU());
        int nameW = textWidth(dc, g.heading, name), w = nameW + S(4) + textWidth(dc, g.small, detail);
        int row = 0;
        for (int i = 0; i < kRows; ++i)
            if (rowEnd[i] < px) { row = i; break; } else if (rowEnd[i] < rowEnd[row]) row = i;
        // Keep labels near the right edge inside the plot.
        int lx = std::min(px + S(4), r - w - S(2));
        int ly = t + S(9) + row * S(17);
        RECT bgr{lx - S(2), ly, lx + w + S(2), ly + S(16)};
        FillRect(dc, &bgr, labelBg);
        drawText(dc, g.heading, RGB(60, 50, 40), lx, ly, name);
        drawText(dc, g.small, RGB(95, 88, 80), lx + nameW + S(4), ly + S(1), detail);
        rowEnd[row] = lx + w + S(6);
    }
    DeleteObject(labelBg);
    SelectObject(dc, g.small);
    SelectObject(dc, oldPen);
    DeleteObject(ev);

    struct L { COLORREF c; const wchar_t* n; } leg[] = {{RGB(70, 110, 200), L"BT"}, {RGB(200, 60, 40), L"ET"},
                                                      {RGB(230, 150, 40), L"RoR (right axis)"},
                                                      {RGB(150, 120, 90), L"true bean temp"}};
    int lx = l + S(30);
    for (const auto& it : leg) {
        HBRUSH br = CreateSolidBrush(it.c);
        RECT sw{lx, area.top + S(8), lx + S(12), area.top + S(18)};
        FillRect(dc, &sw, br);
        DeleteObject(br);
        SetTextColor(dc, RGB(60, 55, 50));
        TextOutW(dc, lx + S(16), area.top + S(5), it.n, static_cast<int>(wcslen(it.n)));
        lx += S(28) + static_cast<int>(wcslen(it.n)) * S(7);
    }
}

void paintBeans(HDC dc, RECT area) {
    HBRUSH bg = CreateSolidBrush(RGB(236, 232, 224));
    FillRect(dc, &area, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.small);
    SetTextColor(dc, RGB(90, 80, 70));
    std::wstring caption = g.vinfo.bean_location == 2   ? L"Beans (cooling tray)"
                           : g.vinfo.bean_location == 1 ? L"Beans in the roaster: " + widen(g.st.bed_state) +
                                                              L" (one dot per parcel; preview of the future 3D view)"
                                                        : L"No beans";
    TextOutW(dc, area.left + S(8), area.top + S(4), caption.c_str(), static_cast<int>(caption.size()));
    size_t n = g.vbeans.size();
    if (n == 0) return;
    if (g.beanX.size() != n) {
        g.beanX.resize(n);
        g.beanY.resize(n);
        g.beanPhase.resize(n);
        unsigned x = 1234567u;
        for (size_t i = 0; i < n; ++i) {
            x = x * 1664525u + 1013904223u;
            g.beanX[i] = (x >> 8) / 16777216.0f;
            x = x * 1664525u + 1013904223u;
            g.beanY[i] = (x >> 8) / 16777216.0f;
            g.beanPhase[i] = static_cast<float>(i) * 0.7f;
        }
    }
    double time = GetTickCount() / 1000.0;
    float agit = std::min(2.0f, g.vinfo.agitation) * (g.vinfo.bean_location == 1 ? 1.0f : 0.15f);
    int w = area.right - area.left - S(20), h = area.bottom - area.top - S(30);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    for (size_t i = 0; i < n; ++i) {
        const rs_visual_bean& b = g.vbeans[i];
        double jx = std::sin(time * 3.1 + g.beanPhase[i]) * 0.02 * agit;
        double jy = std::cos(time * 2.3 + g.beanPhase[i] * 1.3) * 0.04 * agit;
        int cx = area.left + S(10) + static_cast<int>((g.beanX[i] + jx) * w);
        int cy = area.top + S(24) + static_cast<int>((g.beanY[i] + jy) * h);
        int rad = std::max(2, S(static_cast<int>(b.size_mm * 0.75)));
        auto ch = [](float c) { return static_cast<int>(std::clamp(c, 0.0f, 1.0f) * 255); };
        SetDCBrushColor(dc, RGB(ch(b.rgb[0]), ch(b.rgb[1]), ch(b.rgb[2])));
        Ellipse(dc, cx - rad, cy - rad * 2 / 3, cx + rad, cy + rad * 2 / 3);
        if (b.oil > 0.2f) {
            SetDCBrushColor(dc, RGB(255, 245, 230));
            Ellipse(dc, cx - rad / 3, cy - rad / 3, cx, cy - rad / 6);
        }
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

void onPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g.wnd, &ps);
    RECT rc;
    GetClientRect(g.wnd, &rc);
    rc.bottom = std::max(rc.top + 1, rc.bottom - g.statusH);
    RECT right{S(g.leftWidth), 0, rc.right, rc.bottom};
    RECT dirty;
    if (IntersectRect(&dirty, &ps.rcPaint, &right)) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, right.right - right.left, right.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        RECT local{0, 0, right.right - right.left, right.bottom};
        int beanH = S(150);
        RECT graph{0, 0, local.right, local.bottom - beanH};
        RECT beans{0, local.bottom - beanH, local.right, local.bottom};
        paintGraph(mem, graph);
        paintBeans(mem, beans);
        BitBlt(dc, right.left, 0, local.right, local.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    // Left pane background: with WS_CLIPCHILDREN this only touches the gaps between controls.
    RECT left{0, 0, S(g.leftWidth), rc.bottom};
    if (IntersectRect(&dirty, &ps.rcPaint, &left)) {
        FillRect(dc, &dirty, GetSysColorBrush(COLOR_BTNFACE));
        // Section headings: bold caption followed by a hairline to the pane edge.
        HGDIOBJ oldFont = SelectObject(dc, g.heading);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 70, 40));
        const int pad = S(10);
        for (const auto& h : g.headings) {
            int len = static_cast<int>(wcslen(h.text));
            TextOutW(dc, pad, h.y + S(3), h.text, len);
            SIZE sz;
            GetTextExtentPoint32W(dc, h.text, len, &sz);
            RECT line{pad + sz.cx + S(8), h.y + S(3) + sz.cy / 2, left.right - pad, h.y + S(3) + sz.cy / 2 + 1};
            FillRect(dc, &line, GetSysColorBrush(COLOR_3DSHADOW));
        }
        SelectObject(dc, oldFont);
        // Divider between the control pane and the graph.
        RECT div{left.right - 1, 0, left.right, rc.bottom};
        FillRect(dc, &div, GetSysColorBrush(COLOR_3DSHADOW));
    }
    EndPaint(g.wnd, &ps);
}

// ---------------------------------------------------------------- actions
void setFault(const char* probe, const char* type) {
    Value c = Value::object();
    c["cmd"] = "fault";
    c["probe"] = probe;
    c["type"] = type;
    showError(command(c));
}

void setUnits(const char* key, const char* value) {
    Value c = Value::object();
    c["cmd"] = "units";
    c[key] = value;
    Value r = command(c);
    showError(r);
    if (r.getBool("ok", false) && !r.getBool("saved", false))
        MessageBoxW(g.wnd, L"Units changed, but roastsim.json next to the program could not be written, so they won't be remembered.",
                    L"Units", MB_ICONINFORMATION);
    refresh();
}

double editNumber(HWND h) { return _wtof(getText(h).c_str()); }

// Parses a whole edit field as a number; false if it is empty or not a number.
bool readNumber(HWND dlg, int id, double& out) {
    std::wstring s = getText(GetDlgItem(dlg, id));
    wchar_t* end = nullptr;
    out = std::wcstod(s.c_str(), &end);
    while (end && *end == L' ') ++end;
    return !s.empty() && end && *end == 0 && std::isfinite(out);
}

std::wstring num(double v, double step) { return fmt(L"%g", std::round(v / step) * step); }

bool fieldError(HWND dlg, int id, const wchar_t* msg) {
    MessageBoxW(dlg, msg, L"Environment", MB_ICONWARNING);
    HWND e = GetDlgItem(dlg, id);
    SetFocus(e);
    SendMessageW(e, EM_SETSEL, 0, -1);
    return false;
}

bool applyEnvironment(HWND dlg) {
    double amb, volts, ohms, gas, alt;
    if (!readNumber(dlg, IDC_ENV_AMBIENT, amb) || fromT(amb) < -40 || fromT(amb) > 60)
        return fieldError(dlg, IDC_ENV_AMBIENT, isF() ? L"Room temperature must be between -40 and 140 °F."
                                                       : L"Room temperature must be between -40 and 60 °C.");
    if (!readNumber(dlg, IDC_ENV_VOLTS, volts) || volts < 50 || volts > 260)
        return fieldError(dlg, IDC_ENV_VOLTS, L"Mains voltage must be between 50 and 260 V.");
    if (!readNumber(dlg, IDC_ENV_OHMS, ohms) || ohms < 0 || ohms > 5)
        return fieldError(dlg, IDC_ENV_OHMS, L"Wiring resistance must be between 0 and 5 ohms.");
    if (!readNumber(dlg, IDC_ENV_GAS, gas) || gas < 0 || gas > 200)
        return fieldError(dlg, IDC_ENV_GAS, L"Gas supply pressure must be between 0 and 200 %.");
    if (!readNumber(dlg, IDC_ENV_ALT, alt) || fromA(alt) < -500 || fromA(alt) > 5000)
        return fieldError(dlg, IDC_ENV_ALT, isFt() ? L"Altitude must be between -1640 and 16400 ft."
                                                    : L"Altitude must be between -500 and 5000 m.");
    Value c = Value::object();
    c["cmd"] = "env";
    c["ambient_C"] = fromT(amb);
    c["line_voltage"] = volts;
    c["source_ohms"] = ohms;
    c["gas_pressure"] = gas / 100.0;
    c["altitude_m"] = fromA(alt);
    Value r = command(c);
    if (!r.getBool("ok", true)) {
        MessageBoxW(dlg, widen(r.get("error", "failed")).c_str(), L"Environment", MB_ICONWARNING);
        return false;
    }
    return true;
}

void fillEnvironment(HWND dlg, double ambC, double volts, double ohms, double gas, double altM) {
    SetDlgItemTextW(dlg, IDC_ENV_AMBIENT, num(T(ambC), 0.1).c_str());
    SetDlgItemTextW(dlg, IDC_ENV_VOLTS, num(volts, 0.1).c_str());
    SetDlgItemTextW(dlg, IDC_ENV_OHMS, num(ohms, 0.001).c_str());
    SetDlgItemTextW(dlg, IDC_ENV_GAS, num(gas * 100.0, 1).c_str());
    SetDlgItemTextW(dlg, IDC_ENV_ALT, num(A(altM), 1).c_str());
}

// lParam: the edit control to focus first (the menu item picked), or 0.
INT_PTR CALLBACK envDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            Value e = cmd1("state")["env"];
            fillEnvironment(dlg, e.get("ambient_C", 22.0), e.get("line_voltage", 120.0), e.get("source_ohms", 0.25),
                            e.get("gas_pressure", 1.0), e.get("altitude_m", 0.0));
            SetDlgItemTextW(dlg, IDC_ENV_AMBIENT_U, TU());
            SetDlgItemTextW(dlg, IDC_ENV_ALT_U, isFt() ? L"ft" : L"m");
            HWND focus = GetDlgItem(dlg, lp ? static_cast<int>(lp) : IDC_ENV_AMBIENT);
            SetFocus(focus);
            SendMessageW(focus, EM_SETSEL, 0, -1);
            return FALSE;  // focus set by hand
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_ENV_DEFAULTS: {
                    // The simulator's built-in environment (core Environment defaults).
                    fillEnvironment(dlg, 22.0, 120.0, 0.25, 1.0, 0.0);
                    return TRUE;
                }
                case IDOK:
                    if (applyEnvironment(dlg)) EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
            }
            break;
    }
    return FALSE;
}

void showEnvironment(int focusId = 0) {
    DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_ENVIRONMENT), g.wnd, envDlgProc, focusId);
    refresh();
}

INT_PTR CALLBACK preheatDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetDlgItemTextW(dlg, IDC_PRE_TARGET, num(T(g.preheatC), 1).c_str());
            SetDlgItemTextW(dlg, IDC_PRE_UNIT, TU());
            HWND e = GetDlgItem(dlg, IDC_PRE_TARGET);
            SetFocus(e);
            SendMessageW(e, EM_SETSEL, 0, -1);
            return FALSE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    double v;
                    if (!readNumber(dlg, IDC_PRE_TARGET, v)) {
                        MessageBoxW(dlg, L"Enter a target temperature.", L"Preheat", MB_ICONWARNING);
                        return TRUE;
                    }
                    g.preheatC = fromT(v);
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
            }
            break;
    }
    return FALSE;
}

void preheat() {
    if (DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_PREHEAT), g.wnd, preheatDlgProc, 0) != IDOK) return;
    Value c = Value::object();
    c["cmd"] = "preheat";
    c["target_C"] = g.preheatC;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    showError(command(c));  // the skipped-ahead samples arrive with the next refresh
    SetCursor(old);
    refresh();
}

void setSpeed(int i) {
    Value c = Value::object();
    if (i == 0) {
        c["cmd"] = "pause";
        c["value"] = true;
    } else {
        Value p = Value::object();
        p["cmd"] = "pause";
        p["value"] = false;
        command(p);
        c["cmd"] = "time_scale";
        c["value"] = kSpeeds[i];
    }
    showError(command(c));
    refresh();
}

void loadSelected(bool reloadOnly) {
    int ri = static_cast<int>(SendMessageW(g.roasterCombo, CB_GETCURSEL, 0, 0));
    int bi = static_cast<int>(SendMessageW(g.beanCombo, CB_GETCURSEL, 0, 0));
    if (ri < 0) return;
    if (g.st.bean_location == 1 &&
        MessageBoxW(g.wnd, reloadOnly ? L"Beans are roasting. Reload the roaster anyway and discard them?"
                                      : L"Beans are roasting. Switch roaster anyway and discard them?",
                    L"RoastSim", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        for (size_t i = 0; i < g.roasterIds.size(); ++i)  // put the combo back on the roaster in use
            if (g.roasterIds[i] == g.currentRoaster) SendMessageW(g.roasterCombo, CB_SETCURSEL, i, 0);
        return;
    }
    Value c = Value::object();
    c["cmd"] = "load";
    c["roaster"] = g.roasterIds[static_cast<size_t>(ri)];
    if (bi >= 0) c["bean"] = g.beanIds[static_cast<size_t>(bi)];
    showError(command(c));
    applyRoasterDef();
    layout();
}

void setPower(bool on) {
    Value c = Value::object();
    c["cmd"] = "power";
    c["value"] = on;
    showError(command(c));
    refresh();
}

void onCommand(int id, int code) {
    if (id >= IDM_SPEED0 && id < IDM_SPEED0 + kSpeedCount) {
        setSpeed(id - IDM_SPEED0);
        return;
    }
    if (id >= IDM_VOL0 && id < IDM_VOL0 + kVolumeCount) {
        g.volume = kVolumes[id - IDM_VOL0];
        applySound();
        return;
    }
    switch (id) {
        case IDC_ROASTER:
            if (code == CBN_SELCHANGE) loadSelected(false);
            break;
        case IDM_RELOAD: loadSelected(true); break;
        case IDC_BEAN: {
            if (code != CBN_SELCHANGE) break;
            int bi = static_cast<int>(SendMessageW(g.beanCombo, CB_GETCURSEL, 0, 0));
            if (bi < 0) break;
            Value c = Value::object();
            c["cmd"] = "bean";
            c["id"] = g.beanIds[static_cast<size_t>(bi)];
            showError(command(c));
            break;
        }
        case IDC_CHARGE: {
            Value c = Value::object();
            c["cmd"] = "charge";
            c["batch_g"] = fromW(editNumber(g.batchEdit));
            showError(command(c));
            break;
        }
        case IDC_DROP: showError(cmd1("drop")); break;
        case IDM_COOLEND: showError(cmd1("cool_end")); break;
        case IDM_EMPTY: showError(cmd1("empty")); break;
        case IDC_MARKFC:
        case IDM_MARK_DRY: case IDM_MARK_FCS: case IDM_MARK_FCE: case IDM_MARK_SCS: case IDM_MARK_SCE: {
            int idx = id == IDC_MARKFC ? g.nextMark : id - IDM_MARK_DRY;
            if (idx < 0) break;
            Value c = Value::object();
            c["cmd"] = "mark";
            c["event"] = kMarkEvents[idx];
            command(c);
            refresh();  // advance the button right away
            break;
        }
        case IDM_REPORT: showReport(); break;
        case IDC_POWER:
            if (code == BN_CLICKED) setPower(SendMessageW(g.powerCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDM_POWER: setPower(!g.st.powered); break;
        case IDC_COOL:
            if (code == BN_CLICKED)
                rs_host_set_control(g.host, RS_CTL_COOLING, SendMessageW(g.coolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 100 : 0);
            break;
        case IDM_PREHEAT: preheat(); break;
        case IDM_RESET:
            if (MessageBoxW(g.wnd, L"Cool the machine to room temperature and discard any beans?", L"Cold reset",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                cmd1("reset");
                reloadHistory();
            }
            break;
        case IDM_REPAIR: showError(cmd1("repair")); break;
        case IDM_ENV: showEnvironment(); break;
        case IDM_ENV_AMBIENT: showEnvironment(IDC_ENV_AMBIENT); break;
        case IDM_ENV_VOLTS: showEnvironment(IDC_ENV_VOLTS); break;
        case IDM_ENV_ALT: showEnvironment(IDC_ENV_ALT); break;
        case IDM_SOUND:
            g.soundOn = !g.soundOn;
            applySound();
            break;
        case IDM_TEMP_C: setUnits("temperature", "C"); break;
        case IDM_TEMP_F: setUnits("temperature", "F"); break;
        case IDM_W_G: setUnits("weight", "g"); break;
        case IDM_W_KG: setUnits("weight", "kg"); break;
        case IDM_W_OZ: setUnits("weight", "oz"); break;
        case IDM_W_LB: setUnits("weight", "lb"); break;
        case IDM_ALT_M: setUnits("altitude", "m"); break;
        case IDM_ALT_FT: setUnits("altitude", "ft"); break;
        case IDM_FAULT_BT_OPEN: setFault("BT", "open"); break;
        case IDM_FAULT_BT_SPIKES: setFault("BT", "spikes"); break;
        case IDM_FAULT_BT_DRIFT: setFault("BT", "drift"); break;
        case IDM_FAULT_BT_NOISY: setFault("BT", "noisy"); break;
        case IDM_FAULT_ET_OPEN: setFault("ET", "open"); break;
        case IDM_FAULT_CLEAR:
            setFault("BT", "none");
            setFault("ET", "none");
            break;
        case IDM_WEB: {
            Value info = cmd1("server");
            int port = static_cast<int>(info["config"].get("http_port", 8080.0));
            ShellExecuteW(nullptr, L"open", fmt(L"http://localhost:%d/", port).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_FOLDER:
            ShellExecuteW(nullptr, L"open", widen(roastsim::platform::executableDir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_ABOUT:
            MessageBoxW(g.wnd,
                        L"RoastSim 2026 - virtual coffee roaster\n\n"
                        L"Artisan can connect while this window is open:\n"
                        L"  Modbus TCP: 127.0.0.1:502\n"
                        L"  WebSocket:  ws://127.0.0.1:8080/WebSocket\n"
                        L"See docs/ARTISAN_SETUP.md for register maps and settings.\n\n"
                        L"The Units menu also sets what Artisan receives (match Artisan's C/F mode).\n"
                        L"Edit roastsim.json next to the .exe to change ports or bind to the LAN.",
                        L"About RoastSim", MB_ICONINFORMATION);
            break;
        case IDM_EXIT: DestroyWindow(g.wnd); break;
        default: break;
    }
}

// Clicks on the status bar: the speed part pops up the speed menu, the
// environment part opens the Environment dialog.
void onStatusClick(int part) {
    if (part == SB_SPEED) {
        updateMenus();
        RECT r;
        SendMessageW(g.statusBar, SB_GETRECT, SB_SPEED, reinterpret_cast<LPARAM>(&r));
        POINT pt{r.left, r.top};
        ClientToScreen(g.statusBar, &pt);
        TrackPopupMenu(g.speedMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
    } else if (part == SB_ENV) {
        showEnvironment();
    }
}

void onSlider(HWND slider, int code) {
    for (int i = 0; i < kSliderCount; ++i) {
        if (g.sliders[i] != slider) continue;
        g.dragging[i] = code == TB_THUMBTRACK;
        int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        rs_host_set_control(g.host, kSliderControls[i], pos);
        setText(g.sliderValues[i], fmt(L"%d%%", pos));
    }
}

void applySound() {
    if (!g.audio) return;
    rs_audio_set_mix(g.audio, g.volume / 100.0f, 1.0f, 1.0f);
    if (!g.soundOn) {
        rs_audio_pause(g.audio);
        return;
    }
    char err[256] = {0};
    if (!rs_audio_play(g.audio, err, sizeof err)) {
        g.soundOn = false;
        MessageBoxA(g.wnd, err, "Sound", MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------- window
void createWidgets() {
    g.roasterCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_ROASTER);
    g.beanCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BEAN);
    g.desc = mk(L"STATIC", L"", SS_LEFT, IDC_DESC);
    SendMessageW(g.desc, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);
    g.batchEdit = mk(L"EDIT", L"100", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT, IDC_BATCH);
    g.batchUnit = mk(L"STATIC", L"g", SS_LEFT, IDC_BATCHUNIT);
    mk(L"BUTTON", L"Charge", BS_PUSHBUTTON | WS_TABSTOP, IDC_CHARGE);
    mk(L"BUTTON", L"Drop", BS_PUSHBUTTON | WS_TABSTOP, IDC_DROP);
    g.markButton = mk(L"BUTTON", kMarkLabels[0], BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, IDC_MARKFC);
    g.phasesPanel = mk(L"RoastSimPhases", L"", 0, IDC_PHASES);
    g.powerCheck = mk(L"BUTTON", L"Power", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_POWER);
    g.coolCheck = mk(L"BUTTON", L"Cooling tray fan", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_COOL);
    for (int i = 0; i < kSliderCount; ++i) {
        g.sliderLabels[i] = mk(L"STATIC", L"", SS_LEFT, IDC_SLABEL0 + i);
        g.sliders[i] = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_SLIDER0 + i);
        g.sliderValues[i] = mk(L"STATIC", L"0%", SS_RIGHT, IDC_SVALUE0 + i);
    }
    g.readout = mk(L"RoastSimReadout", L"", 0, IDC_READOUT);
    g.statusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g.wnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)), g.inst, nullptr);

    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_WEB, L"Open &web control panel");
    AppendMenuW(file, MF_STRING, IDM_FOLDER, L"Open program &folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

    HMENU roast = CreatePopupMenu();
    AppendMenuW(roast, MF_STRING, IDC_CHARGE, L"&Charge");
    AppendMenuW(roast, MF_STRING, IDC_DROP, L"&Drop");
    AppendMenuW(roast, MF_STRING, IDM_COOLEND, L"&End cooling");
    AppendMenuW(roast, MF_STRING, IDM_EMPTY, L"Empty roaster (discard beans)");
    AppendMenuW(roast, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(roast, MF_STRING, IDM_MARK_DRY, L"Mark DRY");
    AppendMenuW(roast, MF_STRING, IDM_MARK_FCS, L"Mark FC start");
    AppendMenuW(roast, MF_STRING, IDM_MARK_FCE, L"Mark FC end");
    AppendMenuW(roast, MF_STRING, IDM_MARK_SCS, L"Mark SC start");
    AppendMenuW(roast, MF_STRING, IDM_MARK_SCE, L"Mark SC end");
    AppendMenuW(roast, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(roast, MF_STRING, IDM_REPORT, L"Roast &report\tCtrl+R");

    HMENU faults = CreatePopupMenu();
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_OPEN, L"BT probe open circuit");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_SPIKES, L"BT probe spikes");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_DRIFT, L"BT probe drift");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_NOISY, L"BT probe noisy");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_ET_OPEN, L"ET probe open circuit");
    AppendMenuW(faults, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(faults, MF_STRING, IDM_FAULT_CLEAR, L"Clear probe faults");
    HMENU machine = CreatePopupMenu();
    AppendMenuW(machine, MF_STRING, IDM_POWER, L"&Power");
    AppendMenuW(machine, MF_STRING, IDM_PREHEAT, L"Pre&heat...");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_STRING, IDM_RESET, L"&Cold reset...");
    AppendMenuW(machine, MF_STRING, IDM_REPAIR, L"Re&pair (fuse, thermostat)");
    AppendMenuW(machine, MF_STRING, IDM_RELOAD, L"&Reload roaster preset");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_POPUP, reinterpret_cast<UINT_PTR>(faults), L"Probe &faults");

    // Item texts are rewritten with the current values by updateMenus().
    HMENU env = CreatePopupMenu();
    AppendMenuW(env, MF_STRING, IDM_ENV_AMBIENT, L"Room temperature");
    AppendMenuW(env, MF_STRING, IDM_ENV_VOLTS, L"Mains voltage");
    AppendMenuW(env, MF_STRING, IDM_ENV_ALT, L"Altitude");
    AppendMenuW(env, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(env, MF_STRING, IDM_ENV, L"&All environment settings...\tCtrl+E");

    g.speedMenu = CreatePopupMenu();
    const wchar_t* speedNames[kSpeedCount] = {L"&Pause", L"&Real time", L"&2x", L"&5x", L"&10x", L"&30x", L"&60x"};
    for (int i = 0; i < kSpeedCount; ++i) {
        AppendMenuW(g.speedMenu, MF_STRING, IDM_SPEED0 + i, speedNames[i]);
        if (i == 0) AppendMenuW(g.speedMenu, MF_SEPARATOR, 0, nullptr);
    }
    HMENU volume = CreatePopupMenu();
    for (int i = 0; i < kVolumeCount; ++i)
        AppendMenuW(volume, MF_STRING, IDM_VOL0 + i, fmt(L"%d%%", kVolumes[i]).c_str());
    HMENU sim = CreatePopupMenu();
    AppendMenuW(sim, MF_POPUP, reinterpret_cast<UINT_PTR>(g.speedMenu), L"&Speed");
    AppendMenuW(sim, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sim, MF_STRING, IDM_SOUND, L"S&ound");
    AppendMenuW(sim, MF_POPUP, reinterpret_cast<UINT_PTR>(volume), L"&Volume");

    HMENU units = CreatePopupMenu();
    AppendMenuW(units, MF_STRING, IDM_TEMP_C, L"Temperature: °C");
    AppendMenuW(units, MF_STRING, IDM_TEMP_F, L"Temperature: °F");
    AppendMenuW(units, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(units, MF_STRING, IDM_W_G, L"Weight: grams");
    AppendMenuW(units, MF_STRING, IDM_W_KG, L"Weight: kilograms");
    AppendMenuW(units, MF_STRING, IDM_W_OZ, L"Weight: ounces");
    AppendMenuW(units, MF_STRING, IDM_W_LB, L"Weight: pounds");
    AppendMenuW(units, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(units, MF_STRING, IDM_ALT_M, L"Altitude: metres");
    AppendMenuW(units, MF_STRING, IDM_ALT_FT, L"Altitude: feet");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About / Artisan connection");

    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(roast), L"&Roast");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(machine), L"&Machine");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(env), L"&Environment");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(sim), L"&Simulation");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(units), L"&Units");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(g.wnd, bar);
    g.menuBar = bar;

    ACCEL acc[] = {{FVIRTKEY | FCONTROL, 'E', IDM_ENV}, {FVIRTKEY | FCONTROL, 'R', IDM_REPORT}};
    g.accel = CreateAcceleratorTableW(acc, 2);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.wnd = h;
            createWidgets();
            populateLists();
            g.st.struct_size = sizeof(rs_state);
            rs_host_get_state(g.host, &g.st);  // units before the first labels are drawn
            applyRoasterDef();
            layout();
            SetTimer(h, TIMER_UI, 200, nullptr);
            applySound();
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(940);
            mm->ptMinTrackSize.y = S(700);
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_UI) refresh();
            return 0;
        case WM_INITMENUPOPUP: updateMenus(); return 0;
        case WM_HSCROLL:
            if (lp) onSlider(reinterpret_cast<HWND>(lp), LOWORD(wp));
            return 0;
        case WM_COMMAND: onCommand(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_APP_ROAST_DONE:
            // Roast finished (COOL): the graph is already frozen; switch the
            // machine off, then show the report.
            setPower(false);
            showReport();
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->hwndFrom == g.statusBar && nm->code == NM_CLICK)
                onStatusClick(static_cast<int>(reinterpret_cast<NMMOUSE*>(lp)->dwItemSpec));
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: onPaint(); return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_DESTROY:
            KillTimer(h, TIMER_UI);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = inst;
    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW sm = ncm.lfMessageFont;
    sm.lfHeight = sm.lfHeight * 9 / 10;
    g.small = CreateFontIndirectW(&sm);
    LOGFONTW hd = ncm.lfMessageFont;
    hd.lfWeight = FW_SEMIBOLD;
    g.heading = CreateFontIndirectW(&hd);
    LOGFONTW bg = ncm.lfMessageFont;
    bg.lfHeight = -S(30);
    bg.lfWeight = FW_SEMIBOLD;
    g.big = CreateFontIndirectW(&bg);
    g.mono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    // roastsim.json next to the exe configures ports, bind address, units...;
    // unit changes made in the app are saved back to it.
    std::string cfgText, err1;
    std::string cfgPath = roastsim::platform::executableDir() + "/roastsim.json";
    roastsim::readTextFile(cfgPath, cfgText);
    Value cfg = cfgText.empty() ? Value::object() : Value::parse(cfgText, &err1);
    if (!err1.empty() || !cfg.isObject()) {
        MessageBoxA(nullptr, ("roastsim.json: " + err1).c_str(), "RoastSim", MB_ICONWARNING);
        cfg = Value::object();
    }
    cfg["config_path"] = cfgPath;
    std::string cfgJson = cfg.dump();
    char err[1024] = {0};
    g.host = rs_host_create(cfgJson.c_str(), err, sizeof err);
    if (!g.host || !rs_host_start(g.host, err, sizeof err)) {
        MessageBoxA(nullptr, err, "RoastSim failed to start", MB_ICONERROR);
        return 1;
    }
    g.audio = rs_audio_create(g.host, 44100);

    WNDCLASSEXW rc{};
    rc.cbSize = sizeof rc;
    rc.lpfnWndProc = readoutProc;
    rc.hInstance = inst;
    rc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    rc.lpszClassName = L"RoastSimReadout";
    RegisterClassExW(&rc);
    rc.lpfnWndProc = phasesProc;
    rc.lpszClassName = L"RoastSimPhases";
    RegisterClassExW(&rc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"RoastSimMain";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"RoastSim 2026 - virtual coffee roaster",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1300), S(860),
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(w, g.accel, &m) && !IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    rs_audio_destroy(g.audio);
    rs_host_stop(g.host);
    rs_host_destroy(g.host);
    return 0;
}
