// BotWRecomp: built-in resolution scaling. The texture redefine rules of Cemu's BotW Graphics pack
// (botw_texture_rules.inc) resize the game's render targets by g_render_scale; Cemu's texture cache
// and render-target code then scale viewports and scissors to match.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/GraphicPack/GraphicPack2.h"

extern float g_render_scale;  // runtime: --scale
void log_write(const char* fmt, ...);  // runtime/src/log.cpp
extern "C" void botw_vr_crop(int eye, float* out);  // runtime/src/game/vr_render.cpp: BOTW_VR_FOV per edge

namespace {
struct Row {
    int width, height, depth;
    std::vector<unsigned int> formats, formats_excluded;
    std::vector<int> tilemodes, tilemodes_excluded;
    int overwrite_format;
    int ow;
    double ow_add;
    int oh;
    double oh_add;
};
const Row kRows[] = {
#include "botw_texture_rules.inc"
};
}  // namespace

const std::vector<std::shared_ptr<GraphicPack2>>& GraphicPack2::GetActiveGraphicPacks() {
    static std::vector<std::shared_ptr<GraphicPack2>> packs = [] {
        std::vector<std::shared_ptr<GraphicPack2>> v;
        // VR view mode: the render targets take the shape of an eye's field of view instead of 16:9 (BOTW_VR_ASPECT,
        // width over height, 1.25 by default for the Vision Pro's ~2.73 x 2.19 tangent frustum, 0 keeps 16:9), the
        // number of pixels kept: x and y scaled by sqrt(aspect / (16/9)) and its inverse. A 16:9 picture stretched over
        // that field had ~40 % fewer pixels per degree up and down than across (Fable 5.1, 2026-11-02); evened out,
        // the weaker vertical axis gains ~19 %. The eye projection takes its aspect from the tangents (vr_render.cpp
        // apply_frustum), and the compositor maps the whole picture onto the eye, the HUD layer and the screen in
        // front (title, menus, cutscenes) onto 16:9 quads, so all of them come out undistorted
        float sx = g_render_scale, sy = g_render_scale;
        {
            const char* vr = getenv("BOTW_VR");
            const char* mode = getenv("BOTW_VR_MODE");
            const char* a = getenv("BOTW_VR_ASPECT");
            float aspect = a ? (float)atof(a) : 1.25f;
            if (vr && !strcmp(vr, "1") && mode && !strcmp(mode, "view") && aspect > 0) {
                // BOTW_VR_FOV per edge (vr_render.cpp): the aspect scaled by how much the crop narrows the field across
                // against up and down, on the Mac pose's tangents (55 / 45 degrees out / in, 45 up, 50 down); the
                // headset's differ a little, so with very uneven crops pixels can be 1-2 % off square (Fable 5.2,
                // 2026-11-10: the picture is not distorted, the projection and the warp use the real tangents)
                // the right eye's crop (mirrored: its left edge is the inner one): the left eye's inner edge is drawn
                // whole for the system's recordings (vr_render.cpp), which must not change the targets' shape
                float crop[4];
                botw_vr_crop(1, crop);
                const float across = (1.43f * crop[1] + 1.0f * crop[0]) / 2.43f, up = (1.0f * crop[2] + 1.19f * crop[3]) / 2.19f;
                aspect *= across / up;
                const float k = sqrtf(aspect / (16.0f / 9.0f));
                sx = g_render_scale * k, sy = g_render_scale / k;
                log_write("render targets %.3f x %.3f (eye shape %.2f:1, %dx%d per eye)", sx, sy, aspect,
                          (int)(1280 * sx), (int)(720 * sy));
            }
        }
        if (sx == 1.0f && sy == 1.0f) return v;
        // 0.75 unless set (the launcher has set 0.75 for every preset since 2026-11-11, about 1 ms a frame on the
        // headset). A value at or above the scale keeps them at the picture's scale. (Kakariko's occasional white river,
        // item 95, first looked tied to this; later runs showed it is not: 3/35 at the picture's scale, 1/22 at 0.75)
        float small_ratio = 1.0f;
        {
            const char* cap = getenv("BOTW_SMALL_BUFFER_SCALE");
            const float c = cap ? (float)atof(cap) : 0.75f;
            if (c > 0 && c < g_render_scale) small_ratio = c / g_render_scale;
            log_write("small render targets: scale %.3f of %.3f", g_render_scale * small_ratio, g_render_scale);
        }
        auto gp = std::make_shared<GraphicPack2>();
        gp->m_name = "SheikahVision resolution";
        for (const Row& r : kRows) {
            TextureRule rule;
            auto& f = rule.filter_settings;
            f.width = r.width;
            f.height = r.height;
            f.depth = r.depth;
            f.format_whitelist = r.formats;
            f.format_blacklist = r.formats_excluded;
            f.tilemode_whitelist = r.tilemodes;
            f.tilemode_blacklist = r.tilemodes_excluded;
            auto& o = rule.overwrite_settings;
            // BOTW_SMALL_BUFFER_SCALE: the targets smaller than the picture (bloom, fog, half-size lighting steps) stop
            // growing at this scale; headset counters 2026-11-08: their fragment time 2.7 / 4.0 / 5.6 ms at 1.0 / 1.5 / 2.0
            // except the 1024x512 and 512x256 R11G11B10F targets: the 1024x512 one is mip 1 of the 1280x720 scene colour
            // (the heat wave renders its blur there and samples it by LOD), so it must share the picture's scale; at 0.75
            // Latte refuses to copy it into that mip (rescale ratio check, LatteTexture.cpp) and the volcano sky shows a
            // tiled grey-green pattern again (headset and Mac 2026-11-12, item 90; Fable 5.2)
            const bool heat_wave = r.formats.size() == 1 && r.formats[0] == 0x816 &&
                                   ((r.width == 1024 && r.height == 512) || (r.width == 512 && r.height == 256));
            const float small = r.ow < 1280 && r.oh < 720 && !heat_wave ? small_ratio : 1.0f;
            o.width = (int)(sx * small * r.ow + r.ow_add);  // the pack's expressions truncate
            o.height = (int)(sy * small * r.oh + r.oh_add);
            o.format = r.overwrite_format;
            gp->m_rules.push_back(rule);
        }
        v.push_back(gp);
        return v;
    }();
    return packs;
}

