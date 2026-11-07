// VR performance: work the second (right-eye) pass can share with the first. The shadow maps depend on the light,
// not on the eye, so the right pass keeps the left pass's cascades instead of updating and drawing them again
// (BetterVR patch_RND_StereoRendering_Optimizations.asm, docs/bettervr-patch-map.md O3..O7). With VR off the calls
// are made as before.
#include <cstdlib>
#include <cstring>

#include "hooks.h"
#include "vr.h"

namespace {
// BOTW_VR_SHARE_SHADOWS=0 draws the shadows in both passes (A/B measurements)
bool share_shadows() {
    static const bool on = [] {
        const char* e = getenv("BOTW_VR_SHARE_SHADOWS");
        return !(e && strcmp(e, "0") == 0);
    }();
    return on;
}
bool right_pass() { return vr::enabled() && vr::eye() == 1 && share_shadows(); }
}  // namespace

// O3: depthShadow::updateShadowMatrix (a new matrix would not match the cascades drawn in the left pass)
BOTW_SITE(0x039de028) {
    if (!right_pass()) rt_call(c, 0x03b0acd0);
}
// O4: modelSceneShadow::calcViewMaskedLightShadow
BOTW_SITE(0x039ac9b0) {
    if (!right_pass()) rt_call(c, 0x039dea48);
}
// O5..O7: the shadow map's size, depth buffer and draw (the cascades are drawn once, in the left pass)
BOTW_SITE(0x039e02f8) {
    if (!right_pass()) rt_call(c, 0x03b159f0);
}
BOTW_SITE(0x039e0304) {
    if (!right_pass()) rt_call(c, 0x03b0adc4);
}
BOTW_SITE(0x039e0310) {
    if (!right_pass()) rt_call(c, 0x03b0adf0);
}
