// SheikahVision shim: at most one built-in pack is active, the resolution scaling texture rules
// (graphic_pack_shim.cpp, --scale); no custom shaders.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/Core/LatteTextureView.h"

class RendererOutputShader;
class GraphicPack2 {
   public:
    enum class GP_SHADER_TYPE { VERTEX = 0, GEOMETRY = 1, PIXEL = 2 };
    struct TextureRule {
        struct FILTER_SETTINGS {
            enum class MEM1_FILTER { BOTH = 0, INSIDE = 1, OUTSIDE = 2 };
            std::vector<unsigned int> format_whitelist, format_blacklist;
            std::vector<int> tilemode_whitelist, tilemode_blacklist;
            int width = -1, height = -1, depth = -1;
            MEM1_FILTER inMEM1 = MEM1_FILTER::BOTH;
        } filter_settings;
        struct OVERWRITE_SETTINGS {
            int width = -1, height = -1, depth = -1;
            int format = -1;
            int lod_bias = -1;
            int relative_lod_bias = -1;
            int anistropic_value = -1;
        } overwrite_settings;
    };
    static const std::vector<std::shared_ptr<GraphicPack2>>& GetActiveGraphicPacks();
    static void WaitUntilReady() {}
    static const std::string* FindCustomShaderSource(uint64_t, uint64_t, GP_SHADER_TYPE, bool, bool) { return nullptr; }
    // false, as Cemu's BotW Graphics pack (its rules.txt has no colorbufferOptimizationAware): the rules
    // (botw_texture_rules.inc) are written for the padded sizes (heat wave 1024x512, bloom 256x128 / 128x64 / 64x32), so
    // with the optimization on (render targets cut to 1024x360, 256x90, ...) they missed, the heat wave's blurred copy was
    // never synced at any scale above 1.0, and the volcano sky showed a tiled noise pattern (device 2026-11-12, Fable 5.2)
    bool AllowRendertargetSizeOptimization() const { return false; }
    const std::vector<TextureRule>& GetTextureRules() const { return m_rules; }
    const std::string& GetName() const { return m_name; }
    RendererOutputShader* GetUpscalingShader(bool) const { return nullptr; }
    RendererOutputShader* GetOuputShader(bool) const { return nullptr; }
    RendererOutputShader* GetDownscalingShader(bool) const { return nullptr; }
    LatteTextureView::MagFilter GetUpscalingMagFilter() const { return LatteTextureView::MagFilter::kLinear; }
    LatteTextureView::MagFilter GetDownscalingMagFilter() const { return LatteTextureView::MagFilter::kLinear; }

    std::vector<TextureRule> m_rules;
    std::string m_name;
};
