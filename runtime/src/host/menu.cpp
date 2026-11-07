#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "../settings.h"

extern float g_render_scale;  // main.cpp: the scale this run renders at
extern "C" int botw_vr_enabled(void);   // game/vr_frame.cpp
extern "C" void botw_vr_recenter(void);  // game/vr_render.cpp

namespace menu {
namespace {
std::mutex g_lock;
std::atomic<bool> g_open{false};
int g_cursor = 0;

enum RowId { kFps, kScale, kDisplay, kFilter, kGyro, kRumble, kLayout, kTouch, kShowFps, kVolume, kVrTurn, kVrRecenter, kClose, kRows };
const char* kTurns[] = {"Smooth (right stick)", "Snap 30\u00b0", "Snap 45\u00b0", "Snap 60\u00b0"};

// the rows on show, in order: the VR ones only with VR on; the cursor is an index into this
const std::vector<int>& shown() {
    static const std::vector<int> rows = [] {
        std::vector<int> v;
        for (int r = 0; r < kRows; r++)
            if ((r != kVrTurn && r != kVrRecenter) || botw_vr_enabled()) v.push_back(r);
        return v;
    }();
    return rows;
}
const float kScales[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3};  // 0 = fit the display
const char* kDisplays[] = {"Fit 16:9", "Stretch", "Fill (crop)"};
const char* kFilters[] = {"Bilinear", "Bicubic", "Hermite", "Nearest"};

std::string scale_text(float s) {
    char b[64];
    if (s <= 0) return "Auto (fit the display)";
    snprintf(b, sizeof b, "%gx  (%dx%d)", s, (int)lroundf(1280 * s), (int)lroundf(720 * s));
    return b;
}

void step(Settings& s, int row, int dir) {
    auto wrap = [](int v, int n) { return (v % n + n) % n; };
    switch (row) {
        case kFps: s.fps = s.fps >= 60 ? 30 : 60; break;
        case kScale: {
            int i = 0;
            for (int k = 0; k < (int)std::size(kScales); k++)
                if (fabsf(kScales[k] - s.scale) < 0.01f) i = k;
            s.scale = kScales[wrap(i + dir, (int)std::size(kScales))];
            break;
        }
        case kDisplay: s.display = wrap(s.display + dir, 3); break;
        case kFilter: s.filter = wrap(s.filter + dir, 4); break;
        case kGyro: s.gyro = !s.gyro; break;
        case kRumble: s.rumble = !s.rumble; break;
        case kLayout: s.layout = !s.layout; break;
        case kTouch: s.touch = wrap(s.touch + dir, 3), s.touch_hidden = false; break;
        case kShowFps: s.show_fps = !s.show_fps; break;
        case kVolume: s.volume = std::clamp(s.volume + dir * 10, 0, 100); break;
        case kVrTurn: s.vr_turn = wrap(s.vr_turn + dir, 4); break;
    }
}
}  // namespace

std::atomic<float> g_px{-1}, g_py{-1};
std::atomic<bool> g_pdown{false};
void pointer(float x, float y, bool down) {
    g_px = x;
    g_py = y;
    g_pdown = down;
}
void pointer_state(float* x, float* y, bool* down) {
    *x = g_px;
    *y = g_py;
    *down = g_pdown;
}

std::atomic<float> g_safe[4] = {0, 0, 0, 0};
void set_safe_area(float l, float t, float r, float b) {
    g_safe[0] = l, g_safe[1] = t, g_safe[2] = r, g_safe[3] = b;
}
void safe_area(float* l, float* t, float* r, float* b) {
    *l = g_safe[0], *t = g_safe[1], *r = g_safe[2], *b = g_safe[3];
}

std::atomic<float> g_panel[4] = {0, 0, 0, 0}, g_xbtn[4] = {0, 0, 0, 0};
void set_rects(const float p[4], const float x[4]) {
    for (int i = 0; i < 4; i++) g_panel[i] = p[i], g_xbtn[i] = x[i];
}
void rects(float p[4], float x[4]) {
    for (int i = 0; i < 4; i++) p[i] = g_panel[i], x[i] = g_xbtn[i];
}
bool touch_close(float x, float y) {
    const float slop = 0.04f;  // the X is small for a thumb
    bool outside = x < g_panel[0] || y < g_panel[1] || x > g_panel[2] || y > g_panel[3];
    bool on_x = x > g_xbtn[0] - slop && y > g_xbtn[1] - slop && x < g_xbtn[2] + slop && y < g_xbtn[3] + slop;
    if (!g_open || !(outside || on_x)) return false;
    close();
    return true;
}

bool is_open() { return g_open; }
void toggle() { g_open = !g_open; }
void close() { g_open = false; }

void move(int dy) {
    std::lock_guard<std::mutex> l(g_lock);
    const int n = (int)shown().size();
    g_cursor = (g_cursor + dy + n) % n;
}

void change(int dir) {
    int row;
    {
        std::lock_guard<std::mutex> l(g_lock);
        row = shown()[g_cursor];
    }
    if (row == kClose || row == kVrRecenter) return;
    Settings s = settings::get();
    step(s, row, dir);
    settings::set(s);
}

void activate() {
    int row;
    {
        std::lock_guard<std::mutex> l(g_lock);
        row = shown()[g_cursor];
    }
    if (row == kClose) close();
    else if (row == kVrRecenter) botw_vr_recenter(), close();
    else change(+1);
}

void click(int row, int dir) {
    if (row < 0 || row >= (int)shown().size()) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_cursor = row;
    }
    if (dir == 0) activate();
    else change(dir);
}

View view() {
    View v;
    v.open = g_open;
    {
        std::lock_guard<std::mutex> l(g_lock);
        v.cursor = g_cursor;
    }
    Settings s = settings::get();
    char b[64];
    std::vector<Row> rows(kRows);
    rows[kFps] = {"Frame rate", s.fps >= 60 ? "60 fps" : "30 fps (original)"};
    rows[kScale] = {"Resolution", scale_text(s.scale)};
    rows[kDisplay] = {"Aspect", kDisplays[std::clamp(s.display, 0, 2)]};
    rows[kFilter] = {"Upscale filter", kFilters[std::clamp(s.filter, 0, 3)]};
    rows[kGyro] = {"Gyro aiming", s.gyro ? "On" : "Off"};
    rows[kRumble] = {"Rumble", s.rumble ? "On" : "Off"};
    rows[kLayout] = {"Buttons", s.layout ? "A = bottom (Cross / Xbox A)" : "A = right (Circle / Xbox B)"};
    rows[kTouch] = {"Touch controls", s.touch == 1 ? "On" : s.touch == 2 ? "Off" : "Auto (no controller)"};
    rows[kShowFps] = {"FPS counter", s.show_fps ? "On" : "Off"};
    snprintf(b, sizeof b, "%d%%", s.volume);
    rows[kVolume] = {"Volume", b};
    rows[kVrTurn] = {"VR turning", kTurns[std::clamp(s.vr_turn, 0, 3)]};
    rows[kVrRecenter] = {"Recenter VR view", "", true};
    rows[kClose] = {"Close", "", true};
    for (int r : shown()) v.rows.push_back(rows[r]);
    snprintf(b, sizeof b, "Rendering at %gx now; resolution changes apply after a restart.", g_render_scale);
    v.note = b;
    return v;
}
}  // namespace menu
