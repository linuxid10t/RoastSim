// RoastSim native Windows shell.
//
// Talks to the simulator only through the C API (roastsim.h) plus the JSON
// helper for parsing replies, so it doubles as the reference for the GTK and
// Haiku BeAPI shells. Hosts the same servers as roastsimd (Modbus, WebSocket,
// web UI, optional TC4 serial), so Artisan can connect while you watch here.
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
#include <cstring>
#include <cstdio>
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
    IDC_ENVAPPLY, IDC_READOUT, IDC_STATUS, IDC_DESC, IDC_ENVLABEL, IDC_ENVL2, IDC_ENVL3, IDC_VOLUME,
    IDC_SLIDER0 = 200, IDC_SLABEL0 = 220, IDC_SVALUE0 = 240,
    IDM_EXIT = 1001, IDM_WEB, IDM_FOLDER, IDM_COOLEND, IDM_EMPTY,
    IDM_MARK_DRY = 1020, IDM_MARK_FCS, IDM_MARK_FCE, IDM_MARK_SCS, IDM_MARK_SCE,
    IDM_FAULT_BT_OPEN = 1040, IDM_FAULT_BT_SPIKES, IDM_FAULT_BT_DRIFT, IDM_FAULT_BT_NOISY, IDM_FAULT_ET_OPEN,
    IDM_FAULT_CLEAR, IDM_ABOUT = 1060,
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
    return fmt(L"%s%02d:%02d", s < 0 ? L"-" : L"", t / 60, t % 60);
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
    HWND ambientEdit, voltsEdit, altEdit;
    HWND sliders[kSliderCount], sliderLabels[kSliderCount], sliderValues[kSliderCount];
    HWND coolCheck;
    bool dragging[kSliderCount] = {};
    std::vector<HWND> leftWidgets;

    std::vector<std::string> roasterIds, beanIds;
    Value roasterDef;
    std::string currentRoaster;

    rs_state st{};
    std::vector<rs_sample> hist;
    uint64_t histNext = 0;
    int lastPhase = -1;
    std::vector<EventMark> events;
    uint64_t eventSeq = 0;
    rs_audio* audio = nullptr;  // procedural roaster sound (docs/SOUND.md)
    HWND volSlider = nullptr;
    bool reportShownForSeq = false;

    std::vector<rs_visual_bean> vbeans;
    rs_visual_info vinfo{};
    std::vector<float> beanX, beanY, beanPhase;
    int leftWidth = 380;
};
App g;
void applySound();

int S(int v) { return static_cast<int>(std::lround(v * g.dpi)); }

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

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, bool left = true) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    if (left) g.leftWidgets.push_back(h);
    return h;
}

// ---------------------------------------------------------------- roaster setup
void reloadHistory() {
    g.hist.clear();
    g.histNext = 0;
    g.events.clear();
    g.eventSeq = 0;
}

