// RoastSim native Windows shell.
//
// Talks to the simulator only through the C API (roastsim.h) plus the JSON
// helper for parsing replies, so it doubles as the reference for the GTK and
// Haiku BeAPI shells. Hosts the same servers as roastsimd (Modbus, WebSocket,
// web UI, optional TC4 serial), so Artisan can connect while you watch here.
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "roastsim/catalog.hpp"
#include "roastsim/json.hpp"
#include "roastsim/platform.hpp"
#include "roastsim/roastsim.h"

using roastsim::json::Value;

namespace {

// ---------------------------------------------------------------- ids
enum : int {
    IDC_ROASTER = 101, IDC_BEAN, IDC_LOAD, IDC_BATCH, IDC_CHARGE, IDC_DROP, IDC_PREHEAT_T, IDC_PREHEAT,
    IDC_RESET, IDC_REPAIR, IDC_SPEED, IDC_SOUND, IDC_REPORT, IDC_MARKFC, IDC_AMBIENT, IDC_VOLTS, IDC_ALT,
    IDC_POWER, IDC_ENVAPPLY, IDC_READOUT, IDC_STATUS, IDC_DESC, IDC_ENVLABEL, IDC_ENVL2, IDC_ENVL3, IDC_VOLUME, IDC_COOL,
    IDC_SLIDER0 = 200, IDC_SLABEL0 = 220, IDC_SVALUE0 = 240,
    IDM_EXIT = 1001, IDM_WEB, IDM_FOLDER, IDM_COOLEND, IDM_EMPTY,
    IDM_MARK_DRY = 1020, IDM_MARK_FCS, IDM_MARK_FCE, IDM_MARK_SCS, IDM_MARK_SCE,
    IDM_FAULT_BT_OPEN = 1040, IDM_FAULT_BT_SPIKES, IDM_FAULT_BT_DRIFT, IDM_FAULT_BT_NOISY, IDM_FAULT_ET_OPEN,
    IDM_FAULT_CLEAR, IDM_ABOUT = 1060,
    IDM_TEMP_C = 1100, IDM_TEMP_F, IDM_W_G, IDM_W_KG, IDM_W_OZ, IDM_W_LB, IDM_ALT_M, IDM_ALT_FT,
};
enum : UINT_PTR { TIMER_UI = 1 };

constexpr int kSliderCount = 5;  // heat, fan, drum, stir, voltage
const int kSliderControls[kSliderCount] = {RS_CTL_HEAT, RS_CTL_FAN, RS_CTL_DRUM, RS_CTL_STIR, RS_CTL_VOLTAGE};

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
};

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    rs_host* host = nullptr;
    HFONT font = nullptr, mono = nullptr, small = nullptr;
    double dpi = 1.0;

    HWND roasterCombo, beanCombo, batchEdit, preheatEdit, speedCombo, soundCheck, readout, status, desc;
    HWND ambientEdit, voltsEdit, altEdit, coolCheck, powerCheck, volSlider = nullptr;
    HWND sliders[kSliderCount], sliderLabels[kSliderCount], sliderValues[kSliderCount];
    bool dragging[kSliderCount] = {};
    HMENU unitsMenu = nullptr;
    std::wstring readoutText;

    std::vector<std::string> roasterIds, beanIds;
    Value roasterDef;
    std::string currentRoaster;

    rs_state st{};
    std::string unitKey;         // detects unit changes made anywhere (menu, web UI, Artisan TC4)
    double preheatC = 200;       // preheat target, kept in C
    bool envShown = false;
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

