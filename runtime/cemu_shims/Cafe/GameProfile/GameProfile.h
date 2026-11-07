// SheikahVision shim: per-game profile values Cemu ships for BotW (accurate MUL on).
#pragma once
#include "config/CemuConfig.h"
#include <cstdlib>
#include <string_view>

class GameProfile {
   public:
    AccurateShaderMulOption GetAccurateShaderMul() const { return AccurateShaderMulOption::True; }
    PositionInvariance GetPositionInvariance() const { return PositionInvariance::Auto; }
    // BOTW_BUFFER_CACHE=private|shared|host|redirect overrides Cemu's choice (Auto = DevicePrivate on Apple GPUs for BotW);
    // redirect: shared memory without blits, writes only where the GPU is done reading (LatteBufferCache.cpp)
    MetalBufferCacheMode GetBufferCacheMode() const {
        const char* e = getenv("BOTW_BUFFER_CACHE");
        if (!e) return MetalBufferCacheMode::Auto;
        std::string_view v = e;
        return v == "shared" ? MetalBufferCacheMode::DeviceShared
             : v == "host"   ? MetalBufferCacheMode::Host
             : v == "private" ? MetalBufferCacheMode::DevicePrivate
             : v == "redirect" ? MetalBufferCacheMode::SharedRedirect
                              : MetalBufferCacheMode::Auto;
    }
    bool GetShaderFastMath() const { return true; }
};
extern GameProfile* g_current_game_profile;