void applyRoasterDef() {
    Value st = Value::object();
    st["cmd"] = "state";
    st["full"] = true;
    Value s = command(st);
    g.roasterDef = s["roaster_def"];
    g.currentRoaster = s["roaster"].get("id", "");
    SetWindowTextW(g.desc, widen(g.roasterDef.get("description", "")).c_str());
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
            SendMessageW(g.sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(static_cast<int>(c.get("min", 0.0)), static_cast<int>(c.get("max", 100.0))));
            SendMessageW(g.sliders[i], TBM_SETPAGESIZE, 0, 10);
            SetWindowTextW(g.sliderLabels[i], widen(c.get("label", id.c_str())).c_str());
            ShowWindow(g.sliders[i], SW_SHOW);
            ShowWindow(g.sliderLabels[i], SW_SHOW);
            ShowWindow(g.sliderValues[i], SW_SHOW);
        }
    }
    double batch = s.get("next_batch_g", 100.0);
    SetWindowTextW(g.batchEdit, fmt(L"%.0f", batch).c_str());
    for (size_t i = 0; i < g.roasterIds.size(); ++i)
        if (g.roasterIds[i] == g.currentRoaster) SendMessageW(g.roasterCombo, CB_SETCURSEL, i, 0);
    std::string bean = s["bean"].get("id", "");
    for (size_t i = 0; i < g.beanIds.size(); ++i)
        if (g.beanIds[i] == bean) SendMessageW(g.beanCombo, CB_SETCURSEL, i, 0);
    reloadHistory();
    g.beanX.clear();
    InvalidateRect(g.wnd, nullptr, FALSE);
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
    // batch / charge / drop
    place(GetDlgItem(g.wnd, IDC_BATCH), pad, y, S(70), rowH);
    place(GetDlgItem(g.wnd, IDC_CHARGE), pad + S(76), y, S(90), rowH);
    place(GetDlgItem(g.wnd, IDC_DROP), pad + S(172), y, S(90), rowH);
    place(GetDlgItem(g.wnd, IDC_MARKFC), pad + S(268), y, w - S(268), rowH);
    y += rowH + gap;
    place(GetDlgItem(g.wnd, IDC_PREHEAT_T), pad, y, S(70), rowH);
    place(GetDlgItem(g.wnd, IDC_PREHEAT), pad + S(76), y, S(90), rowH);
    place(GetDlgItem(g.wnd, IDC_RESET), pad + S(172), y, S(90), rowH);
    place(GetDlgItem(g.wnd, IDC_REPAIR), pad + S(268), y, w - S(268), rowH);
    y += rowH + gap * 2;
    for (int i = 0; i < kSliderCount; ++i) {
        if (!IsWindowVisible(g.sliders[i])) continue;
        place(g.sliderLabels[i], pad, y + S(4), S(90), rowH);
        place(g.sliders[i], pad + S(92), y, w - S(92) - S(52), S(30));
        place(g.sliderValues[i], pad + w - S(48), y + S(4), S(48), rowH);
        y += S(32);
    }
    place(g.coolCheck, pad, y, half, rowH);
    place(g.soundCheck, pad + half + gap, y, half, rowH);
    y += rowH + gap;
    place(g.speedCombo, pad, y, S(110), S(300));
    place(GetDlgItem(g.wnd, IDC_REPORT), pad + S(116), y, S(110), rowH);
    place(g.volSlider, pad + S(232), y, w - S(232), rowH);
    y += rowH + gap;
    // environment
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
    InvalidateRect(g.wnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- refresh
void syncSliders() {
    for (int i = 0; i < kSliderCount; ++i) {
        if (!IsWindowVisible(g.sliders[i])) continue;
        double v = g.st.controls[kSliderControls[i]];
        if (!g.dragging[i] && GetFocus() != g.sliders[i])
            SendMessageW(g.sliders[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(v)));
        double act = g.st.actuators[kSliderControls[i]];
        SetWindowTextW(g.sliderValues[i], std::fabs(act - v) > 0.5 ? fmt(L"%.0f→%.0f", act, v).c_str() : fmt(L"%.0f%%", v).c_str());
    }
    SendMessageW(g.coolCheck, BM_SETCHECK, g.st.controls[RS_CTL_COOLING] >= 50 ? BST_CHECKED : BST_UNCHECKED, 0);
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
    std::wstring t;
    t += widen(r.get("roaster", "")) + L"\n" + widen(r.get("bean", "")) + L"\n\n";
    t += fmt(L"Batch %.0f g  ->  %.1f g   (loss %.1f%%, moisture %.1f%%)\n", r.get("batch_g", 0.0), r.get("out_g", 0.0),
             r.get("weight_loss_pct", 0.0), r.get("moisture_pct", 0.0));
    t += L"TP " + mmss(r.get("tp_s", -1.0)) + L"   DRY " + mmss(r.get("dry_s", -1.0)) + L"   FCs " +
         mmss(r.get("fcs_s", -1.0)) + L"   DROP " + mmss(r.get("drop_s", -1.0)) + L"\n";
    t += fmt(L"Development %.0f s  (DTR %.1f%%)\n", r.get("dev_time_s", 0.0), r.get("dtr_pct", 0.0));
    t += fmt(L"Agtron whole %.1f / ground %.1f  (spread %.1f)\n", r.get("agtron_whole", 0.0), r.get("agtron_ground", 0.0),
             r.get("agtron_sd", 0.0));
    t += L"Roast level: " + widen(r.get("roast_level", "")) + L"\n";
    t += fmt(L"RoR max %.1f, at FC %.1f, at drop %.1f C/min\n\n", r.get("max_ror", 0.0), r.get("ror_at_fcs", 0.0),
             r.get("ror_at_drop", 0.0));
    for (const auto& n : r["notes"].items()) t += L"• " + widen(n.str()) + L"\n";
    MessageBoxW(g.wnd, t.c_str(), L"Roast report", MB_ICONINFORMATION);
}

void refresh() {
    g.st.struct_size = sizeof(rs_state);
    if (!rs_host_get_state(g.host, &g.st)) return;
    // History: reload when a roast starts/ends or history was trimmed.
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

    // Events
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

    const wchar_t* phases[] = {L"idle (empty)", L"ROASTING", L"cooling", L"done"};
    std::wstring r;
    r += fmt(L"Roast time  %s    %s\r\n", mmss(g.st.t_roast).c_str(), phases[std::clamp(g.st.phase, 0, 3)]);
    r += fmt(L"BT %7.1f °C    RoR %6.1f °C/min\r\n", g.st.bt, g.st.ror_bt);
    r += fmt(L"ET %7.1f °C    RoR %6.1f °C/min\r\n", g.st.et, g.st.ror_et);
    if (g.st.bean_location != 0) {
        r += fmt(L"Beans: mean %.1f (surface %.1f, core %.1f)\r\n", g.st.bean_mean_c, g.st.bean_surface_c, g.st.bean_core_c);
        r += fmt(L"Moisture %.2f%%   loss %.1f%%\r\n", g.st.moisture_pct, g.st.weight_loss_pct);
        r += fmt(L"Agtron %.0f whole / %.0f ground (±%.1f)\r\n", g.st.agtron_whole, g.st.agtron_ground, g.st.agtron_sd);
        r += fmt(L"First crack %.0f%%   second %.0f%%\r\n", g.st.fc_frac * 100, g.st.sc_frac * 100);
    }
    r += fmt(L"Heater %.0f W   air %.2f g/s\r\n", g.st.heater_w, g.st.airflow_g_s);
    r += fmt(L"Inlet %.0f   exhaust %.0f   wall %.0f\r\n", g.st.inlet_c, g.st.exhaust_c, g.st.wall_c);
    r += fmt(L"Agitation %.2f   mixing %.0f%%   smoke %.0f%%\r\n", g.st.agitation, g.st.mix_quality * 100, g.st.smoke * 100);
    r += L"Alarms: " + alarmText(g.st.alarms) + L"\r\n";
    SetWindowTextW(g.readout, r.c_str());

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
        if (g.st.paused) s += L"   [PAUSED]";
        else if (g.st.time_scale != 1.0 && !g.st.logger_connected) s += fmt(L"   [%.0fx]", g.st.time_scale);
        SetWindowTextW(g.status, s.c_str());
    }

    // Visual parcels for the bean preview
    g.vinfo.struct_size = sizeof(rs_visual_info);
    g.vbeans.resize(400);
    int nb = rs_host_get_visual(g.host, &g.vinfo, g.vbeans.data(), 400);
    g.vbeans.resize(static_cast<size_t>(std::max(0, nb)));
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- painting
void paintGraph(HDC dc, RECT area) {
    HBRUSH bg = CreateSolidBrush(RGB(250, 249, 246));
    FillRect(dc, &area, bg);
    DeleteObject(bg);
    SelectObject(dc, g.small);
    SetBkMode(dc, TRANSPARENT);

    const int l = area.left + S(48), r = area.right - S(48), t = area.top + S(26), b = area.bottom - S(26);
    if (r <= l + 20 || b <= t + 20) return;
    // x range
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
    double tMax = 250;
    for (const auto& s : g.hist) tMax = std::max<double>(tMax, std::max(s.bt, s.et) + 10);
    tMax = std::ceil(tMax / 50.0) * 50.0;
    const double rorMax = 50;
    auto X = [&](double x) { return l + static_cast<int>((x - x0) / (x1 - x0) * (r - l)); };
    auto Y = [&](double v) { return b - static_cast<int>(v / tMax * (b - t)); };
    auto YR = [&](double v) { return b - static_cast<int>(std::clamp(v, -5.0, rorMax) / rorMax * (b - t)); };

    HPEN grid = CreatePen(PS_SOLID, 1, RGB(225, 222, 215));
    SelectObject(dc, grid);
    SetTextColor(dc, RGB(110, 105, 95));
    for (double v = 0; v <= tMax; v += 50) {
        MoveToEx(dc, l, Y(v), nullptr);
        LineTo(dc, r, Y(v));
        std::wstring s = fmt(L"%.0f", v);
        TextOutW(dc, area.left + S(8), Y(v) - S(7), s.c_str(), static_cast<int>(s.size()));
    }
    for (double v = 0; v <= rorMax; v += 10) {
        std::wstring s = fmt(L"%.0f", v);
        TextOutW(dc, r + S(6), YR(v) - S(7), s.c_str(), static_cast<int>(s.size()));
    }
    double step = (x1 - x0) > 900 ? 120 : 60;
    for (double x = std::ceil(x0 / step) * step; x <= x1; x += step) {
        MoveToEx(dc, X(x), t, nullptr);
        LineTo(dc, X(x), b);
        std::wstring s = mmss(x);
        TextOutW(dc, X(x) - S(14), b + S(6), s.c_str(), static_cast<int>(s.size()));
    }
    DeleteObject(grid);

    // heat % as a light area at the bottom
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
        SelectObject(dc, p);
        bool first = true;
        for (const auto& s : g.hist) {
            double x = tx(s);
            if (x < x0 || x > x1) { first = true; continue; }
            double v = get(s);
            if (!ror && v <= -0.5) { first = true; continue; }
            int py = ror ? YR(v) : Y(v);
            if (first) MoveToEx(dc, X(x), py, nullptr);
            else LineTo(dc, X(x), py);
            first = false;
        }
        DeleteObject(p);
    };
    series(RGB(70, 110, 200), S(2), [](const rs_sample& s) { return static_cast<double>(s.et); }, false);
    series(RGB(200, 60, 40), S(2), [](const rs_sample& s) { return static_cast<double>(s.bt); }, false);
    if (roast) {
        series(RGB(230, 150, 40), 1, [](const rs_sample& s) { return s.t_roast > 20 ? static_cast<double>(s.ror_bt) : -5.0; }, true);
        series(RGB(150, 120, 90), 1, [](const rs_sample& s) { return static_cast<double>(s.bean_true); }, false);
    }

    // event markers
    HPEN ev = CreatePen(PS_DOT, 1, RGB(90, 80, 70));
    SelectObject(dc, ev);
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
    DeleteObject(ev);

    // legend
    struct L { COLORREF c; const wchar_t* n; } leg[] = {{RGB(200, 60, 40), L"BT"}, {RGB(70, 110, 200), L"ET"},
                                                      {RGB(230, 150, 40), L"RoR (right axis)"},
                                                      {RGB(150, 120, 90), L"true bean temp"}};
    int lx = l;
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
    const wchar_t* caption = g.vinfo.bean_location == 2 ? L"Beans (cooling tray)"
                             : g.vinfo.bean_location == 1 ? L"Beans in the roaster (one dot per parcel; preview of the future 3D view)"
                                                          : L"No beans";
    TextOutW(dc, area.left + S(8), area.top + S(4), caption, static_cast<int>(wcslen(caption)));
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
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
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
}

void onPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g.wnd, &ps);
    RECT rc;
    GetClientRect(g.wnd, &rc);
    RECT right{S(g.leftWidth), 0, rc.right, rc.bottom};
    if (right.right > right.left && right.bottom > 0) {
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
    RECT left{0, 0, S(g.leftWidth), rc.bottom};
    FillRect(dc, &left, GetSysColorBrush(COLOR_BTNFACE));
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
            c["batch_g"] = _wtof(getText(g.batchEdit).c_str());
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
            Value c = Value::object();
            c["cmd"] = "preheat";
            c["target_C"] = _wtof(getText(GetDlgItem(g.wnd, IDC_PREHEAT_T)).c_str());
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
            c["ambient_C"] = _wtof(getText(g.ambientEdit).c_str());
            c["line_voltage"] = _wtof(getText(g.voltsEdit).c_str());
            c["altitude_m"] = _wtof(getText(g.altEdit).c_str());
            showError(command(c));
            break;
        }
        case 0x7F0: break;
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
                        L"Edit roastsim.json next to the .exe to change ports or bind to the LAN.",
                        L"About RoastSim", MB_ICONINFORMATION);
            break;
        case IDM_EXIT: DestroyWindow(g.wnd); break;
        default:
            if (id == IDC_SOUND) {
                if (code == BN_CLICKED) applySound();
                break;
            }
            if (id == 0x7F1) break;
            if (id == static_cast<int>(GetDlgCtrlID(g.coolCheck)) && code == BN_CLICKED) {
                bool on = SendMessageW(g.coolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                rs_host_set_control(g.host, RS_CTL_COOLING, on ? 100 : 0);
            }
    }
}