// ---------------------------------------------------------------- roaster setup
void reloadHistory() {
    g.hist.clear();
    g.histNext = 0;
    g.events.clear();
    g.eventSeq = 0;
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
    std::wstring wu = widen(wUnit());
    setText(GetDlgItem(g.wnd, IDC_CHARGE), L"Charge (" + wu + L")");
    setText(GetDlgItem(g.wnd, IDC_PREHEAT), std::wstring(L"Preheat (") + TU() + L")");
    setText(GetDlgItem(g.wnd, IDC_ENVLABEL), std::wstring(L"Ambient ") + TU());
    setText(GetDlgItem(g.wnd, IDC_ENVL3), isFt() ? L"Altitude ft" : L"Altitude m");
    SetWindowTextW(g.batchEdit, fmtW(g.st.next_batch_g > 0 ? g.st.next_batch_g : g.st.batch_g).c_str());
    SetWindowTextW(g.preheatEdit, fmt(L"%.0f", T(g.preheatC)).c_str());
    SetWindowTextW(g.ambientEdit, fmt(L"%.0f", T(g.st.ambient_c)).c_str());
    SetWindowTextW(g.voltsEdit, fmt(L"%.0f", g.st.line_voltage).c_str());
    SetWindowTextW(g.altEdit, fmt(L"%.0f", A(g.st.altitude_m)).c_str());
    std::string w = wUnit();
    CheckMenuRadioItem(g.unitsMenu, IDM_TEMP_C, IDM_TEMP_F, isF() ? IDM_TEMP_F : IDM_TEMP_C, MF_BYCOMMAND);
    int wid = w == "kg" ? IDM_W_KG : w == "oz" ? IDM_W_OZ : w == "lb" ? IDM_W_LB : IDM_W_G;
    CheckMenuRadioItem(g.unitsMenu, IDM_W_G, IDM_W_LB, wid, MF_BYCOMMAND);
    CheckMenuRadioItem(g.unitsMenu, IDM_ALT_M, IDM_ALT_FT, isFt() ? IDM_ALT_FT : IDM_ALT_M, MF_BYCOMMAND);
    invalidateRight();
}