// Picture experiments BOTW_NO_FXAA=1 / BOTW_NO_DOF=1 (launcher, off by default): the game's own full-screen FXAA and
// depth-of-field passes, identified by the pixel shader hashes BetterVR's Graphics pack replaces for the same purpose
// (BotW-BetterVR/resources/BreathOfTheWild_Graphics). Cemu's community packs agree: Graphics "Anti-Aliasing: None"
// makes f14bb57cd5c9cb77 the same copy, Enhancements "Depth Of Field: Disabled" replaces cb0e6e8cbec4502a, and their
// Metal versions carry the aux hashes Latte computes here (ff060c183153097b, ff060c183060c54b). The Metal
// source Latte generates for them is edited before it is compiled (LatteShader.cpp), only when every name the edit
// needs is present; otherwise the shader is left as it is and the log says so.
//   f14bb57cd5c9cb77 world FXAA: its output becomes a straight copy of its input (the pack's "AA off": a skipped pass
//                    would leave its target black)
//   c92c1c4c0a2fb839 depth-of-field blur: it only writes the out-of-focus pixels, blended over the sharp image, so
//                    discarding every pixel leaves the sharp image everywhere
//   cb0e6e8cbec4502a one-pass DOF of lock-on, camera rune and scope: it fills a target composited later, so it keeps
//                    writing, the centre tap instead of a disc of taps
namespace {
bool env_on(const char* name) {
    const char* e = getenv(name);
    return e && strcmp(e, "1") == 0;
}
bool insert_before_return(std::string& src, const char* code) {
    size_t at = src.rfind("return out;");
    if (at == std::string::npos) return false;
    src.insert(at, code);
    return true;
}
bool has_all(const std::string& src, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (src.find(n) == std::string::npos) return false;
    return true;
}
}  // namespace

void botw_patch_pixel_shader(uint64_t baseHash, uint64_t auxHash, std::string& src) {
    static const bool noFxaa = env_on("BOTW_NO_FXAA"), noDof = env_on("BOTW_NO_DOF");
    const char* what = nullptr;
    bool ok = false;
    if (noFxaa && baseHash == 0xf14bb57cd5c9cb77ull) {
        what = "FXAA (copy of its input)";
        ok = has_all(src, {"float4 passPixelColor0 [[color(0)]]", "texture2d<float> tex0", "samplr0",
                           "in.passParameterSem2"}) &&
             insert_before_return(src, "out.passPixelColor0 = tex0.sample(samplr0, in.passParameterSem2.xy);\n");
    } else if (noDof && baseHash == 0xc92c1c4c0a2fb839ull) {
        what = "depth of field (every pixel discarded, the sharp image stays)";
        ok = insert_before_return(src, "discard_fragment();\n");
    } else if (noDof && baseHash == 0xcb0e6e8cbec4502aull) {
        what = "lock-on / camera / scope depth of field (centre tap)";
        ok = has_all(src, {"float4 passPixelColor0 [[color(0)]]", "texture2d<float> tex0", "samplr0",
                           "in.passParameterSem3"}) &&
             insert_before_return(src, "out.passPixelColor0 = float4(tex0.sample(samplr0, (in.passParameterSem3.xy + "
                                       "in.passParameterSem3.zw) / 2.0).xyz, 0.0);\n");
    }
    if (what)
        log_write("[picture] pixel shader %016llx_%016llx %s: %s", (unsigned long long)baseHash,
                  (unsigned long long)auxHash, what, ok ? "bypassed" : "NOT patched (unexpected source, left as is)");
}