void onSlider(HWND slider, int code) {
    for (int i = 0; i < kSliderCount; ++i) {
        if (g.sliders[i] != slider) continue;
        g.dragging[i] = code == TB_THUMBTRACK;
        int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        rs_host_set_control(g.host, kSliderControls[i], pos);
        SetWindowTextW(g.sliderValues[i], fmt(L"%d%%", pos).c_str());
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
    g.batchEdit = mk(L"EDIT", L"100", WS_BORDER | ES_NUMBER | WS_TABSTOP, IDC_BATCH);
    mk(L"BUTTON", L"Charge (g)", BS_PUSHBUTTON | WS_TABSTOP, IDC_CHARGE);
    mk(L"BUTTON", L"Drop", BS_PUSHBUTTON | WS_TABSTOP, IDC_DROP);
    mk(L"BUTTON", L"Mark FC", BS_PUSHBUTTON | WS_TABSTOP, IDC_MARKFC);
    mk(L"EDIT", L"200", WS_BORDER | ES_NUMBER | WS_TABSTOP, IDC_PREHEAT_T);
    mk(L"BUTTON", L"Preheat (°C)", BS_PUSHBUTTON | WS_TABSTOP, IDC_PREHEAT);
    mk(L"BUTTON", L"Cold reset", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESET);
    mk(L"BUTTON", L"Repair", BS_PUSHBUTTON | WS_TABSTOP, IDC_REPAIR);
    for (int i = 0; i < kSliderCount; ++i) {
        g.sliderLabels[i] = mk(L"STATIC", L"", SS_LEFT, IDC_SLABEL0 + i);
        g.sliders[i] = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_SLIDER0 + i);
        g.sliderValues[i] = mk(L"STATIC", L"0%", SS_RIGHT, IDC_SVALUE0 + i);
    }
    g.coolCheck = mk(L"BUTTON", L"Cooling tray fan", BS_AUTOCHECKBOX | WS_TABSTOP, 0x7F2);
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
    {
        const wchar_t* envNames[] = {L"Ambient °C", L"Line volts", L"Altitude m"};
        for (int i = 0; i < 3; ++i) {
            HWND lbl = mk(L"STATIC", envNames[i], SS_LEFT, IDC_ENVLABEL + i);
            SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);
        }
    }
    g.ambientEdit = mk(L"EDIT", L"22", WS_BORDER | WS_TABSTOP, IDC_AMBIENT);
    g.voltsEdit = mk(L"EDIT", L"120", WS_BORDER | WS_TABSTOP, IDC_VOLTS);
    g.altEdit = mk(L"EDIT", L"0", WS_BORDER | WS_TABSTOP, IDC_ALT);
    SendMessageW(g.ambientEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"ambient °C"));
    SendMessageW(g.voltsEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"line V"));
    SendMessageW(g.altEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"altitude m"));
    mk(L"BUTTON", L"Apply env", BS_PUSHBUTTON | WS_TABSTOP, IDC_ENVAPPLY);
    g.readout = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_BORDER, IDC_READOUT);
    SendMessageW(g.readout, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono), TRUE);
    g.status = mk(L"STATIC", L"", SS_LEFT, IDC_STATUS);
    SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(g.small), TRUE);

    // Tooltips for the environment fields
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
            applyRoasterDef();
            layout();
            SetTimer(h, TIMER_UI, 200, nullptr);
            applySound();
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(900);
            mm->ptMinTrackSize.y = S(700);
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
            SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(lp) == g.readout) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
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

    // Optional roastsim.json next to the exe configures ports/bind address/etc.
    std::string cfgText;
    std::string cfgPath = roastsim::platform::executableDir() + "/roastsim.json";
    roastsim::readTextFile(cfgPath, cfgText);
    char err[1024] = {0};
    g.host = rs_host_create(cfgText.empty() ? nullptr : cfgText.c_str(), err, sizeof err);
    if (!g.host || !rs_host_start(g.host, err, sizeof err)) {
        MessageBoxA(nullptr, err, "RoastSim failed to start", MB_ICONERROR);
        return 1;
    }
    g.audio = rs_audio_create(g.host, 44100);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"RoastSimMain";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"RoastSim 2026 - virtual coffee roaster", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1280), S(820), nullptr, nullptr, inst, nullptr);
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