// ---------------------------------------------------------------- layout
void layout() {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int pad = S(8), w = S(g.leftWidth) - 2 * pad, rowH = S(24), gap = S(6);
    int y = pad;
    auto place = [&](HWND h, int x, int yy, int ww, int hh) { MoveWindow(h, x, yy, ww, hh, FALSE); };
    place(g.roasterCombo, pad, y, w, S(300));
    y += rowH + gap;
    int half = (w - gap) / 2;
    place(g.beanCombo, pad, y, w - S(70) - gap, S(300));
    place(GetDlgItem(g.wnd, IDC_LOAD), pad + w - S(70), y, S(70), rowH);
    y += rowH + gap;
    place(g.desc, pad, y, w, S(48));
    y += S(48) + gap;
    int c1 = S(70), c2 = S(100);
    place(g.batchEdit, pad, y, c1, rowH);
    place(GetDlgItem(g.wnd, IDC_CHARGE), pad + c1 + gap, y, c2, rowH);
    place(GetDlgItem(g.wnd, IDC_DROP), pad + c1 + c2 + 2 * gap, y, S(80), rowH);
    place(GetDlgItem(g.wnd, IDC_MARKFC), pad + c1 + c2 + S(80) + 3 * gap, y, w - (c1 + c2 + S(80) + 3 * gap), rowH);
    y += rowH + gap;
    place(g.preheatEdit, pad, y, c1, rowH);
    place(GetDlgItem(g.wnd, IDC_PREHEAT), pad + c1 + gap, y, c2, rowH);
    place(GetDlgItem(g.wnd, IDC_RESET), pad + c1 + c2 + 2 * gap, y, S(80), rowH);
    place(GetDlgItem(g.wnd, IDC_REPAIR), pad + c1 + c2 + S(80) + 3 * gap, y, w - (c1 + c2 + S(80) + 3 * gap), rowH);
    y += rowH + gap * 2;
    for (int i = 0; i < kSliderCount; ++i) {
        if (!IsWindowVisible(g.sliders[i])) continue;
        place(g.sliderLabels[i], pad, y + S(4), S(90), rowH);
        place(g.sliders[i], pad + S(92), y, w - S(92) - S(60), S(30));
        place(g.sliderValues[i], pad + w - S(56), y + S(4), S(56), rowH);
        y += S(32);
    }
    place(g.powerCheck, pad, y, S(74), rowH);
    place(g.coolCheck, pad + S(80), y, half - S(80), rowH);
    place(g.soundCheck, pad + half + gap, y, S(70), rowH);
    place(g.volSlider, pad + half + gap + S(70), y, w - half - gap - S(70), rowH);
    y += rowH + gap;
    place(g.speedCombo, pad, y, S(120), S(300));
    place(GetDlgItem(g.wnd, IDC_REPORT), pad + S(126), y, S(110), rowH);
    y += rowH + gap;
    int ew = (w - S(80) - 3 * gap) / 3;
    for (int i = 0; i < 3; ++i) place(GetDlgItem(g.wnd, IDC_ENVLABEL + i), pad + i * (ew + gap), y, ew, S(18));
    y += S(18);
    place(g.ambientEdit, pad, y, ew, rowH);
    place(g.voltsEdit, pad + ew + gap, y, ew, rowH);
    place(g.altEdit, pad + 2 * (ew + gap), y, ew, rowH);
    place(GetDlgItem(g.wnd, IDC_ENVAPPLY), pad + w - S(80), y, S(80), rowH);
    y += rowH + gap;
    int statusH = S(54);
    place(g.readout, pad, y, w, std::max(S(60), static_cast<int>(rc.bottom) - y - statusH - pad - gap));
    place(g.status, pad, rc.bottom - statusH - pad, w, statusH);
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
    const wchar_t* phases[] = {L"idle (empty)", L"ROASTING", L"cooling", L"done"};
    const wchar_t* tu = TU();
    std::wstring r;
    r += fmt(L"Roast time  %s    %s\r\n", mmss(s.t_roast).c_str(), phases[std::clamp(s.phase, 0, 3)]);
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
    if (key != g.unitKey || !g.envShown) {
        g.unitKey = key;
        g.envShown = true;
        applyUnits();
    }
    // History: reload when a roast starts or history was trimmed.
    if (g.st.phase != g.lastPhase || g.st.history_end < g.histNext) {
        if ((g.lastPhase != RS_PHASE_ROASTING && g.st.phase == RS_PHASE_ROASTING) || g.st.history_end < g.histNext) {
            g.hist.clear();
            g.histNext = 0;
        }
        g.lastPhase = g.st.phase;
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
            if (type == "CHARGE") g.events.clear();
            g.events.push_back({type, e.get("detail", ""), e.get("t_roast", -1.0), e.get("t", 0.0)});
            if (type == "COOL") PostMessageW(g.wnd, WM_COMMAND, IDC_REPORT, 0);
        }
        g.eventSeq = g.st.event_seq;
    }

    syncSliders();
    if (GetFocus() != g.speedCombo && !SendMessageW(g.speedCombo, CB_GETDROPPEDSTATE, 0, 0)) {
        const double speeds[] = {0, 1, 2, 5, 10, 30, 60};
        int sel = 1;
        for (int i = 1; i < 7; ++i)
            if (std::fabs(speeds[i] - g.st.time_scale) < 0.01) sel = i;
        if (g.st.paused) sel = 0;
        if (SendMessageW(g.speedCombo, CB_GETCURSEL, 0, 0) != sel) SendMessageW(g.speedCombo, CB_SETCURSEL, sel, 0);
    }
    setReadout(buildReadout());

    static int slow = 0;
    if (++slow % 5 == 1) {
        // Another front end (web UI, Artisan, console) may have loaded a different roaster.
        Value cur = cmd1("state");
        if (cur["roaster"].get("id", "") != g.currentRoaster) {
            applyRoasterDef();
            layout();
        }
        Value info = cmd1("server");
        std::wstring s;
        s += L"Modbus: " + widen(info["net"].get("modbus", "off")) + L"\r\n";
        s += L"Web/WS: " + widen(info["net"].get("http", "off")) + L"\r\n";
        s += g.st.logger_connected ? L"Logger connected (real time locked)" : L"No logger connected";
        if (!g.st.powered) s += L"   [POWER OFF]";
        if (g.st.paused) s += L"   [PAUSED]";
        else if (g.st.time_scale != 1.0 && !g.st.logger_connected) s += fmt(L"   [%.0fx]", g.st.time_scale);
        setText(g.status, s);
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
    bool roast = g.st.t_roast >= 0;
    double x0, x1;
    auto tx = [&](const rs_sample& s) { return roast ? static_cast<double>(s.t_roast) : static_cast<double>(s.t); };
    if (roast) {
        x0 = -60;
        x1 = std::max(12.0 * 60.0, std::ceil((g.st.t_roast + 60) / 120.0) * 120.0);
    } else {
        x1 = std::max(60.0, g.st.t_session);
        x0 = std::max(0.0, x1 - 600.0);
    }
    // Axes in display units: C 0-250 by 50 / F 0-500 by 100; RoR 0-50 C/min / 0-90 F/min.
    const double tStep = isF() ? 100.0 : 50.0, rorMax = isF() ? 90.0 : 50.0, rorStep = isF() ? 15.0 : 10.0;
    double tMax = isF() ? 500.0 : 250.0;
    for (const auto& s : g.hist) tMax = std::max<double>(tMax, std::max(T(s.bt), T(s.et)) + 10);
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
    double step = (x1 - x0) > 900 ? 120 : 60;
    for (double x = std::ceil(x0 / step) * step; x <= x1; x += step) {
        MoveToEx(dc, X(x), t, nullptr);
        LineTo(dc, X(x), b);
        std::wstring s = mmss(x);
        TextOutW(dc, X(x) - S(14), b + S(6), s.c_str(), static_cast<int>(s.size()));
    }
    SelectObject(dc, oldPen);
    DeleteObject(grid);

    HBRUSH heatBrush = CreateSolidBrush(RGB(244, 214, 190));
    int colW = std::max(1, static_cast<int>(std::ceil((r - l) / (x1 - x0))) + 1);
    for (const auto& s : g.hist) {
        double x = tx(s);
        if (x < x0 || x > x1) continue;
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
            if (x < x0 || x > x1) { first = true; continue; }
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
    series(RGB(70, 110, 200), S(2), [](const rs_sample& s) { return static_cast<double>(s.et); }, false);
    series(RGB(200, 60, 40), S(2), [](const rs_sample& s) { return static_cast<double>(s.bt); }, false);
    if (roast) {
        series(RGB(230, 150, 40), 1, [](const rs_sample& s) { return s.t_roast > 20 ? static_cast<double>(s.ror_bt) : -5.0; }, true);
        // No beans before CHARGE: skip (-1) rather than drawing 0 C (= 32 F).
        series(RGB(150, 120, 90), 1, [](const rs_sample& s) { return s.t_roast >= 0 ? static_cast<double>(s.bean_true) : -1.0; }, false);
    }

    HPEN ev = CreatePen(PS_DOT, 1, RGB(90, 80, 70));
    oldPen = SelectObject(dc, ev);
    SetTextColor(dc, RGB(60, 50, 40));
    int labelRow = 0;
    for (const auto& e : g.events) {
        if (e.type == "ALARM" || e.type == "NOTE") continue;
        double x = roast ? e.t_roast : e.t;
        if (x < x0 || x > x1) continue;
        MoveToEx(dc, X(x), t, nullptr);
        LineTo(dc, X(x), b);
        std::wstring s = widen(e.type);
        TextOutW(dc, X(x) + S(3), t + S(2) + (labelRow++ % 2) * S(14), s.c_str(), static_cast<int>(s.size()));
    }
    SelectObject(dc, oldPen);
    DeleteObject(ev);

    struct L { COLORREF c; const wchar_t* n; } leg[] = {{RGB(200, 60, 40), L"BT"}, {RGB(70, 110, 200), L"ET"},
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
    if (IntersectRect(&dirty, &ps.rcPaint, &left)) FillRect(dc, &dirty, GetSysColorBrush(COLOR_BTNFACE));
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

void onCommand(int id, int code) {
    switch (id) {
        case IDC_LOAD:
        case IDC_ROASTER: {
            if (id == IDC_ROASTER && code != CBN_SELCHANGE) break;
            int ri = static_cast<int>(SendMessageW(g.roasterCombo, CB_GETCURSEL, 0, 0));
            int bi = static_cast<int>(SendMessageW(g.beanCombo, CB_GETCURSEL, 0, 0));
            if (ri < 0) break;
            if (g.st.bean_location == 1 &&
                MessageBoxW(g.wnd, L"Beans are roasting. Load anyway and discard them?", L"RoastSim", MB_YESNO | MB_ICONQUESTION) != IDYES)
                break;
            Value c = Value::object();
            c["cmd"] = "load";
            c["roaster"] = g.roasterIds[static_cast<size_t>(ri)];
            if (bi >= 0) c["bean"] = g.beanIds[static_cast<size_t>(bi)];
            showError(command(c));
            applyRoasterDef();
            layout();
            break;
        }
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
        case IDM_MARK_FCS: {
            Value c = Value::object();
            c["cmd"] = "mark";
            c["event"] = "FCs";
            command(c);
            break;
        }
        case IDM_MARK_DRY: case IDM_MARK_FCE: case IDM_MARK_SCS: case IDM_MARK_SCE: {
            const char* names[] = {"DRY", "FCs", "FCe", "SCs", "SCe"};
            Value c = Value::object();
            c["cmd"] = "mark";
            c["event"] = names[id - IDM_MARK_DRY];
            command(c);
            break;
        }
        case IDC_PREHEAT: {
            g.preheatC = fromT(editNumber(g.preheatEdit));
            Value c = Value::object();
            c["cmd"] = "preheat";
            c["target_C"] = g.preheatC;
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            showError(command(c));
            reloadHistory();
            break;
        }
        case IDC_RESET:
            if (MessageBoxW(g.wnd, L"Cool the machine to ambient and discard any beans?", L"Reset", MB_YESNO) == IDYES) {
                cmd1("reset");
                reloadHistory();
            }
            break;
        case IDC_REPAIR: showError(cmd1("repair")); break;
        case IDC_REPORT: showReport(); break;
        case IDC_SPEED: {
            if (code != CBN_SELCHANGE) break;
            int i = static_cast<int>(SendMessageW(g.speedCombo, CB_GETCURSEL, 0, 0));
            const double speeds[] = {0, 1, 2, 5, 10, 30, 60};
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
                c["value"] = speeds[i];
            }
            showError(command(c));
            break;
        }
        case IDC_ENVAPPLY: {
            Value c = Value::object();
            c["cmd"] = "env";
            c["ambient_C"] = fromT(editNumber(g.ambientEdit));
            c["line_voltage"] = editNumber(g.voltsEdit);
            c["altitude_m"] = fromA(editNumber(g.altEdit));
            showError(command(c));
            break;
        }
        case IDC_SOUND:
            if (code == BN_CLICKED) applySound();
            break;
        case IDC_COOL:
            if (code == BN_CLICKED)
                rs_host_set_control(g.host, RS_CTL_COOLING, SendMessageW(g.coolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 100 : 0);
            break;
        case IDC_POWER:
            if (code == BN_CLICKED) {
                Value c = Value::object();
                c["cmd"] = "power";
                c["value"] = SendMessageW(g.powerCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                showError(command(c));
            }
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
    bool on = SendMessageW(g.soundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    float vol = static_cast<float>(SendMessageW(g.volSlider, TBM_GETPOS, 0, 0)) / 100.0f;
    rs_audio_set_mix(g.audio, vol, 1.0f, 1.0f);
    if (!on) {
        rs_audio_pause(g.audio);
        return;
    }
    char err[256] = {0};
    if (!rs_audio_play(g.audio, err, sizeof err)) {
        SendMessageW(g.soundCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        MessageBoxA(g.wnd, err, "Sound", MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------- window
void createWidgets() {
    g.roasterCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_ROASTER);
    g.beanCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BEAN);
    mk(L"BUTTON", L"Reload", BS_PUSHBUTTON | WS_TABSTOP, IDC_LOAD);
    g.desc = mk(L"STATIC", L"", SS_LEFT, IDC_DESC);
    SendMessageW(g.desc, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);
    g.batchEdit = mk(L"EDIT", L"100", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IDC_BATCH);
    mk(L"BUTTON", L"Charge (g)", BS_PUSHBUTTON | WS_TABSTOP, IDC_CHARGE);
    mk(L"BUTTON", L"Drop", BS_PUSHBUTTON | WS_TABSTOP, IDC_DROP);
    mk(L"BUTTON", L"Mark FC", BS_PUSHBUTTON | WS_TABSTOP, IDC_MARKFC);
    g.preheatEdit = mk(L"EDIT", L"200", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IDC_PREHEAT_T);
    mk(L"BUTTON", L"Preheat (°C)", BS_PUSHBUTTON | WS_TABSTOP, IDC_PREHEAT);
    mk(L"BUTTON", L"Cold reset", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESET);
    mk(L"BUTTON", L"Repair", BS_PUSHBUTTON | WS_TABSTOP, IDC_REPAIR);
    for (int i = 0; i < kSliderCount; ++i) {
        g.sliderLabels[i] = mk(L"STATIC", L"", SS_LEFT, IDC_SLABEL0 + i);
        g.sliders[i] = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_SLIDER0 + i);
        g.sliderValues[i] = mk(L"STATIC", L"0%", SS_RIGHT, IDC_SVALUE0 + i);
    }
    g.powerCheck = mk(L"BUTTON", L"Power", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_POWER);
    g.coolCheck = mk(L"BUTTON", L"Cooling tray fan", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_COOL);
    g.soundCheck = mk(L"BUTTON", L"Sound", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SOUND);
    SendMessageW(g.soundCheck, BM_SETCHECK, BST_CHECKED, 0);
    g.volSlider = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_VOLUME);
    SendMessageW(g.volSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g.volSlider, TBM_SETPOS, TRUE, 70);
    g.speedCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_SPEED);
    for (const wchar_t* s : {L"Pause", L"1x real time", L"2x", L"5x", L"10x", L"30x", L"60x"})
        SendMessageW(g.speedCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(g.speedCombo, CB_SETCURSEL, 1, 0);
    mk(L"BUTTON", L"Roast report", BS_PUSHBUTTON | WS_TABSTOP, IDC_REPORT);
    const wchar_t* envNames[] = {L"Ambient °C", L"Line volts", L"Altitude m"};
    for (int i = 0; i < 3; ++i) {
        HWND lbl = mk(L"STATIC", envNames[i], SS_LEFT, IDC_ENVLABEL + i);
        SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);
    }
    g.ambientEdit = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IDC_AMBIENT);
    g.voltsEdit = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IDC_VOLTS);
    g.altEdit = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IDC_ALT);
    mk(L"BUTTON", L"Apply env", BS_PUSHBUTTON | WS_TABSTOP, IDC_ENVAPPLY);
    g.readout = mk(L"RoastSimReadout", L"", 0, IDC_READOUT);
    g.status = mk(L"STATIC", L"", SS_LEFT, IDC_STATUS);
    SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);

    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_WEB, L"Open web control panel");
    AppendMenuW(file, MF_STRING, IDM_FOLDER, L"Open program folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"Exit");
    HMENU roast = CreatePopupMenu();
    AppendMenuW(roast, MF_STRING, IDC_CHARGE, L"Charge");
    AppendMenuW(roast, MF_STRING, IDC_DROP, L"Drop");
    AppendMenuW(roast, MF_STRING, IDM_COOLEND, L"End cooling");
    AppendMenuW(roast, MF_STRING, IDM_EMPTY, L"Empty roaster (discard beans)");
    AppendMenuW(roast, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(roast, MF_STRING, IDM_MARK_DRY, L"Mark DRY");
    AppendMenuW(roast, MF_STRING, IDM_MARK_FCS, L"Mark FC start");
    AppendMenuW(roast, MF_STRING, IDM_MARK_FCE, L"Mark FC end");
    AppendMenuW(roast, MF_STRING, IDM_MARK_SCS, L"Mark SC start");
    AppendMenuW(roast, MF_STRING, IDM_MARK_SCE, L"Mark SC end");
    AppendMenuW(roast, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(roast, MF_STRING, IDC_REPORT, L"Roast report");
    g.unitsMenu = CreatePopupMenu();
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_TEMP_C, L"Temperature: °C");
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_TEMP_F, L"Temperature: °F");
    AppendMenuW(g.unitsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_W_G, L"Weight: grams");
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_W_KG, L"Weight: kilograms");
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_W_OZ, L"Weight: ounces");
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_W_LB, L"Weight: pounds");
    AppendMenuW(g.unitsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_ALT_M, L"Altitude: metres");
    AppendMenuW(g.unitsMenu, MF_STRING, IDM_ALT_FT, L"Altitude: feet");
    HMENU faults = CreatePopupMenu();
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_OPEN, L"BT probe open circuit");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_SPIKES, L"BT probe spikes");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_DRIFT, L"BT probe drift");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_BT_NOISY, L"BT probe noisy");
    AppendMenuW(faults, MF_STRING, IDM_FAULT_ET_OPEN, L"ET probe open circuit");
    AppendMenuW(faults, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(faults, MF_STRING, IDM_FAULT_CLEAR, L"Clear probe faults");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"About / Artisan connection");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(roast), L"&Roast");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g.unitsMenu), L"&Units");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(faults), L"F&aults");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(g.wnd, bar);
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
            mm->ptMinTrackSize.y = S(760);
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_UI) refresh();
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == g.volSlider) applySound();
            else if (lp) onSlider(reinterpret_cast<HWND>(lp), LOWORD(wp));
            return 0;
        case WM_COMMAND: onCommand(LOWORD(wp), HIWORD(wp)); return 0;
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
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    rs_audio_destroy(g.audio);
    rs_host_stop(g.host);
    rs_host_destroy(g.host);
    return 0;
}
