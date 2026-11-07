#include <algorithm>
#include <unordered_map>
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
void botw_gpu_busy_add(double start, double end);

// BotWRecomp: BOTW_DUMP_PASSES=<frame>: every render pass of that one frame (its first colour target or depth, the draws
// in it), markers for the eye copies, and each pass's GPU time (Metal counter samples at the stage boundaries: the
// vertex / tiling stage and the fragment stage of every render encoder, the whole of every blit / compute encoder) to
// see what the GPU's time per frame goes to. Printed once the frame's last command buffer has completed, with sums per
// target shape.
#include <mach/mach_time.h>
static int g_dump_at = [] { const char* e = getenv("BOTW_DUMP_PASSES"); return e ? atoi(e) : 0; }();
// BOTW_DUMP_COUNT=<k> dumps k frames from that one on, BOTW_DUMP_EVERY=<n> frames apart (default 1 frame, 30 apart):
// single frames differ by 10-15 %, so average several (scratchpad analyzer: one "end of frame" line per frame)
static int g_dump_count = [] { const char* e = getenv("BOTW_DUMP_COUNT"); return e ? std::max(atoi(e), 1) : 1; }();
static int g_dump_every = [] { const char* e = getenv("BOTW_DUMP_EVERY"); return e ? std::max(atoi(e), 1) : 30; }();
static int g_dump_frame = 0;
// the reason of the next blit encoder, for the dump (set by the callers of GetBlitCommandEncoder)
const char* g_botw_blit_reason = "blit";
static bool g_dumping = false;
static int g_pass_draws = -1, g_pass_geom = 0;
static std::unordered_map<uint64, int> g_pass_shaders;  // BOTW_DUMP_PASSES: the pixel shaders of the pass, by hash
static std::string g_pass_desc;
struct DumpPass { std::string desc; int draws; int sample; int kind; int geom = 0; std::string shaders; };  // kind 0 render, 1 blit, 2 compute, 3 marker
static std::vector<DumpPass> g_dump_passes;
static MTL::CounterSampleBuffer* g_dump_counters = nullptr;
static constexpr int kDumpSamples = 4096;  // Metal allows 32 KB of samples
static int g_dump_next_sample = 0;
static bool g_dump_counters_tried = false;
static MTL::Timestamp g_dump_cpu0, g_dump_gpu0;
static MTL::Device* g_dump_device = nullptr;
static void dump_counters_init(MTL::Device* device)
{
    if (g_dump_counters_tried) return;
    g_dump_counters_tried = true;
    g_dump_device = device;
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary))
    {
        cemuLog_log(LogType::Force, "pass timing: this device does not sample counters at stage boundaries");
        return;
    }
    MTL::CounterSet* timestamps = nullptr;
    NS::Array* sets = device->counterSets();
    for (NS::UInteger i = 0; sets && i < sets->count(); i++)
    {
        auto* cs = (MTL::CounterSet*)sets->object(i);
        if (cs->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) timestamps = cs;
    }
    if (!timestamps) { cemuLog_log(LogType::Force, "pass timing: no timestamp counter set"); return; }
    auto* d = MTL::CounterSampleBufferDescriptor::alloc()->init();
    d->setCounterSet(timestamps);
    d->setStorageMode(MTL::StorageModeShared);
    d->setSampleCount(kDumpSamples);
    NS::Error* err = nullptr;
    g_dump_counters = device->newCounterSampleBuffer(d, &err);
    d->release();
    if (!g_dump_counters) cemuLog_log(LogType::Force, "pass timing: counter sample buffer failed: {}", err ? err->localizedDescription()->utf8String() : "?");
}
static int dump_take_samples(int n)
{
    if (!g_dumping || !g_dump_counters || g_dump_next_sample + n > kDumpSamples) return -1;
    int s = g_dump_next_sample;
    g_dump_next_sample += n;
    return s;
}
static void dump_pass_begin(MTL::RenderPassDescriptor* d)
{
    if (!g_dumping) return;
    MTL::Texture* t = d->colorAttachments()->object(0)->texture();
    const char* kind = "colour";
    if (!t) { t = d->depthAttachment()->texture(); kind = "depth"; }
    g_pass_desc = t ? fmt::format("{} {}x{} fmt {}", kind, t->width(), t->height(), (int)t->pixelFormat()) : "none";
    int n = 0;
    for (int i = 0; i < 8; i++) if (d->colorAttachments()->object(i)->texture()) n++;
    g_pass_desc += fmt::format(" ({} colour, depth {})", n, d->depthAttachment()->texture() ? "yes" : "no");
    g_pass_draws = 0;
    g_pass_shaders.clear();
    const int s = dump_take_samples(4);
    auto* a = d->sampleBufferAttachments()->object(0);
    if (s >= 0)
    {
        a->setSampleBuffer(g_dump_counters);
        a->setStartOfVertexSampleIndex(s);
        a->setEndOfVertexSampleIndex(s + 1);
        a->setStartOfFragmentSampleIndex(s + 2);
        a->setEndOfFragmentSampleIndex(s + 3);
    }
    else
        a->setSampleBuffer(nullptr);
    g_dump_passes.push_back({g_pass_desc, 0, s, 0});
}
// after the encoder is made: the descriptor is cached and reused, so the sample attachment must not stay on it
static void dump_pass_started(MTL::RenderPassDescriptor* d)
{
    if (g_dumping) d->sampleBufferAttachments()->object(0)->setSampleBuffer(nullptr);
}
static void dump_pass_end()
{
    if (g_dumping && g_pass_draws >= 0 && !g_dump_passes.empty()) g_dump_passes.back().draws = g_pass_draws, g_dump_passes.back().geom = g_pass_geom;
    if (g_dumping && g_pass_draws >= 0 && !g_dump_passes.empty() && !g_pass_shaders.empty())
    {
        std::vector<std::pair<int, uint64>> top;
        for (const auto& [hash, n] : g_pass_shaders) top.push_back({n, hash});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 5; i++)
            g_dump_passes.back().shaders += fmt::format(" {:016x}x{}", top[i].second, top[i].first);
    }
    g_pass_draws = -1, g_pass_geom = 0;
}
// a blit / compute encoder of the dumped frame: both samples of its run
template <typename Desc>
static Desc* dump_encoder_descriptor(int kind)
{
    const int s = dump_take_samples(2);
    if (s < 0) return nullptr;
    auto* d = Desc::alloc()->init();
    auto* a = d->sampleBufferAttachments()->object(0);
    a->setSampleBuffer(g_dump_counters);
    a->setStartOfEncoderSampleIndex(s);
    a->setEndOfEncoderSampleIndex(s + 1);
    g_dump_passes.push_back({kind == 1 ? std::string("blit: ") + g_botw_blit_reason : "compute", 0, s, kind});
    return d;
}
extern "C" void botw_metal_dump_mark(const char* what)
{
    if (g_dumping) g_dump_passes.push_back({what, 0, -1, 3});
}
// the dumped frame is over and its last command buffer committed: wait for the GPU, resolve the samples, print
static void dump_frame_print(MTL::CommandBuffer* last, int frame)
{
    if (last) last->waitUntilCompleted();
    std::vector<uint64> ts;
    if (g_dump_counters && g_dump_next_sample > 0)
    {
        NS::Data* data = g_dump_counters->resolveCounterRange(NS::Range::Make(0, g_dump_next_sample));
        if (data)
        {
            ts.resize(data->length() / sizeof(uint64));
            memcpy(ts.data(), data->mutableBytes(), ts.size() * sizeof(uint64));
        }
    }
    // GPU timestamps to milliseconds: the ratio of the CPU clock (nanoseconds) to the GPU clock over this frame
    MTL::Timestamp cpu1 = 0, gpu1 = 0;
    if (g_dump_device) g_dump_device->sampleTimestamps(&cpu1, &gpu1);
    double scale = 1e-6;
    if (gpu1 > g_dump_gpu0 && cpu1 > g_dump_cpu0) scale = (double)(cpu1 - g_dump_cpu0) / (double)(gpu1 - g_dump_gpu0) * 1e-6;
    auto ms = [&](int a, int b) -> double {
        if (a < 0 || b >= (int)ts.size() || ts[a] == 0 || ts[b] == 0 || ts[b] < ts[a]) return 0.0;
        return (double)(ts[b] - ts[a]) * scale;
    };
    uint64 first = ~0ull, lastEnd = 0;
    double sumV = 0, sumF = 0, sumBlit = 0, sumCompute = 0;
    std::map<std::string, std::array<double, 4>> perShape;  // shape -> passes, draws, vertex ms, fragment ms
    int eye = -1;  // -1 before any eye marker; the passes before the marker belong to that eye
    std::array<std::array<double, 4>, 3> perEye{};  // [right, left, after] passes, draws, vertex, fragment
    int eyeSlot = 0;
    for (const auto& p : g_dump_passes)
    {
        if (p.kind == 3)
        {
            cemuLog_log(LogType::Force, "pass: --- {}", p.desc);
            if (eyeSlot < 2) eyeSlot++;
            continue;
        }
        if (p.kind == 0)
        {
            const double v = ms(p.sample, p.sample + 1), f = ms(p.sample + 2, p.sample + 3);
            sumV += v, sumF += f;
            cemuLog_log(LogType::Force, "pass: {} draws ({} geometry-like), {}, vertex {:.3f} ms, fragment {:.3f} ms{}{}", p.draws, p.geom, p.desc, v, f, p.shaders.empty() ? "" : ", shaders", p.shaders);
            auto& sh = perShape[p.desc];
            sh[0]++, sh[1] += p.draws, sh[2] += v, sh[3] += f;
            auto& e = perEye[eyeSlot];
            e[0]++, e[1] += p.draws, e[2] += v, e[3] += f;
            if (p.sample >= 0 && p.sample + 3 < (int)ts.size())
            {
                for (int k = 0; k < 4; k++) if (ts[p.sample + k]) { first = std::min(first, ts[p.sample + k]); lastEnd = std::max(lastEnd, ts[p.sample + k]); }
            }
        }
        else
        {
            const double t = ms(p.sample, p.sample + 1);
            (p.kind == 1 ? sumBlit : sumCompute) += t;
            cemuLog_log(LogType::Force, "pass: {} encoder {:.3f} ms", p.desc, t);
            if (p.sample >= 0 && p.sample + 1 < (int)ts.size() && ts[p.sample] && ts[p.sample + 1])
            {
                first = std::min(first, ts[p.sample]);
                lastEnd = std::max(lastEnd, ts[p.sample + 1]);
            }
        }
    }
    std::vector<std::pair<std::string, std::array<double, 4>>> shapes(perShape.begin(), perShape.end());
    std::sort(shapes.begin(), shapes.end(), [](auto& a, auto& b) { return a.second[2] + a.second[3] > b.second[2] + b.second[3]; });
    for (const auto& [desc, sh] : shapes)
        cemuLog_log(LogType::Force, "pass sum: {} passes, {} draws, vertex {:.2f} ms, fragment {:.2f} ms: {}", (int)sh[0], (int)sh[1], sh[2], sh[3], desc);
    const char* eyeName[3] = {"before the first eye marker", "between the markers", "after the second marker"};
    for (int i = 0; i < 3; i++)
        cemuLog_log(LogType::Force, "pass eye {}: {} passes, {} draws, vertex {:.2f} ms, fragment {:.2f} ms", eyeName[i], (int)perEye[i][0], (int)perEye[i][1], perEye[i][2], perEye[i][3]);
    cemuLog_log(LogType::Force, "pass: === end of frame {}: {} encoders; render vertex {:.2f} ms + fragment {:.2f} ms, blit {:.2f} ms, compute {:.2f} ms; GPU span first to last sample {:.2f} ms (stages overlap)",
        frame, (int)g_dump_passes.size(), sumV, sumF, sumBlit, sumCompute, (first < lastEnd) ? (double)(lastEnd - first) * scale : 0.0);
    g_dump_passes.clear();
    g_dump_next_sample = 0;
}
  // below: the game's GPU time for the [gpu] log line
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalDepthStencilCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalSamplerCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalQuery.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/UtilityShaderSource.h"

#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "config/CemuConfig.h"

// SheikahVision VR stereo, on the Latte thread: the eye the command stream draws (IT_HLE_BOTW_VR_EYE, vr_frame.cpp) and
// the eye picture's size (from the eye copies). A draw is "geometry-like" when its targets have the eye picture's
// size and its pixel shader samples no texture of that size (the G-buffer and depth passes: scene geometry whose
// result for the other eye differs only by the camera), as opposed to the screen-space passes (lighting, post) that
// read the eye's own buffers. BOTW_VR_STEREO_SKIP=1 (measurement only, wrong picture) drops the right eye's
// geometry-like draws to bound what single-pass stereo could save.
static int g_latte_eye = -1;
static uint32 g_eye_w = 0, g_eye_h = 0;
static int g_stereo_skip = [] { const char* e = getenv("BOTW_VR_STEREO_SKIP"); return e ? atoi(e) : 0; }();
extern "C" void botw_latte_set_eye(int eye) { g_latte_eye = eye; }
extern "C" void botw_latte_note_eye_view(LatteTextureView* view)
{
    MTL::Texture* t = static_cast<LatteTextureViewMtl*>(view)->GetRGBAView();
    g_eye_w = (uint32)t->width(), g_eye_h = (uint32)t->height();
}
static bool texture_is_eye_sized(LatteTextureView* view)
{
    if (!view) return false;
    MTL::Texture* t = static_cast<LatteTextureViewMtl*>(view)->GetRGBAView();
    return t->width() == g_eye_w && t->height() == g_eye_h;
}

// BotWRecomp: deferred clears (BOTW_MERGE_CLEARS=1). The game clears a target right before drawing into it; Cemu
// runs each clear as a render pass of its own (0 draws) and the draws then start another pass that loads what was just
// cleared. Here a clear is only noted, and the next render pass whose attachment is that texture starts with a clear
// load action instead (one encoder less, no load). Any other GPU work first (another target, a blit, a compute pass,
// a commit) runs the noted clears as before, in order, so nothing can observe the difference.
// BOTW_MERGE_CLEARS=2 also lets a noted clear wait past work that does not touch its texture: on the headset the game
// clears four eye-sized targets right after each eye's copy, then draws to other targets first, so none merged and each
// clear was a full-resolution pass of its own (2026-11-08: 13 such passes a frame, 2.9 ms at 2.0x). The clear runs at the
// latest when a draw samples the texture, a blit or copy touches it, or any encoder not known to leave it alone starts.
static int g_merge_clears = [] { const char* e = getenv("BOTW_MERGE_CLEARS"); return e ? atoi(e) : 0; }();
struct PendingClear
{
    MTL::Texture* texture;  // retained
    int slice, level;
    bool color;
    MTL::ClearColor clearColor;
    bool depth, stencil;
    double depthValue;
    uint32 stencilValue;
};
static std::vector<PendingClear> g_pending_clears;
static std::vector<std::pair<int, int>> g_merged_attachments;  // (kind 0 colour / 1 depth / 2 stencil, index) set to clear on the next descriptor
uint64 g_botw_clears_merged = 0, g_botw_clears_standalone = 0;
// the textures the next blit encoder touches (botw_blit_touches, set just before GetBlitCommandEncoder); not set: unknown
static MTL::Texture* g_blit_touch[2] = {nullptr, nullptr};
static bool g_blit_touch_known = false;
void botw_blit_touches(MTL::Texture* a, MTL::Texture* b) { g_blit_touch[0] = a, g_blit_touch[1] = b, g_blit_touch_known = true; }
static MTL::Texture* texture_base(MTL::Texture* t) { return t && t->parentTexture() ? t->parentTexture() : t; }
static bool descriptor_attaches(MTL::RenderPassDescriptor* d, MTL::Texture* tex)
{
    for (int i = 0; i < 8; i++)
        if (texture_base(d->colorAttachments()->object(i)->texture()) == tex) return true;
    return texture_base(d->depthAttachment()->texture()) == tex || texture_base(d->stencilAttachment()->texture()) == tex;
}
static bool attachment_matches(MTL::Texture* att, NS::UInteger attSlice, NS::UInteger attLevel, const PendingClear& p)
{
    if (!att) return false;
    MTL::Texture* base = att->parentTexture() ? att->parentTexture() : att;
    if (base != p.texture || att->pixelFormat() != p.texture->pixelFormat()) return false;  // a format-changing view clears differently
    return (int)(attSlice + att->parentRelativeSlice()) == p.slice && (int)(attLevel + att->parentRelativeLevel()) == p.level;
}
static void pending_clear_run(MTL::CommandBuffer* cb, const PendingClear& p)
{
    NS_STACK_SCOPED MTL::RenderPassDescriptor* d = MTL::RenderPassDescriptor::alloc()->init();
    if (p.color)
    {
        auto a = d->colorAttachments()->object(0);
        a->setTexture(p.texture), a->setClearColor(p.clearColor), a->setLoadAction(MTL::LoadActionClear), a->setStoreAction(MTL::StoreActionStore);
        a->setSlice(p.slice), a->setLevel(p.level);
    }
    if (p.depth)
    {
        auto a = d->depthAttachment();
        a->setTexture(p.texture), a->setClearDepth(p.depthValue), a->setLoadAction(MTL::LoadActionClear), a->setStoreAction(MTL::StoreActionStore);
        a->setSlice(p.slice), a->setLevel(p.level);
    }
    if (p.stencil)
    {
        auto a = d->stencilAttachment();
        a->setTexture(p.texture), a->setClearStencil(p.stencilValue), a->setLoadAction(MTL::LoadActionClear), a->setStoreAction(MTL::StoreActionStore);
        a->setSlice(p.slice), a->setLevel(p.level);
    }
    auto pool = NS::AutoreleasePool::alloc()->init();
    dump_pass_begin(d);
    MTL::RenderCommandEncoder* enc = cb->renderCommandEncoder(d);
    dump_pass_started(d);
    enc->endEncoding();
    dump_pass_end();
    pool->release();
    g_botw_clears_standalone++;
}
// before the next encoder: the noted clears either become the load action of `next`'s matching attachments (when
// next is a render pass descriptor) or run now as passes of their own
static void flush_pending_clears(MTL::CommandBuffer* cb, MTL::RenderPassDescriptor* next)
{
    std::vector<PendingClear> waiting;
    for (auto& p : g_pending_clears)
    {
        bool merged = false;
        if (next)
        {
            if (p.color)
            {
                for (int i = 0; i < 8 && !merged; i++)
                {
                    auto a = next->colorAttachments()->object(i);
                    if (a->loadAction() == MTL::LoadActionLoad && attachment_matches(a->texture(), a->slice(), a->level(), p))
                    {
                        a->setLoadAction(MTL::LoadActionClear), a->setClearColor(p.clearColor);
                        g_merged_attachments.push_back({0, i});
                        merged = true;
                    }
                }
            }
            else
            {
                auto da = next->depthAttachment();
                auto sa = next->stencilAttachment();
                const bool dOk = !p.depth || (da->loadAction() == MTL::LoadActionLoad && attachment_matches(da->texture(), da->slice(), da->level(), p));
                const bool sOk = !p.stencil || (sa->loadAction() == MTL::LoadActionLoad && attachment_matches(sa->texture(), sa->slice(), sa->level(), p));
                if (dOk && sOk)
                {
                    if (p.depth) da->setLoadAction(MTL::LoadActionClear), da->setClearDepth(p.depthValue), g_merged_attachments.push_back({1, 0});
                    if (p.stencil) sa->setLoadAction(MTL::LoadActionClear), sa->setClearStencil(p.stencilValue), g_merged_attachments.push_back({2, 0});
                    merged = true;
                }
            }
        }
        if (merged) g_botw_clears_merged++;
        else if (g_merge_clears == 2 && next && !descriptor_attaches(next, p.texture)) { waiting.push_back(p); continue; }
        else pending_clear_run(cb, p);
        p.texture->release();
    }
    g_pending_clears = std::move(waiting);
}
// mode 2: run now the noted clears of these textures (the others keep waiting); nullptr entries are ignored
static bool pending_clear_touches(MTL::Texture* t)
{
    t = texture_base(t);
    if (t)
        for (const auto& p : g_pending_clears)
            if (p.texture == t) return true;
    return false;
}
static void run_pending_clears_of(MTL::CommandBuffer* cb, MTL::Texture* a, MTL::Texture* b)
{
    a = texture_base(a), b = texture_base(b);
    std::vector<PendingClear> waiting;
    for (auto& p : g_pending_clears)
    {
        if (p.texture == a || p.texture == b) { pending_clear_run(cb, p); p.texture->release(); }
        else waiting.push_back(p);
    }
    g_pending_clears = std::move(waiting);
}
// after the encoder is made from `d` (cached descriptors are reused): back to plain loads
static void pending_clears_restore(MTL::RenderPassDescriptor* d)
{
    for (auto [kind, i] : g_merged_attachments)
    {
        if (kind == 0) d->colorAttachments()->object(i)->setLoadAction(MTL::LoadActionLoad);
        else if (kind == 1) d->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
        else d->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
    }
    g_merged_attachments.clear();
}
static void note_pending_clear(const PendingClear& p)
{
    // a newer clear of the same slice replaces an older one of the same kind
    for (auto& q : g_pending_clears)
        if (q.texture == p.texture && q.slice == p.slice && q.level == p.level && q.color == p.color && q.depth == p.depth && q.stencil == p.stencil)
        {
            q = p;
            q.texture->retain();
            p.texture->release();
            return;
        }
    g_pending_clears.push_back(p);
}
#define IMGUI_IMPL_METAL_CPP
#include "imgui/imgui_extension.h"
#include "imgui/imgui_impl_metal.h"

#define EVENT_VALUE_WRAP 4096

extern bool hasValidFramebufferAttached;

float supportBufferData[512 * 4];

// Defined in the Common renderer
void LatteDraw_handleSpecialState8_clearAsDepth();

std::vector<MetalRenderer::DeviceInfo> MetalRenderer::GetDevices()
{
    NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
    std::vector<MetalRenderer::DeviceInfo> result;
    result.reserve(devices->count());
    for (uint32 i = 0; i < devices->count(); i++)
    {
        MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
        result.push_back({std::string(device->name()->utf8String()), device->registryID()});
    }

    return result;
}

MetalRenderer::MetalRenderer() : Renderer(RendererAPI::Metal)
{
    // Options

    // Position invariance
    switch (g_current_game_profile->GetPositionInvariance())
    {
    case PositionInvariance::Auto:
        switch (CafeSystem::GetForegroundTitleId())
        {
        // Bayonetta
        case 0x0005000010157F00: // EUR
        case 0x0005000010157E00: // USA
        case 0x000500001014DB00: // JPN
        // Bayonetta 2
        case 0x0005000010172700: // EUR
        case 0x0005000010172600: // USA
        // Disney Planes
        case 0x0005000010136900: // EUR
        case 0x0005000010136A00: // EUR (TODO: check)
        case 0x0005000010136B00: // EUR (TODO: check)
        case 0x000500001011C500: // USA (TODO: check)
        // LEGO STAR WARS: The Force Awakens
        case 0x00050000101DAA00: // EUR
        case 0x00050000101DAB00: // USA
        // Mario Kart 8
        case 0x000500001010ED00: // EUR
        case 0x000500001010EC00: // USA
        case 0x000500001010EB00: // JPN
        case 0x0005000010183A00: // JPN (TODO: check)
        // Minecraft: Story Mode
        case 0x000500001020A300: // EUR
        case 0x00050000101E0100: // USA
        //case 0x000500001020a200: // USA
        // Ninja Gaiden 3: Razor's Edge
        case 0x0005000010110B00: // EUR
        case 0x0005000010139B00: // EUR (TODO: check)
        case 0x0005000010110A00: // USA
        case 0x0005000010110900: // JPN
        // Resident Evil: Revelations
        case 0x000500001012B400: // EUR
        case 0x000500001012CF00: // USA
        // Star Fox Zero
        case 0x00050000101B0500: // EUR
        case 0x0005000010201C00: // EUR (TODO: check)
        case 0x00050000101B0400: // USA
        case 0x0005000010201B00: // USA (TODO: check)
        // The Legend of Zelda: Breath of the Wild
        case 0x00050000101C9500: // EUR
        case 0x00050000101C9400: // USA
        case 0x00050000101C9300: // JPN
        // Wonderful 101
        case 0x0005000010135300: // EUR
        case 0x000500001012DC00: // USA
        case 0x0005000010116300: // JPN
        case 0x0005000010185600: // JPN (TODO: check)
            m_positionInvariance = true;
            break;
        default:
            m_positionInvariance = false;
            break;
        }
        break;
    case PositionInvariance::False:
        m_positionInvariance = false;
        break;
    case PositionInvariance::True:
        m_positionInvariance = true;
        break;
    }

    // Pick a device
    auto& config = GetConfig();
    const bool hasDeviceSet = config.mtl_graphic_device_uuid != 0;

    // If a device is set, try to find it
    if (hasDeviceSet)
    {
        NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
        for (uint32 i = 0; i < devices->count(); i++)
        {
            MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
            if (device->registryID() == config.mtl_graphic_device_uuid)
            {
                m_device = device;
                break;
            }
        }
    }

    if (!m_device)
    {
        if (hasDeviceSet)
        {
            cemuLog_log(LogType::Force, "The selected GPU ({}) could not be found. Using the system default device.", config.mtl_graphic_device_uuid);
            config.mtl_graphic_device_uuid = 0;
        }
        // Use the system default device
        m_device = MTL::CreateSystemDefaultDevice();
    }

    // Vendor
    const char* deviceName = m_device->name()->utf8String();
    if (memcmp(deviceName, "Apple", 5) == 0)
        m_vendor = GfxVendor::Apple;
    else if (memcmp(deviceName, "AMD", 3) == 0)
        m_vendor = GfxVendor::AMD;
    else if (memcmp(deviceName, "Intel", 5) == 0)
        m_vendor = GfxVendor::Intel;
    else if (memcmp(deviceName, "NVIDIA", 6) == 0)
        m_vendor = GfxVendor::Nvidia;
    else
        m_vendor = GfxVendor::Generic;

    m_selectedDeviceName = deviceName;

    // Feature support
    m_isAppleGPU = m_device->supportsFamily(MTL::GPUFamilyApple1);
    m_supportsFramebufferFetch = GetConfig().framebuffer_fetch.GetValue() ? m_device->supportsFamily(MTL::GPUFamilyApple2) : false;
#if TARGET_OS_IPHONE  // iOS port: no Managed storage on iOS (the simulator's device reports no unified memory)
    m_hasUnifiedMemory = true;
#else
    m_hasUnifiedMemory = m_device->hasUnifiedMemory();
#endif
    m_supportsMetal3 = m_device->supportsFamily(MTL::GPUFamilyMetal3);
    m_supportsMeshShaders = (m_supportsMetal3 && (m_vendor != GfxVendor::Intel || GetConfig().force_mesh_shaders.GetValue())); // Intel GPUs have issues with mesh shaders
    m_recommendedMaxVRAMUsage = m_device->recommendedMaxWorkingSetSize();
    m_pixelFormatSupport = MetalPixelFormatSupport(m_device);

    CheckForPixelFormatSupport(m_pixelFormatSupport);

    // Command queue
    m_commandQueue = m_device->newCommandQueue();

    // Synchronization resources
    m_event = m_device->newEvent();

    // Resources
    NS_STACK_SCOPED MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Nearest sampler state", samplerDescriptor));
#endif
    m_nearestSampler = m_device->newSamplerState(samplerDescriptor);

    samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
    samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Linear sampler state", samplerDescriptor));
#endif
    m_linearSampler = m_device->newSamplerState(samplerDescriptor);

    // Null resources
    NS_STACK_SCOPED MTL::TextureDescriptor* textureDescriptor = MTL::TextureDescriptor::alloc()->init();
    textureDescriptor->setTextureType(MTL::TextureType1D);
    textureDescriptor->setWidth(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead);
    m_nullTexture1D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture1D->setLabel(GetLabel("Null texture 1D", m_nullTexture1D));
#endif

    textureDescriptor->setTextureType(MTL::TextureType2D);
    textureDescriptor->setHeight(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    m_nullTexture2D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture2D->setLabel(GetLabel("Null texture 2D", m_nullTexture2D));
#endif

    m_memoryManager = new MetalMemoryManager(this);
    m_outputShaderCache = new MetalOutputShaderCache(this);
    m_pipelineCache = new MetalPipelineCache(this);
    m_depthStencilCache = new MetalDepthStencilCache(this);
    m_samplerCache = new MetalSamplerCache(this);

    // Lower the commit treshold when buffer cache needs reduced latency
    if (m_memoryManager->NeedsReducedLatency())
        m_defaultCommitTreshlod = 64;
    else
        m_defaultCommitTreshlod = 196;

    // Occlusion queries
    m_occlusionQuery.m_resultBuffer = m_device->newBuffer(OCCLUSION_QUERY_POOL_SIZE * sizeof(uint64), MTL::ResourceStorageModeShared);
#ifdef CEMU_DEBUG_ASSERT
    m_occlusionQuery.m_resultBuffer->setLabel(GetLabel("Occlusion query result buffer", m_occlusionQuery.m_resultBuffer));
#endif
    m_occlusionQuery.m_resultsPtr = (uint64*)m_occlusionQuery.m_resultBuffer->contents();

    // Reset vertex and uniform buffers
   	for (uint32 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
        m_state.m_vertexBufferOffsets[i] = INVALID_OFFSET, m_state.m_vertexBufferRedirect[i] = nullptr;

   	for (uint32 i = 0; i < METAL_SHADER_TYPE_TOTAL; i++)
    {
        for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
            m_state.m_uniformBufferOffsets[i][j] = INVALID_OFFSET;
    }

    // Utility shader library

    // Create the library
    NS::Error* error = nullptr;
	NS_STACK_SCOPED MTL::Library* utilityLibrary = m_device->newLibrary(ToNSString(utilityShaderSource), nullptr, &error);
	if (error)
    {
        cemuLog_log(LogType::Force, "failed to create utility library (error: {})", error->localizedDescription()->utf8String());
    }

    // Pipelines
    NS_STACK_SCOPED MTL::Function* vertexFullscreenFunction = utilityLibrary->newFunction(ToNSString("vertexFullscreen"));
    NS_STACK_SCOPED MTL::Function* fragmentCopyDepthToColorFunction = utilityLibrary->newFunction(ToNSString("fragmentCopyDepthToColor"));

    m_copyDepthToColorDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    m_copyDepthToColorDesc->setVertexFunction(vertexFullscreenFunction);
    m_copyDepthToColorDesc->setFragmentFunction(fragmentCopyDepthToColorFunction);

    // Void vertex pipelines
#if TARGET_OS_SIMULATOR  // iOS port: the simulator rejects attachment-less pipelines (unused, see CopyBufferToBuffer)
    m_copyBufferToBufferPipeline = nullptr;
#else
    if (m_isAppleGPU)
        m_copyBufferToBufferPipeline = new MetalVoidVertexPipeline(this, utilityLibrary, "vertexCopyBufferToBuffer");
#endif

    // HACK: for some reason, this variable ends up being initialized to some garbage data, even though its declared as bool m_captureFrame = false;
    m_occlusionQuery.m_lastCommandBuffer = nullptr;
    m_captureFrame = false;
}

MetalRenderer::~MetalRenderer()
{
    if (m_isAppleGPU)
        delete m_copyBufferToBufferPipeline;
    //delete m_copyTextureToTexturePipeline;
    //delete m_restrideBufferPipeline;

    m_copyDepthToColorDesc->release();
    for (const auto [pixelFormat, pipeline] : m_copyDepthToColorPipelines)
        pipeline->release();

    delete m_outputShaderCache;
    delete m_pipelineCache;
    delete m_depthStencilCache;
    delete m_samplerCache;
    delete m_memoryManager;

    m_nullTexture1D->release();
    m_nullTexture2D->release();

    m_nearestSampler->release();
    m_linearSampler->release();

    if (m_readbackBuffer)
        m_readbackBuffer->release();

    if (m_xfbRingBuffer)
        m_xfbRingBuffer->release();

    m_occlusionQuery.m_resultBuffer->release();

    m_event->release();

    m_commandQueue->release();
    m_device->release();
}

void MetalRenderer::InitializeLayer(const Vector2i& size, bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    layer = MetalLayerHandle(m_device, size, mainWindow);
    layer.GetLayer()->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
}

void MetalRenderer::ShutdownLayer(bool mainWindow)
{
    GetLayer(mainWindow) = MetalLayerHandle();
}

void MetalRenderer::ResizeLayer(const Vector2i& size, bool mainWindow)
{
    GetLayer(mainWindow).Resize(size);
}

void MetalRenderer::Initialize()
{
    Renderer::Initialize();
    RendererShaderMtl::Initialize();
}

void MetalRenderer::Shutdown()
{
    // TODO: should shutdown both layers
    ImGui_ImplMetal_Shutdown();
    CommitCommandBuffer();
    Renderer::Shutdown();
    RendererShaderMtl::Shutdown();
}

bool MetalRenderer::IsPadWindowActive()
{
    return (GetLayer(false).GetLayer() != nullptr);
}

bool MetalRenderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
    // Subtract host memory from total VRAM, since it's shared with the CPU
    usageInMB = (m_device->currentAllocatedSize() - m_memoryManager->GetHostAllocationSize()) / 1024 / 1024;
    totalInMB = m_recommendedMaxVRAMUsage / 1024 / 1024;

    return true;
}

void MetalRenderer::ClearColorbuffer(bool padView)
{
    if (!AcquireDrawable(!padView))
        return;

    ClearColorTextureInternal(GetLayer(!padView).GetDrawable()->texture(), 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
}

void MetalRenderer::DrawEmptyFrame(bool mainWindow)
{
    if (!BeginFrame(mainWindow))
		return;
	SwapBuffers(mainWindow, !mainWindow);
}

void MetalRenderer::SwapBuffers(bool swapTV, bool swapDRC)
{
    if (swapTV)
        SwapBuffer(true);
    if (swapDRC)
        SwapBuffer(false);

    // Reset the command buffers (they are released by TemporaryBufferAllocator)
    CommitCommandBuffer();

    // BotWRecomp: the renderer's own counts per frame, averaged over 300 frames (what the GPU's time per frame goes
    // to: passes, command buffers, the slow draw paths)
    {
        static uint64 frames = 0, passes = 0, buffers = 0, clears = 0, manual = 0, mesh = 0, fans = 0;
        auto& m = m_performanceMonitor;
        frames++, passes += m.m_renderPasses, buffers += m.m_commandBuffers, clears += m.m_clears;
        manual += m.m_manualVertexFetchDraws, mesh += m.m_meshDraws, fans += m.m_triangleFans;
        if (frames == 300)
        {
            cemuLog_log(LogType::Force, "metal per frame: {} render passes, {} command buffers, {} clears ({} merged into the next pass), {} manual vertex fetch draws, {} mesh (geometry shader) draws, {} triangle fans",
                passes / frames, buffers / frames, clears / frames, g_botw_clears_merged / frames, manual / frames, mesh / frames, fans / frames);
            g_botw_clears_merged = g_botw_clears_standalone = 0;
            if (m_memoryManager->IsRedirectMode())
            {
                uint64 st[8];
                LatteBufferCache_takeRedirectStats(st);
                cemuLog_log(LogType::Force, "buffer cache redirect per frame: {} direct uploads, {} redirected bindings, {} stale marks, {} blit uploads; ring {} KB in use, {} allocations refused (ring full)",
                    st[0] / frames, st[1] / frames, st[2] / frames, st[3] / frames, m_redirectRing.used / 1024, m_redirectRing.fallbacks);
                // BotWRecomp: what the command thread copies (the refusals above are the whole period's, not per frame)
                cemuLog_log(LogType::Force, "buffer cache redirect copies: {} KB a frame, largest {} KB, {} of the bindings served by an earlier copy, refused {} in {} frames",
                    st[4] / 1024 / frames, st[5] / 1024, st[6] / frames, m_redirectRing.fallbacks, frames);
                cemuLog_log(LogType::Force, "buffer cache large bindings: {:.2f} a frame got their stale pages by blit instead of a copy (BOTW_REDIRECT_BLIT_KB)",
                    (double)st[7] / frames);
                m_redirectRing.fallbacks = 0;
            }
            frames = passes = buffers = clears = manual = mesh = fans = 0;
        }
    }
    dump_pass_end();
    if (g_dumping) dump_frame_print(m_executingCommandBuffers.empty() ? nullptr : m_executingCommandBuffers.back(), g_dump_frame);
    ++g_dump_frame;
    g_dumping = g_dump_at > 0 && g_dump_frame >= g_dump_at && (g_dump_frame - g_dump_at) % g_dump_every == 0 &&
                (g_dump_frame - g_dump_at) / g_dump_every < g_dump_count;
    if (g_dumping)
    {
        dump_counters_init(m_device);
        m_device->sampleTimestamps(&g_dump_cpu0, &g_dump_gpu0);
    }
    // Debug
    m_performanceMonitor.ResetPerFrameData();

    // GPU capture
    if (m_capturing)
    {
        EndCapture();
    }
    else if (m_captureFrame)
    {
        StartCapture();
        m_captureFrame = false;
    }
}

void MetalRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView) {
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;

	if (m_mainLayer.GetDrawable())
	{
		// we already took a pad view screenshow and want a main window screenshot
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;

		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;

		// remember which screenshot is left to take
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	auto texMtl = static_cast<LatteTextureMtl*>(texView->baseTexture);

	int width, height;
	texMtl->GetEffectiveSize(width, height, 0);

	uint32 bytesPerRow = GetMtlTextureBytesPerRow(texMtl->format, texMtl->isDepth, width);
	uint32 size = GetMtlTextureBytesPerImage(texMtl->format, texMtl->isDepth, height, bytesPerRow);

	auto blitCommandEncoder = GetBlitCommandEncoder();

	auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
	auto buffer = bufferAllocator.AllocateBufferMemory(size, 1);

	blitCommandEncoder->copyFromTexture(texMtl->GetTexture(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), buffer.mtlBuffer, buffer.bufferOffset, bytesPerRow, 0);

	bool formatValid = true;
	std::vector<uint8> rgb_data;
	rgb_data.reserve(3 * width * height);

	auto pixelFormat = texMtl->GetTexture()->pixelFormat();
	// TODO: implement more formats
	switch (pixelFormat)
	{
	case MTL::PixelFormatRGBA8Unorm:
		for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
		{
			rgb_data.emplace_back(*ptr);
			rgb_data.emplace_back(*(ptr + 1));
			rgb_data.emplace_back(*(ptr + 2));
		}
		break;
	case MTL::PixelFormatRGBA8Unorm_sRGB:
		for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
		{
			rgb_data.emplace_back(SRGBComponentToRGB(*ptr));
			rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 1)));
			rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 2)));
		}
		break;
	default:
		cemuLog_log(LogType::Force, "Unsupported screenshot texture pixel format {}", pixelFormat);
		formatValid = false;
		break;
	}

	if (formatValid)
		SaveScreenshot(rgb_data, width, height, !padView);
}

void MetalRenderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
								sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
								bool padView, bool clearBackground)
{
    if (!AcquireDrawable(!padView))
        return;

    MTL::Texture* presentTexture = static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView();

    // Create render pass
    auto& layer = GetLayer(!padView);

    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(layer.GetDrawable()->texture());
    colorAttachment->setLoadAction(clearBackground ? MTL::LoadActionClear : MTL::LoadActionLoad);
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    auto renderCommandEncoder = GetTemporaryRenderCommandEncoder(renderPassDescriptor);

    // Get a render pipeline

    // Find out which shader we are using
    uint8 shaderIndex = 255;
    if (shader == RendererOutputShader::s_copy_shader) shaderIndex = 0;
    else if (shader == RendererOutputShader::s_bicubic_shader) shaderIndex = 1;
    else if (shader == RendererOutputShader::s_hermit_shader) shaderIndex = 2;
    else if (shader == RendererOutputShader::s_copy_shader_ud) shaderIndex = 3;
    else if (shader == RendererOutputShader::s_bicubic_shader_ud) shaderIndex = 4;
    else if (shader == RendererOutputShader::s_hermit_shader_ud) shaderIndex = 5;

    uint8 shaderType = shaderIndex % 3;

    // Get the render pipeline state
    auto renderPipelineState = m_outputShaderCache->GetPipeline(shader, shaderIndex, m_state.m_usesSRGB);

    // Draw to Metal layer
    renderCommandEncoder->setRenderPipelineState(renderPipelineState);
    renderCommandEncoder->setFragmentTexture(presentTexture, 0);
    renderCommandEncoder->setFragmentSamplerState((useLinearTexFilter ? m_linearSampler : m_nearestSampler), 0);

    // Set uniforms
    float outputSize[2] = {(float)imageWidth, (float)imageHeight};
    switch (shaderType)
    {
    case 2:
        renderCommandEncoder->setFragmentBytes(outputSize, sizeof(outputSize), 0);
        break;
    default:
        break;
    }

    renderCommandEncoder->setViewport(MTL::Viewport{(double)imageX, (double)imageY, (double)imageWidth, (double)imageHeight, 0.0, 1.0});
    {
        // BotWRecomp: the image may extend past the drawable (fill/crop mode); the scissor must not
        auto* target = layer.GetDrawable()->texture();
        sint32 sx0 = std::max(imageX, 0), sy0 = std::max(imageY, 0);
        sint32 sx1 = std::min(imageX + imageWidth, (sint32)target->width());
        sint32 sy1 = std::min(imageY + imageHeight, (sint32)target->height());
        renderCommandEncoder->setScissorRect(MTL::ScissorRect{(uint32)sx0, (uint32)sy0, (uint32)std::max(sx1 - sx0, 0), (uint32)std::max(sy1 - sy0, 0)});
    }

    renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));

    EndEncoding();
}

// SheikahVision VR: a full-screen triangle sampling a texture as it is (alpha too), for the HUD and the compositor copies
MTL::Library* MetalRenderer::GetVRLibrary()
{
    if (m_vrLibrary)
        return m_vrLibrary;
    static const char* kSource = R"(#include <metal_stdlib>
using namespace metal;
struct VertexOut { float4 position [[position]]; float2 uv; };
vertex VertexOut hud_vertex(ushort vid [[vertex_id]]) {
    float2 pos = vid == 0 ? float2(-1.0, -3.0) : vid == 1 ? float2(-1.0, 1.0) : float2(3.0, 1.0);
    VertexOut out;
    out.position = float4(pos, 0.0, 1.0);
    out.uv = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    return out;
}
fragment float4 hud_fragment(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    return tex.sample(s, in.uv);
}
// contrast-adaptive sharpening (AMD FidelityFX CAS's idea) of an eye's picture as it is copied, at the game's
// resolution: the cross of neighbours pulls the centre away from their average, less where the contrast is high
fragment float4 cas_fragment(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]],
                             constant float& strength [[buffer(0)]]) {
    const float2 t = 1.0 / float2(tex.get_width(), tex.get_height());
    const float4 c4 = tex.sample(s, in.uv);
    const float3 c = saturate(c4.rgb);
    const float3 n = saturate(tex.sample(s, in.uv + float2(0, -t.y)).rgb);
    const float3 so = saturate(tex.sample(s, in.uv + float2(0, t.y)).rgb);
    const float3 e = saturate(tex.sample(s, in.uv + float2(t.x, 0)).rgb);
    const float3 w = saturate(tex.sample(s, in.uv + float2(-t.x, 0)).rgb);
    const float3 mn = min(c, min(min(n, so), min(e, w)));
    const float3 mx = max(c, max(max(n, so), max(e, w)));
    const float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    const float3 k = -amp * mix(0.125, 0.2, strength);
    return float4(saturate((c + (n + so + e + w) * k) / (1.0 + 4.0 * k)), c4.a);
}
fragment float4 depth_fragment(VertexOut in [[stage_in]], depth2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);  // depths are not blended across an edge
    return float4(tex.sample(s, in.uv));
}
)";
    NS::Error* error = nullptr;
    m_vrLibrary = m_device->newLibrary(NS::String::string(kSource, NS::UTF8StringEncoding), nullptr, &error);
    if (!m_vrLibrary)
        cemuLog_log(LogType::Force, "VR shaders: {}", error ? error->localizedDescription()->utf8String() : "?");
    return m_vrLibrary;
}

void MetalRenderer::DrawTextureToTexture(LatteTextureView* texView, MTL::Texture* dst, float sharpen)
{
    // the game's scan-out buffer is sRGB in BotW: its pictures hold linear colours, which the display encodes; written
    // through an sRGB view of dst they are encoded the same way, so dst holds what the screen shows (the compositor
    // decodes it; copied as they were, the colours were decoded twice and the headset's picture came out dark and too
    // green, device 2026-11-02)
    MTL::Texture* target = dst;
    if (LatteGPUState.tvBufferUsesSRGB && dst->pixelFormat() == MTL::PixelFormatBGRA8Unorm)
        target = dst->newTextureView(MTL::PixelFormatBGRA8Unorm_sRGB);
    const bool cas = sharpen > 0;
    MTL::RenderPipelineState*& pipeline = cas ? (target != dst ? m_casToTextureSRGBPipeline : m_casToTexturePipeline)
                                              : (target != dst ? m_copyToTextureSRGBPipeline : m_copyToTexturePipeline);
    if (!pipeline)
    {
        MTL::Library* library = GetVRLibrary();
        if (!library)
            return;
        MTL::Function* vertexFn = library->newFunction(NS::String::string("hud_vertex", NS::UTF8StringEncoding));
        MTL::Function* fragmentFn = library->newFunction(
            NS::String::string(cas ? "cas_fragment" : "hud_fragment", NS::UTF8StringEncoding));
        NS_STACK_SCOPED auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vertexFn);
        desc->setFragmentFunction(fragmentFn);
        desc->colorAttachments()->object(0)->setPixelFormat(target->pixelFormat());
        NS::Error* error = nullptr;
        pipeline = m_device->newRenderPipelineState(desc, &error);
        vertexFn->release();
        fragmentFn->release();
        if (!pipeline)
        {
            cemuLog_log(LogType::Force, "VR copy pipeline: {}", error ? error->localizedDescription()->utf8String() : "?");
            if (target != dst) target->release();
            return;
        }
        cemuLog_log(LogType::Force, "VR copy pipeline for {}{}", target != dst ? "sRGB (linear pictures encoded)" : "as is",
                    cas ? ", sharpened" : "");
    }
    NS_STACK_SCOPED MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setTexture(target);
    attachment->setLoadAction(MTL::LoadActionDontCare);
    attachment->setStoreAction(MTL::StoreActionStore);
    auto encoder = GetTemporaryRenderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    encoder->setFragmentTexture(static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView(), 0);
    encoder->setFragmentSamplerState(m_linearSampler, 0);
    if (cas) encoder->setFragmentBytes(&sharpen, sizeof(sharpen), 0);
    encoder->setViewport(MTL::Viewport{0.0, 0.0, (double)dst->width(), (double)dst->height(), 0.0, 1.0});
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    EndEncoding();
    if (target != dst) target->release();  // the command buffer keeps what it uses
}

// SheikahVision VR: an eye's depth buffer copied into dst (R32Float), its values as they are (0..1)
void MetalRenderer::DrawDepthToTexture(LatteTextureView* texView, MTL::Texture* dst)
{
    if (!m_copyDepthPipeline)
    {
        MTL::Library* library = GetVRLibrary();
        if (!library)
            return;
        MTL::Function* vertexFn = library->newFunction(NS::String::string("hud_vertex", NS::UTF8StringEncoding));
        MTL::Function* fragmentFn = library->newFunction(NS::String::string("depth_fragment", NS::UTF8StringEncoding));
        NS_STACK_SCOPED auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vertexFn);
        desc->setFragmentFunction(fragmentFn);
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR32Float);
        NS::Error* error = nullptr;
        m_copyDepthPipeline = m_device->newRenderPipelineState(desc, &error);
        vertexFn->release();
        fragmentFn->release();
        if (!m_copyDepthPipeline)
        {
            cemuLog_log(LogType::Force, "VR depth pipeline: {}", error ? error->localizedDescription()->utf8String() : "?");
            return;
        }
    }
    NS_STACK_SCOPED MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setTexture(dst);
    attachment->setLoadAction(MTL::LoadActionDontCare);
    attachment->setStoreAction(MTL::StoreActionStore);
    auto encoder = GetTemporaryRenderCommandEncoder(pass);
    encoder->setRenderPipelineState(m_copyDepthPipeline);
    encoder->setFragmentTexture(static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView(), 0);
    encoder->setViewport(MTL::Viewport{0.0, 0.0, (double)dst->width(), (double)dst->height(), 0.0, 1.0});
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    EndEncoding();
}

// SheikahVision VR: the HUD layer (drawn by the game over a transparent buffer, so its colours are premultiplied by its
// alpha) blended over the eye images: one = src + dst * (1 - src.a). Its own small pipeline: Cemu's output shaders
// write alpha 1.
void MetalRenderer::DrawBackbufferHUD(LatteTextureView* texView, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight)
{
    if (!AcquireDrawable(true))
        return;
    auto& pipeline = m_hudPipeline[m_state.m_usesSRGB ? 1 : 0];
    if (!pipeline)
    {
        NS::Error* error = nullptr;
        MTL::Library* library = GetVRLibrary();
        if (!library)
            return;
        MTL::Function* vertexFn = library->newFunction(NS::String::string("hud_vertex", NS::UTF8StringEncoding));
        MTL::Function* fragmentFn = library->newFunction(NS::String::string("hud_fragment", NS::UTF8StringEncoding));
        NS_STACK_SCOPED auto desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vertexFn);
        desc->setFragmentFunction(fragmentFn);
        auto color = desc->colorAttachments()->object(0);
        color->setPixelFormat(m_state.m_usesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
        color->setBlendingEnabled(true);
        color->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        color->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        color->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        pipeline = m_device->newRenderPipelineState(desc, &error);
        vertexFn->release();
        fragmentFn->release();
        if (!pipeline)
        {
            cemuLog_log(LogType::Force, "VR HUD pipeline: {}", error ? error->localizedDescription()->utf8String() : "?");
            return;
        }
    }
    auto& layer = GetLayer(true);
    NS_STACK_SCOPED MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    auto attachment = pass->colorAttachments()->object(0);
    attachment->setTexture(layer.GetDrawable()->texture());
    attachment->setLoadAction(MTL::LoadActionLoad);
    attachment->setStoreAction(MTL::StoreActionStore);
    auto encoder = GetTemporaryRenderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    encoder->setFragmentTexture(static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView(), 0);
    encoder->setFragmentSamplerState(m_linearSampler, 0);
    encoder->setViewport(MTL::Viewport{(double)imageX, (double)imageY, (double)imageWidth, (double)imageHeight, 0.0, 1.0});
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    EndEncoding();
}

bool MetalRenderer::BeginFrame(bool mainWindow)
{
    return AcquireDrawable(mainWindow);
}

void MetalRenderer::Flush(bool waitIdle)
{
    if (m_recordedDrawcalls > 0 || waitIdle)
        CommitCommandBuffer();

    if (waitIdle && m_executingCommandBuffers.size() != 0)
        m_executingCommandBuffers.back()->waitUntilCompleted();
}

void MetalRenderer::NotifyLatteCommandProcessorIdle()
{
    //if (m_commitOnIdle)
    //    CommitCommandBuffer();
}

bool MetalRenderer::ImguiBegin(bool mainWindow)
{
    if (!Renderer::ImguiBegin(mainWindow))
		return false;

	if (!AcquireDrawable(mainWindow))
		return false;

	EnsureImGuiBackend();

	// Check if the font texture needs to be built
	ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->IsBuilt())
        ImGui_ImplMetal_CreateFontsTexture(m_device);

	auto& layer = GetLayer(mainWindow);

	// Render pass descriptor
	NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(layer.GetDrawable()->texture());
    colorAttachment->setLoadAction(MTL::LoadActionLoad);
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    // New frame
	ImGui_ImplMetal_NewFrame(renderPassDescriptor);
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();

	if (m_encoderType != MetalEncoderType::Render)
	    GetTemporaryRenderCommandEncoder(renderPassDescriptor);

	return true;
}

void MetalRenderer::ImguiEnd()
{
    EnsureImGuiBackend();

    if (m_encoderType != MetalEncoderType::Render)
    {
        cemuLog_logOnce(LogType::Force, "no render command encoder, cannot draw ImGui");
        return;
    }

    ImGui::Render();
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), GetCurrentCommandBuffer(), (MTL::RenderCommandEncoder*)m_commandEncoder);
	//ImGui::EndFrame();

	EndEncoding();
}

ImTextureID MetalRenderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
    try
	{
		std::vector <uint8> tmp(size.x * size.y * 4);
		for (size_t i = 0; i < data.size() / 3; ++i)
		{
			tmp[(i * 4) + 0] = data[(i * 3) + 0];
			tmp[(i * 4) + 1] = data[(i * 3) + 1];
			tmp[(i * 4) + 2] = data[(i * 3) + 2];
			tmp[(i * 4) + 3] = 0xFF;
		}

		NS_STACK_SCOPED MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
		desc->setTextureType(MTL::TextureType2D);
		desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		desc->setWidth(size.x);
		desc->setHeight(size.y);
		desc->setStorageMode(m_isAppleGPU ? MTL::StorageModeShared : MTL::StorageModeManaged);
		desc->setUsage(MTL::TextureUsageShaderRead);

		MTL::Texture* texture = m_device->newTexture(desc);

		// TODO: do a GPU copy?
		texture->replaceRegion(MTL::Region(0, 0, size.x, size.y), 0, 0, tmp.data(), size.x * 4, 0);

		return (ImTextureID)texture;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate imgui texture: {}", ex.what());
		return nullptr;
	}
}

void MetalRenderer::DeleteTexture(ImTextureID id)
{
    EnsureImGuiBackend();

    ((MTL::Texture*)id)->release();
}

void MetalRenderer::DeleteFontTextures()
{
    EnsureImGuiBackend();

    ImGui_ImplMetal_DestroyFontsTexture();
}

void MetalRenderer::AppendOverlayDebugInfo()
{
    ImGui::Text("--- GPU info ---");
    ImGui::Text("GPU                        %s", m_device->name()->utf8String());
    ImGui::Text("Is Apple GPU               %s", (m_isAppleGPU ? "yes" : "no"));
    ImGui::Text("Supports framebuffer fetch %s", (m_supportsFramebufferFetch ? "yes" : "no"));
    ImGui::Text("Has unified memory         %s", (m_hasUnifiedMemory ? "yes" : "no"));
    ImGui::Text("Supports Metal3            %s", (m_supportsMetal3 ? "yes" : "no"));

    ImGui::Text("--- Metal info ---");
    ImGui::Text("Render pipeline states     %zu", m_pipelineCache->GetPipelineCacheSize());

    ImGui::Text("--- Metal info (per frame) ---");
    ImGui::Text("Command buffers            %u", m_performanceMonitor.m_commandBuffers);
    ImGui::Text("Render passes              %u", m_performanceMonitor.m_renderPasses);
    ImGui::Text("Clears                     %u", m_performanceMonitor.m_clears);
    ImGui::Text("Manual vertex fetch draws  %u (mesh draws: %u)", m_performanceMonitor.m_manualVertexFetchDraws, m_performanceMonitor.m_meshDraws);
    ImGui::Text("Triangle fans              %u", m_performanceMonitor.m_triangleFans);

    ImGui::Text("--- Cache debug info ---");

	uint32 bufferCacheHeapSize = 0;
	uint32 bufferCacheAllocationSize = 0;
	uint32 bufferCacheNumAllocations = 0;

	LatteBufferCache_getStats(bufferCacheHeapSize, bufferCacheAllocationSize, bufferCacheNumAllocations);

	ImGui::Text("Buffer");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Allocs: %u", (uint32)(bufferCacheAllocationSize + 1023) / 1024, ((uint32)bufferCacheHeapSize + 1023) / 1024, (uint32)bufferCacheNumAllocations);

	uint32 numBuffers;
	size_t totalSize, freeSize;

	m_memoryManager->GetStagingAllocator().GetStats(numBuffers, totalSize, freeSize);
	ImGui::Text("Staging");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);

	m_memoryManager->GetIndexAllocator().GetStats(numBuffers, totalSize, freeSize);
	ImGui::Text("Index");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);
}

void MetalRenderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
    // halfZ is handled in the shader

    m_state.m_viewport = MTL::Viewport{x, y, width, height, nearZ, farZ};
}

void MetalRenderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
    m_state.m_scissor = MTL::ScissorRect{(uint32)scissorX, (uint32)scissorY, (uint32)scissorWidth, (uint32)scissorHeight};
}

LatteCachedFBO* MetalRenderer::rendertarget_createCachedFBO(uint64 key)
{
	return new CachedFBOMtl(this, key);
}

void MetalRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	if (cfbo == (LatteCachedFBO*)m_state.m_activeFBO.m_fbo)
	    m_state.m_activeFBO = {nullptr};
}

void MetalRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	m_state.m_activeFBO = {(CachedFBOMtl*)cfbo, MetalAttachmentsInfo((CachedFBOMtl*)cfbo)};
	m_state.m_fboChanged = true;
}

void* MetalRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
    return m_memoryManager->AcquireTextureUploadBuffer(size);
}

void MetalRenderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
    m_memoryManager->ReleaseTextureUploadBuffer(mem);
}

TextureDecoder* MetalRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
    return GetMtlPixelFormatInfo(format, isDepth).textureDecoder;
}

void MetalRenderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
    if (hostTexture->isDepth)
    {
        texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, hostTexture->hasStencil, 0.0f, 0);
    }
    else
    {
        texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

MTL::BlitOption GetBlitOptionForTexture(const LatteTextureMtl* texture) {
    switch (texture->GetTexture()->pixelFormat()) {
        case MTL::PixelFormatDepth16Unorm:
        case MTL::PixelFormatDepth32Float:
            return MTL::BlitOptionDepthFromDepthStencil;

        case MTL::PixelFormatStencil8:
            return MTL::BlitOptionStencilFromDepthStencil;

        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatDepth32Float_Stencil8:
            // Can't copy both in one call — caller must specify.
            // Default to depth
            return MTL::BlitOptionDepthFromDepthStencil;

        default:
            return MTL::BlitOptionNone;
    }
}

bool IsDepthStencilFormat(MTL::PixelFormat format) {
    return format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
           format == MTL::PixelFormatDepth32Float_Stencil8;
}

// TODO: do a cpu copy on Apple Silicon?
void MetalRenderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
    g_botw_blit_reason = "texture upload";
    auto textureMtl = (LatteTextureMtl*)hostTexture;

    uint32 offsetZ = 0;
    if (textureMtl->Is3DTexture())
    {
        offsetZ = sliceIndex;
        sliceIndex = 0;
    }

    size_t bytesPerRow = GetMtlTextureBytesPerRow(textureMtl->format, textureMtl->isDepth, width);
    // No need to set bytesPerImage for 3D textures, since we always load just one slice
    //size_t bytesPerImage = GetMtlTextureBytesPerImage(textureMtl->GetFormat(), textureMtl->isDepth, height, bytesPerRow);
    //if (m_isAppleGPU)
    //{
    //    textureMtl->GetTexture()->replaceRegion(MTL::Region(0, 0, offsetZ, width, height, 1), mipIndex, sliceIndex, pixelData, bytesPerRow, 0);
    //}
    //else
    //{
    auto blitCommandEncoder = GetBlitCommandEncoder();

    // Allocate a temporary buffer
    auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
    auto allocation = bufferAllocator.AllocateBufferMemory(compressedImageSize, 1);
    memcpy(allocation.memPtr, pixelData, compressedImageSize);
    bufferAllocator.FlushReservation(allocation);

    // Copy the data from the temporary buffer to the texture
	if (IsDepthStencilFormat(textureMtl->GetTexture()->pixelFormat())) {
        // Metal doesn't allow copying depth and stencil data at the same time, so we need to do two copies for combined depth/stencil formats
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), MTL::BlitOptionDepthFromDepthStencil);
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), MTL::BlitOptionStencilFromDepthStencil);
    } else {
        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), GetBlitOptionForTexture(textureMtl));
    }
    //}
}

void MetalRenderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    if (!FormatIsRenderable(hostTexture->format))
    {
        cemuLog_logOnce(LogType::Force, "cannot clear color texture with format {}, because it's not renderable", hostTexture->format);
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();

    if (g_merge_clears)
    {
        EndEncoding();
        mtlTexture->retain();
        note_pending_clear({mtlTexture, sliceIndex, mipIndex, true, MTL::ClearColor(r, g, b, a), false, false, 0.0, 0});
        m_performanceMonitor.m_clears++;
        return;
    }
    ClearColorTextureInternal(mtlTexture, sliceIndex, mipIndex, r, g, b, a);
}

void MetalRenderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
    clearStencil = (clearStencil && GetMtlPixelFormatInfo(hostTexture->format, true).hasStencil);
    if (!clearDepth && !clearStencil)
    {
        cemuLog_logOnce(LogType::Force, "skipping depth/stencil clear");
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();

    if (g_merge_clears)
    {
        EndEncoding();
        mtlTexture->retain();
        note_pending_clear({mtlTexture, (int)sliceIndex, mipIndex, false, MTL::ClearColor(0, 0, 0, 0), clearDepth, clearStencil, depthValue, stencilValue});
        m_performanceMonitor.m_clears++;
        return;
    }
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    if (clearDepth)
    {
        auto depthAttachment = renderPassDescriptor->depthAttachment();
        depthAttachment->setTexture(mtlTexture);
        depthAttachment->setClearDepth(depthValue);
        depthAttachment->setLoadAction(MTL::LoadActionClear);
        depthAttachment->setStoreAction(MTL::StoreActionStore);
        depthAttachment->setSlice(sliceIndex);
        depthAttachment->setLevel(mipIndex);
    }
    if (clearStencil)
    {
        auto stencilAttachment = renderPassDescriptor->stencilAttachment();
        stencilAttachment->setTexture(mtlTexture);
        stencilAttachment->setClearStencil(stencilValue);
        stencilAttachment->setLoadAction(MTL::LoadActionClear);
        stencilAttachment->setStoreAction(MTL::StoreActionStore);
        stencilAttachment->setSlice(sliceIndex);
        stencilAttachment->setLevel(mipIndex);
    }

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

LatteTexture* MetalRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
    return new LatteTextureMtl(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void MetalRenderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
    m_state.m_textures[textureUnit] = static_cast<LatteTextureViewMtl*>(textureView);
}

void MetalRenderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth_)
{
    g_botw_blit_reason = "texture copy";
    // Source size seems to apply to the destination texture as well, therefore we need to adjust it when block size doesn't match
    Uvec2 srcBlockTexelSize = GetMtlPixelFormatInfo(src->format, src->isDepth).blockTexelSize;
    Uvec2 dstBlockTexelSize = GetMtlPixelFormatInfo(dst->format, dst->isDepth).blockTexelSize;
    if (srcBlockTexelSize.x != dstBlockTexelSize.x || srcBlockTexelSize.y != dstBlockTexelSize.y)
    {
        uint32 multX = (srcBlockTexelSize.x > dstBlockTexelSize.x ? srcBlockTexelSize.x / dstBlockTexelSize.x : dstBlockTexelSize.x / srcBlockTexelSize.x);
        effectiveCopyWidth *= multX;

        uint32 multY = (srcBlockTexelSize.y > dstBlockTexelSize.y ? srcBlockTexelSize.y / dstBlockTexelSize.y : dstBlockTexelSize.y / srcBlockTexelSize.y);
        effectiveCopyHeight *= multY;
    }

    botw_blit_touches(static_cast<LatteTextureMtl*>(src)->GetTexture(), static_cast<LatteTextureMtl*>(dst)->GetTexture());
    auto blitCommandEncoder = GetBlitCommandEncoder();
    {
        extern const char* g_botw_copy_site;
        if (g_dumping && !g_dump_passes.empty() && g_dump_passes.back().kind == 1)
            g_dump_passes.back().desc += fmt::format(" [{} {:08x} {}x{} fmt {:x}{} mip {} -> {:08x} fmt {:x}{}]", g_botw_copy_site,
                src->physAddress, effectiveCopyWidth, effectiveCopyHeight, (uint32)src->format, src->isDepth ? " depth" : "", srcMip,
                dst->physAddress, (uint32)dst->format, dst->isDepth ? " depth" : "");
        g_botw_copy_site = "other";
    }

    auto mtlSrc = static_cast<LatteTextureMtl*>(src)->GetTexture();
    auto mtlDst = static_cast<LatteTextureMtl*>(dst)->GetTexture();

    uint32 srcBaseLayer = 0;
    uint32 dstBaseLayer = 0;
    uint32 srcOffsetZ = 0;
    uint32 dstOffsetZ = 0;
    uint32 srcLayerCount = 1;
    uint32 dstLayerCount = 1;
    uint32 srcDepth = 1;
    uint32 dstDepth = 1;

	if (src->Is3DTexture())
	{
		srcOffsetZ = srcSlice;
		srcDepth = srcDepth_;
	}
	else
	{
		srcBaseLayer = srcSlice;
		srcLayerCount = srcDepth_;
	}

	if (dst->Is3DTexture())
	{
		dstOffsetZ = dstSlice;
		dstDepth = srcDepth_;
	}
	else
	{
		dstBaseLayer = dstSlice;
		dstLayerCount = srcDepth_;
	}

	// If copying whole textures, we can do a more efficient copy
    if (effectiveSrcX == 0 && effectiveSrcY == 0 && effectiveDstX == 0 && effectiveDstY == 0 &&
        srcOffsetZ == 0 && dstOffsetZ == 0 &&
        effectiveCopyWidth == src->GetMipWidth(srcMip) && effectiveCopyHeight == src->GetMipHeight(srcMip) && srcDepth == src->GetMipDepth(srcMip) &&
        effectiveCopyWidth == dst->GetMipWidth(dstMip) && effectiveCopyHeight == dst->GetMipHeight(dstMip) && dstDepth == dst->GetMipDepth(dstMip) &&
        srcLayerCount == dstLayerCount)
    {
        blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer, srcMip, mtlDst, dstBaseLayer, dstMip, srcLayerCount, 1);
    }
    else
    {
        if (srcLayerCount == dstLayerCount)
        {
            for (uint32 i = 0; i < srcLayerCount; i++)
            {
                blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer + i, srcMip, MTL::Origin(effectiveSrcX, effectiveSrcY, srcOffsetZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, srcDepth), mtlDst, dstBaseLayer + i, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, dstOffsetZ));
            }
        }
        else
        {
            for (uint32 i = 0; i < std::max(srcLayerCount, dstLayerCount); i++)
            {
                if (srcLayerCount == 1)
                    srcOffsetZ++;
                else
                    srcSlice++;

                if (dstLayerCount == 1)
                    dstOffsetZ++;
                else
                    dstSlice++;

                blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer, srcMip, MTL::Origin(effectiveSrcX, effectiveSrcY, srcOffsetZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, 1), mtlDst, dstBaseLayer, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, dstOffsetZ));
            }
        }
    }
}

LatteTextureReadbackInfo* MetalRenderer::texture_createReadback(LatteTextureView* textureView)
{
    auto* baseTexture = static_cast<LatteTextureMtl*>(textureView->baseTexture);
    if (baseTexture->m_isAlternateFormat)
    {
        cemuLog_logDebug(LogType::Force, "Metal does not support readback of texture format 0x{:x}", (uint32)baseTexture->format);
        return nullptr;
    }
    size_t uploadSize = baseTexture->GetTexture()->allocatedSize();

    if ((m_readbackBufferWriteOffset + uploadSize) > TEXTURE_READBACK_SIZE)
	{
		m_readbackBufferWriteOffset = 0;
	}

    auto* result = new LatteTextureReadbackInfoMtl(this, textureView, m_readbackBufferWriteOffset);
    m_readbackBufferWriteOffset += uploadSize;

	return result;
}

void MetalRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
    // scale copy size to effective size
	sint32 effectiveCopyWidth = width;
	sint32 effectiveCopyHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
	//sint32 sourceEffectiveWidth, sourceEffectiveHeight;
	//sourceTexture->GetEffectiveSize(sourceEffectiveWidth, sourceEffectiveHeight, srcMip);

    texture_copyImageSubData(sourceTexture, srcMip, 0, 0, srcSlice, destinationTexture, dstMip, 0, 0, dstSlice, effectiveCopyWidth, effectiveCopyHeight, 1);
}

void MetalRenderer::bufferCache_init(const sint32 bufferSize)
{
    m_memoryManager->InitBufferCache(bufferSize);
}

void MetalRenderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
    m_memoryManager->UploadToBufferCache(buffer, bufferOffset, size);
}

void MetalRenderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    m_memoryManager->CopyBufferCache(srcOffset, dstOffset, size);
}

void MetalRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    if (m_memoryManager->UseHostMemoryForCache())
        dstOffset -= m_memoryManager->GetImportedMemBaseAddress();

    CopyBufferToBuffer(GetXfbRingBuffer(), srcOffset, m_memoryManager->GetBufferCache(), dstOffset, size, MTL::RenderStageVertex | MTL::RenderStageMesh, ALL_MTL_RENDER_STAGES);
}

void MetalRenderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
    cemu_assert_debug(!m_memoryManager->UseHostMemoryForCache());
    for (const auto& binding : bindings)
    {
        cemu_assert_debug(binding.index < LATTE_MAX_VERTEX_BUFFERS);
        m_state.m_vertexBufferOffsets[binding.index] = binding.bindOffset;
        m_state.m_vertexBufferRedirect[binding.index] = (MTL::Buffer*)binding.redirectBuffer;
        m_state.m_redirectSize[0][binding.index] = binding.bindSize;
        if (binding.redirectBuffer) m_state.m_redirectCommandBuffer = m_currentCommandBuffer.m_commited ? nullptr : m_currentCommandBuffer.m_commandBuffer;
    }
}

void MetalRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size, void* redirectBuffer)
{
    cemu_assert_debug(!m_memoryManager->UseHostMemoryForCache());

    m_state.m_uniformBufferOffsets[GetMtlGeneralShaderType(shaderType)][bufferIndex] = offset;
    m_state.m_uniformBufferRedirect[GetMtlGeneralShaderType(shaderType)][bufferIndex] = (MTL::Buffer*)redirectBuffer;
    m_state.m_redirectSize[1 + GetMtlGeneralShaderType(shaderType)][bufferIndex] = size;
    if (redirectBuffer) m_state.m_redirectCommandBuffer = m_currentCommandBuffer.m_commited ? nullptr : m_currentCommandBuffer.m_commandBuffer;
}

// ---- SheikahVision redirect buffer cache mode -------------------------------------------------------------------------
bool MetalRenderer::bufferCache_redirectMode()
{
    return m_memoryManager->IsRedirectMode();
}

void MetalRenderer::bufferCache_uploadDirect(uint8* buffer, sint32 size, uint32 bufferOffset)
{
    m_memoryManager->UploadToBufferCacheDirect(buffer, bufferOffset, size);
}

uint32 MetalRenderer::bufferCache_gpuDoneDrawcall()
{
    return m_gpuDoneDrawcall.load(std::memory_order_acquire);
}

static int g_redirect_ring_mb = [] { const char* e = getenv("BOTW_REDIRECT_RING_MB"); return e ? std::max(atoi(e), 8) : 64; }();

uint8* MetalRenderer::bufferCache_redirectAlloc(uint32 size, void** redirectBuffer, uint32* redirectOffset)
{
    auto& r = m_redirectRing;
    if (!r.buffer)
    {
        r.size = (uint32)g_redirect_ring_mb * 1024 * 1024;
        r.buffer = m_device->newBuffer(r.size, MTL::ResourceStorageModeShared);
        r.base = (uint8*)r.buffer->contents();
        cemuLog_log(LogType::Force, "buffer cache redirect ring: {} MB", g_redirect_ring_mb);
    }
    size = (size + 255) & ~255u;
    if (size > r.size - r.used)
    {
        r.fallbacks++;
        return nullptr;
    }
    // the ring never hands out a range crossing its end
    if (r.head + size > r.size)
    {
        const uint32 skip = r.size - r.head;
        if (size > r.size - r.used - skip) { r.fallbacks++; return nullptr; }
        r.used += skip;
        r.head = 0;
    }
    const uint32 offset = r.head;
    r.head = (r.head + size) % r.size;
    r.used += size;
    *redirectBuffer = r.buffer;
    *redirectOffset = offset;
    return r.base + offset;
}

// A command buffer was committed between the bindings (LatteBufferCache_Sync) and this draw: the ring space of the
// redirected bindings is fenced to the committed buffer and would be reclaimed when it completes, while this draw
// (in the next buffer) still reads it. Copy the data into fresh space, which belongs to the current buffer. The old
// space is still intact: the ring is only reclaimed at commits.
void MetalRenderer::RedirectRebind()
{
    auto move = [&](MTL::Buffer*& buf, size_t& offset, uint32 size) {
        void* nb; uint32 no;
        uint8* dst = bufferCache_redirectAlloc(size, &nb, &no);
        if (!dst) return;  // ring full: keep the old space (fenced to the previous buffer; it is released only once that completes, and this draw's buffer is committed later, so the risk is a later reuse before completion... rare and bounded by the ring size)
        memcpy(dst, m_redirectRing.base + offset, size);
        buf = (MTL::Buffer*)nb, offset = no;
    };
    for (uint32 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
        if (m_state.m_vertexBufferRedirect[i]) move(m_state.m_vertexBufferRedirect[i], m_state.m_vertexBufferOffsets[i], m_state.m_redirectSize[0][i]);
    for (uint32 t = 0; t < METAL_GENERAL_SHADER_TYPE_TOTAL; t++)
        for (uint32 i = 0; i < MAX_MTL_BUFFERS; i++)
            if (m_state.m_uniformBufferRedirect[t][i]) move(m_state.m_uniformBufferRedirect[t][i], m_state.m_uniformBufferOffsets[t][i], m_state.m_redirectSize[1 + t][i]);
    m_state.m_redirectCommandBuffer = m_currentCommandBuffer.m_commited ? nullptr : m_currentCommandBuffer.m_commandBuffer;
}

// at a commit: remember how far the ring was used by this command buffer, and release what completed buffers used
static std::atomic<uint64> g_redirect_ring_epoch{0};
extern "C" uint64 botw_redirect_ring_epoch(void) { return g_redirect_ring_epoch.load(std::memory_order_relaxed); }
void MetalRenderer::RedirectRingCommit(MTL::CommandBuffer* committed)
{
    g_redirect_ring_epoch.fetch_add(1, std::memory_order_relaxed);  // copies made before this commit are not reused after it
    auto& r = m_redirectRing;
    // release the fences of completed command buffers (in commit order)
    while (!r.fences.empty() && CommandBufferCompleted(r.fences.front().commandBuffer))
    {
        const auto& f = r.fences.front();
        r.tail = f.head;
        r.used -= f.used;
        f.commandBuffer->release();  // retained below (m_executingCommandBuffers releases its reference on completion)
        r.fences.pop_front();
    }
    if (r.buffer)
    {
        // bytes allocated since the previous fence belong to this command buffer
        uint32 since = r.used;
        for (const auto& f : r.fences) since -= f.used;
        committed->retain();
        r.fences.push_back({committed, r.head, since, LatteGPUState.drawCallCounter});
    }
}

RendererShader* MetalRenderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
    return new RendererShaderMtl(this, type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

void MetalRenderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
    m_state.m_streamoutState.buffers[bufferIndex].enabled = true;
	m_state.m_streamoutState.buffers[bufferIndex].ringBufferOffset = ringBufferOffset;
}

void MetalRenderer::streamout_begin()
{
    // Do nothing
}

void MetalRenderer::streamout_rendererFinishDrawcall()
{
    // Do nothing
}

void MetalRenderer::draw_beginSequence()
{
    m_state.m_skipDrawSequence = false;

    bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

    // update shader state
	LatteSHRC_UpdateActiveShaders();
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logOnce(LogType::Force, "Skipping drawcalls due to shader error\n");
		m_state.m_skipDrawSequence = true;
		cemu_assert_debug(false);
		return;
	}

	// update render target and texture state
	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			cemuLog_logOnce(LogType::Force, "Rendertarget invalid\n");
			m_state.m_skipDrawSequence = true;
			return; // no render target
		}

		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
			cemuLog_logOnce(LogType::Force, "Drawcall with no color buffer or depth buffer attached\n");
			m_state.m_skipDrawSequence = true;
			return; // no render target
		}
		LatteTexture_updateTextures();
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}

	// apply render target
	LatteMRT::ApplyCurrentState();

	// viewport and scissor box
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	if (!LatteGPUState.contextNew.IsRasterizationEnabled() && !streamoutEnable)
		m_state.m_skipDrawSequence = true;
}

void MetalRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
    if (m_state.m_skipDrawSequence)
	{
	    LatteGPUState.drawCallCounter++;
		return;
	}

	// fast clear color as depth
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		LatteGPUState.drawCallCounter++;
		return;
	}
	else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
		draw_handleSpecialState5();
		LatteGPUState.drawCallCounter++;
		return;
	}

	auto& encoderState = m_state.m_encoderState;

    // Shaders
    LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
    LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
    const auto fetchShader = LatteSHRC_GetActiveFetchShader();

    // SheikahVision VR stereo: classify the draw (see g_latte_eye)
    bool geometryLike = false;
    if (g_latte_eye >= 0 && g_eye_w && m_state.m_activeFBO.m_fbo)
    {
        auto* fbo = m_state.m_activeFBO.m_fbo;
        LatteTextureView* first = fbo->colorBuffer[0].texture ? fbo->colorBuffer[0].texture : fbo->depthBuffer.texture;
        if (texture_is_eye_sized(first) && pixelShader)
        {
            geometryLike = true;
            const sint32 n = pixelShader->resourceMapping.getTextureCount();
            for (sint32 i = 0; i < n && geometryLike; i++)
            {
                const auto unit = pixelShader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
                if (pixelShader->textureRenderTargetIndex[unit] != 255) { geometryLike = false; break; }  // framebuffer fetch
                if (texture_is_eye_sized(m_state.m_textures[unit + LATTE_CEMU_PS_TEX_UNIT_BASE])) geometryLike = false;
            }
        }
        if (geometryLike && g_pass_draws >= 0) g_pass_geom++;
        if (geometryLike && g_stereo_skip && g_latte_eye == 1)
        {
            LatteGPUState.drawCallCounter++;
            return;
        }
    }

    // mode 2: a texture this draw may sample still has a clear waiting: run it first (a stale slot only costs a clear
    // that would have run anyway)
    if (g_merge_clears == 2 && !g_pending_clears.empty())
    {
        for (auto* view : m_state.m_textures)
        {
            if (!view || !view->baseTexture) continue;
            auto* tex = static_cast<LatteTextureMtl*>(view->baseTexture)->GetTexture();
            if (pending_clear_touches(tex))
            {
                EndEncoding();
                run_pending_clears_of(GetCommandBuffer(), tex, nullptr);
            }
        }
    }

    /*
    bool neverSkipAccurateBarrier = false;

    // "Accurate barriers" is usually enabled globally but since the CPU cost is substantial we allow users to disable it (debug -> 'Accurate barriers' option)
	// We always force accurate barriers for known problematic shaders
	if (pixelShader)
	{
		if (pixelShader->baseHash == 0x6f6f6e7b9aae57af && pixelShader->auxHash == 0x00078787f9249249) // BotW lava
			neverSkipAccurateBarrier = true;
		if (pixelShader->baseHash == 0x4c0bd596e3aef4a6 && pixelShader->auxHash == 0x003c3c3fc9269249) // BotW foam layer for water on the bottom of waterfalls
			neverSkipAccurateBarrier = true;
	}

	// Check if we need to end the render pass
	if (!m_state.m_isFirstDrawInRenderPass && (GetConfig().vk_accurate_barriers || neverSkipAccurateBarrier))
	{
    	// Fragment shader is most likely to require a render pass flush, so check for it first
    	bool endRenderPass = CheckIfRenderPassNeedsFlush(pixelShader);
    	if (!endRenderPass)
    	    endRenderPass = CheckIfRenderPassNeedsFlush(vertexShader);
    	if (!endRenderPass && geometryShader)
    	    endRenderPass = CheckIfRenderPassNeedsFlush(geometryShader);

    	if (endRenderPass)
        {
    	    EndEncoding();
            // TODO: only log in debug?
            cemuLog_logOnce(LogType::Force, "Ending render pass due to render target self-dependency\n");
        }
	}
	*/

    // Primitive type
    const LattePrimitiveMode primitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
    auto mtlPrimitiveType = GetMtlPrimitiveType(primitiveMode);

    bool usesGeometryShader = UseGeometryShader(LatteGPUState.contextNew, geometryShader != nullptr);
    if (usesGeometryShader && !m_supportsMeshShaders)
        return;

    bool fetchVertexManually = (usesGeometryShader || fetchShader->mtlFetchVertexManually);

	// Index buffer
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	uint32 indexMax = 0;
	Renderer::IndexAllocation indexAllocation;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, indexAllocation);
	auto indexAllocationMtl = static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(indexAllocation.rendererInternal);

	// Buffer cache
	if (m_memoryManager->UseHostMemoryForCache())
	{
		// direct memory access (Wii U memory space imported as a buffer), update buffer bindings
		draw_updateVertexBuffersDirectAccess();
		if (vertexShader)
			draw_updateUniformBuffersDirectAccess(vertexShader, mmSQ_VTX_UNIFORM_BLOCK_START);
		if (geometryShader)
			draw_updateUniformBuffersDirectAccess(geometryShader, mmSQ_GS_UNIFORM_BLOCK_START);
		if (pixelShader)
			draw_updateUniformBuffersDirectAccess(pixelShader, mmSQ_PS_UNIFORM_BLOCK_START);
	}
	else
	{
    	// synchronize vertex and uniform cache and update buffer bindings
    	// We need to call this before getting the render command encoder, since it can cause buffer copies
		uint8 stageUniformModifiedMask = 0;
    	LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);
	}

	// Render pass
	auto renderCommandEncoder = GetRenderCommandEncoder();
	if (m_state.m_redirectCommandBuffer && m_state.m_redirectCommandBuffer != GetCurrentCommandBuffer())
		RedirectRebind();

    // Render pipeline state
    PipelineObject* pipelineObj = m_pipelineCache->GetRenderPipelineState(fetchShader, vertexShader, geometryShader, pixelShader, m_state.m_lastUsedFBO.m_attachmentsInfo, m_state.m_activeFBO.m_attachmentsInfo, m_state.m_activeFBO.m_fbo->m_size, count, LatteGPUState.contextNew);
    if (!pipelineObj->m_pipeline)
        return;

    if (pipelineObj->m_pipeline != encoderState.m_renderPipelineState)
   	{
        renderCommandEncoder->setRenderPipelineState(pipelineObj->m_pipeline);
  		encoderState.m_renderPipelineState = pipelineObj->m_pipeline;
   	}

	// Depth stencil state

	// Disable depth write when there is no depth attachment
	auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
	bool depthWriteEnable = depthControl.get_Z_WRITE_ENABLE();
	if (!m_state.m_activeFBO.m_fbo->depthBuffer.texture)
	    depthControl.set_Z_WRITE_ENABLE(false);

	MTL::DepthStencilState* depthStencilState = m_depthStencilCache->GetDepthStencilState(LatteGPUState.contextNew);
	if (depthStencilState != encoderState.m_depthStencilState)
	{
	    renderCommandEncoder->setDepthStencilState(depthStencilState);
		encoderState.m_depthStencilState = depthStencilState;
	}

	// Restore the original depth write state
	depthControl.set_Z_WRITE_ENABLE(depthWriteEnable);

	// Stencil reference
	bool stencilEnable = LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
	if (stencilEnable)
	{
	    bool backStencilEnable = LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
		uint32 stencilRefFront = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
    	uint32 stencilRefBack;
        if (backStencilEnable)
            stencilRefBack = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILREF_B();
        else
            stencilRefBack = stencilRefFront;

	    if (stencilRefFront != encoderState.m_stencilRefFront || stencilRefBack != encoderState.m_stencilRefBack)
		{
		    renderCommandEncoder->setStencilReferenceValues(stencilRefFront, stencilRefBack);

			encoderState.m_stencilRefFront = stencilRefFront;
			encoderState.m_stencilRefBack = stencilRefBack;
		}
	}

	// Blend color
	uint32* blendColorConstantU32 = LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;

	if (blendColorConstantU32[0] != encoderState.m_blendColor[0] || blendColorConstantU32[1] != encoderState.m_blendColor[1] || blendColorConstantU32[2] != encoderState.m_blendColor[2] || blendColorConstantU32[3] != encoderState.m_blendColor[3])
	{
    	float* blendColorConstant = (float*)LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;
    	renderCommandEncoder->setBlendColor(blendColorConstant[0], blendColorConstant[1], blendColorConstant[2], blendColorConstant[3]);

        encoderState.m_blendColor[0] = blendColorConstantU32[0];
        encoderState.m_blendColor[1] = blendColorConstantU32[1];
        encoderState.m_blendColor[2] = blendColorConstantU32[2];
        encoderState.m_blendColor[3] = blendColorConstantU32[3];
	}

	// polygon control
	const auto& polygonControlReg = LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	uint32 cullFront = polygonControlReg.get_CULL_FRONT();
	uint32 cullBack = polygonControlReg.get_CULL_BACK();
	uint32 polyOffsetFrontEnable = polygonControlReg.get_OFFSET_FRONT_ENABLED();

	if (polyOffsetFrontEnable)
	{
    	uint32 frontScaleU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
    	uint32 frontOffsetU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
    	uint32 offsetClampU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.getRawValue();

        if (frontOffsetU32 != encoderState.m_depthBias || frontScaleU32 != encoderState.m_depthSlope || offsetClampU32 != encoderState.m_depthClamp)
        {
           	float frontScale = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
           	float frontOffset = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
           	float offsetClamp = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();

           	frontScale /= 16.0f;

            renderCommandEncoder->setDepthBias(frontOffset, frontScale, offsetClamp);

            encoderState.m_depthBias = frontOffsetU32;
            encoderState.m_depthSlope = frontScaleU32;
            encoderState.m_depthClamp = offsetClampU32;
        }
	}
	else
	{
	    if (0 != encoderState.m_depthBias || 0 != encoderState.m_depthSlope || 0 != encoderState.m_depthClamp)
		{
	        renderCommandEncoder->setDepthBias(0.0f, 0.0f, 0.0f);

			encoderState.m_depthBias = 0;
			encoderState.m_depthSlope = 0;
			encoderState.m_depthClamp = 0;
		}
	}

	// Depth clip mode
	cemu_assert_debug(LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE()); // near or far clipping can be disabled individually
	bool zClipEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() == false;

	if (zClipEnable != encoderState.m_depthClipEnable)
	{
	    renderCommandEncoder->setDepthClipMode(zClipEnable ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);
        encoderState.m_depthClipEnable = zClipEnable;
	}

	// Visibility result mode
	if (m_occlusionQuery.m_active)
	{
	    auto mode = (m_occlusionQuery.m_currentIndex == INVALID_UINT32 ? MTL::VisibilityResultModeDisabled : MTL::VisibilityResultModeCounting);
	    renderCommandEncoder->setVisibilityResultMode(mode, m_occlusionQuery.m_currentIndex * sizeof(uint64));
	}

	// todo - how does culling behave with rects?
	// right now we just assume that their winding is always CW
	if (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}

	// Cull mode

	// Cull front and back is handled by disabling rasterization
	if (!(cullFront && cullBack))
	{
        MTL::CullMode cullMode;
       	if (cullFront)
      		cullMode = MTL::CullModeFront;
       	else if (cullBack)
      		cullMode = MTL::CullModeBack;
       	else
      		cullMode = MTL::CullModeNone;

        if (cullMode != encoderState.m_cullMode)
       	{
       	    renderCommandEncoder->setCullMode(cullMode);
      		encoderState.m_cullMode = cullMode;
       	}
	}

	// Front face
	MTL::Winding frontFaceWinding;
	if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW)
		frontFaceWinding = MTL::WindingCounterClockwise;
	else
		frontFaceWinding = MTL::WindingClockwise;

    if (frontFaceWinding != encoderState.m_frontFaceWinding)
   	{
   	    renderCommandEncoder->setFrontFacingWinding(frontFaceWinding);
  		encoderState.m_frontFaceWinding = frontFaceWinding;
   	}

    // Viewport
    if (m_state.m_viewport.originX != encoderState.m_viewport.originX ||
        m_state.m_viewport.originY != encoderState.m_viewport.originY ||
        m_state.m_viewport.width != encoderState.m_viewport.width ||
        m_state.m_viewport.height != encoderState.m_viewport.height ||
        m_state.m_viewport.znear != encoderState.m_viewport.znear ||
        m_state.m_viewport.zfar != encoderState.m_viewport.zfar)
    {
        renderCommandEncoder->setViewport(m_state.m_viewport);

        encoderState.m_viewport = m_state.m_viewport;
    }

    // Scissor
    if (m_state.m_scissor.x != encoderState.m_scissor.x ||
        m_state.m_scissor.y != encoderState.m_scissor.y ||
        m_state.m_scissor.width != encoderState.m_scissor.width ||
        m_state.m_scissor.height != encoderState.m_scissor.height)
    {
        encoderState.m_scissor = m_state.m_scissor;

        // TODO: clamp scissor to render target dimensions?
        //scissor.width = ;
        //scissor.height = ;
        renderCommandEncoder->setScissorRect(encoderState.m_scissor);
    }

	// Resources

	// Vertex buffers
    for (uint8 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
    {
        size_t offset = m_state.m_vertexBufferOffsets[i];
        if (offset != INVALID_OFFSET)
        {
            // Bind
            MTL::Buffer* buf = m_state.m_vertexBufferRedirect[i] ? m_state.m_vertexBufferRedirect[i] : m_memoryManager->GetBufferCache();
            SetBuffer(renderCommandEncoder, GetMtlShaderType(vertexShader->shaderType, usesGeometryShader), buf, offset, GET_MTL_VERTEX_BUFFER_INDEX(i));
        }
    }

	// Prepare streamout
	m_state.m_streamoutState.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	// Uniform buffers, textures and samplers
	BindStageResources(renderCommandEncoder, vertexShader, usesGeometryShader);
	if (usesGeometryShader && geometryShader)
	    BindStageResources(renderCommandEncoder, geometryShader, usesGeometryShader);
	BindStageResources(renderCommandEncoder, pixelShader, usesGeometryShader);

	// Draw
	if (usesGeometryShader)
	{
	    if (hostIndexType != INDEX_TYPE::NONE)
		    SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_OBJECT, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, vertexShader->resourceMapping.indexBufferBinding);

		uint8 hostIndexTypeU8 = (uint8)hostIndexType;
		renderCommandEncoder->setObjectBytes(&hostIndexTypeU8, sizeof(hostIndexTypeU8), vertexShader->resourceMapping.indexTypeBinding);
        encoderState.m_buffers[METAL_SHADER_TYPE_OBJECT][vertexShader->resourceMapping.indexTypeBinding] = {nullptr};

		uint32 verticesPerPrimitive = GetVerticesPerPrimitive(primitiveMode);
		uint32 threadgroupCount = count * instanceCount;
		if (PrimitiveRequiresConnection(primitiveMode))
		    threadgroupCount -= verticesPerPrimitive - 1;
		else
		    threadgroupCount /= verticesPerPrimitive;

		renderCommandEncoder->drawMeshThreadgroups(MTL::Size(threadgroupCount, 1, 1), MTL::Size(verticesPerPrimitive, 1, 1), MTL::Size(1, 1, 1));
	}
	else
	{
        if (hostIndexType != INDEX_TYPE::NONE)
       	{
       	    auto mtlIndexType = GetMtlIndexType(hostIndexType);
      		renderCommandEncoder->drawIndexedPrimitives(mtlPrimitiveType, hostIndexCount, mtlIndexType, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, instanceCount, baseVertex, baseInstance);
       	}
       	else
       	{
      		renderCommandEncoder->drawPrimitives(mtlPrimitiveType, baseVertex, count, instanceCount, baseInstance);
       	}
	}

	m_state.m_isFirstDrawInRenderPass = false;
	if (g_pass_draws >= 0)
	{
		g_pass_draws++;
		if (LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader()) g_pass_shaders[ps->baseHash]++;
	}

	// Occlusion queries
	if (m_occlusionQuery.m_active)
	    m_occlusionQuery.m_currentIndex = (m_occlusionQuery.m_currentIndex + 1) % OCCLUSION_QUERY_POOL_SIZE;

	// Streamout
	LatteStreamout_FinishDrawcall(m_memoryManager->UseHostMemoryForCache());

	// Debug
	if (fetchVertexManually)
	    m_performanceMonitor.m_manualVertexFetchDraws++;
	if (usesGeometryShader)
	    m_performanceMonitor.m_meshDraws++;
	if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN)
	    m_performanceMonitor.m_triangleFans++;

	LatteGPUState.drawCallCounter++;
}

void MetalRenderer::draw_endSequence()
{
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	// post-drawcall logic
	if (pixelShader)
		LatteRenderTarget_trackUpdates();
	bool hasReadback = LatteTextureReadback_Update();
	m_recordedDrawcalls++;
	// The number of draw calls needs to twice as big, since we are interrupting the render pass
	// TODO: ucomment?
	// BOTW_COMMIT_MULT: how many times the threshold (2 as in Cemu). Each commit here ends the render pass, and the next
	// draw loads and stores every attachment again: headset counters 2026-11-08, the G-buffer's 3740 draws in 9-10 passes
	static const uint32 commit_mult = [] { const char* e = getenv("BOTW_COMMIT_MULT"); return e && atoi(e) > 0 ? (uint32)atoi(e) : 2u; }();
	if (m_recordedDrawcalls >= m_commitTreshold * commit_mult/* || hasReadback*/)
	{
		CommitCommandBuffer();

        // TODO: where should this be called?
        LatteTextureReadback_UpdateFinishedTransfers(false);
	}
}

void MetalRenderer::draw_updateVertexBuffersDirectAccess()
{
	LatteFetchShader* parsedFetchShader = LatteSHRC_GetActiveFetchShader();
	if (!parsedFetchShader)
		return;

	for (auto& bufferGroup : parsedFetchShader->bufferGroups)
	{
		uint32 bufferIndex = bufferGroup.attributeBufferIndex;
		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
		MPTR bufferAddress = LatteGPUState.contextRegister[bufferBaseRegisterIndex + 0];

		if (bufferAddress == MPTR_NULL) [[unlikely]]
			bufferAddress = m_memoryManager->GetImportedMemBaseAddress();

		m_state.m_vertexBufferOffsets[bufferIndex] = bufferAddress - m_memoryManager->GetImportedMemBaseAddress();
	}
}

void MetalRenderer::draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset)
{
	if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		for (const auto& buf : shader->list_quickBufferList)
		{
			sint32 i = buf.index;
			MPTR physicalAddr = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 0];
			uint32 uniformSize = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 1] + 1;

			if (physicalAddr == MPTR_NULL) [[unlikely]]
			{
				cemu_assert_unimplemented();
				continue;
			}
			uniformSize = std::min<uint32>(uniformSize, buf.size);

			cemu_assert_debug(physicalAddr < 0x50000000);

			uint32 bufferIndex = i;
			cemu_assert_debug(bufferIndex < 16);

			m_state.m_uniformBufferOffsets[GetMtlGeneralShaderType(shader->shaderType)][bufferIndex] = physicalAddr - m_memoryManager->GetImportedMemBaseAddress();
		}
	}
}

void MetalRenderer::draw_handleSpecialState5()
{
    LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();

	LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
	auto colorTextureMtl = static_cast<LatteTextureViewMtl*>(colorBuffer);
	auto depthTextureMtl = static_cast<LatteTextureViewMtl*>(depthBuffer);

	sint32 vpWidth, vpHeight;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);

	// Get the pipeline
	MTL::PixelFormat colorPixelFormat = colorTextureMtl->GetRGBAView()->pixelFormat();
	auto& pipeline = m_copyDepthToColorPipelines[colorPixelFormat];
	if (!pipeline)
	{
	    m_copyDepthToColorDesc->colorAttachments()->object(0)->setPixelFormat(colorPixelFormat);

        NS::Error* error = nullptr;
        pipeline = m_device->newRenderPipelineState(m_copyDepthToColorDesc, &error);
        if (error)
        {
            cemuLog_log(LogType::Force, "failed to create copy depth to color pipeline (error: {})", error->localizedDescription()->utf8String());
        }
	}

	// Sadly, we need to end encoding to ensure that the depth data is up-to-date
	EndEncoding();

	// Copy depth to color
	auto renderCommandEncoder = GetRenderCommandEncoder();

	auto& encoderState = m_state.m_encoderState;

	renderCommandEncoder->setRenderPipelineState(pipeline);
	// TODO: make a helper function for this
	encoderState.m_renderPipelineState = pipeline;
	SetTexture(renderCommandEncoder, METAL_SHADER_TYPE_FRAGMENT, depthTextureMtl->GetRGBAView(), GET_HELPER_TEXTURE_BINDING(0));
	// TODO: make a helper function for this
	renderCommandEncoder->setFragmentBytes(&vpWidth, sizeof(sint32), GET_HELPER_BUFFER_BINDING(0));
	encoderState.m_buffers[METAL_SHADER_TYPE_FRAGMENT][GET_HELPER_BUFFER_BINDING(0)] = {nullptr};

	renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle,  NS::UInteger(0),  NS::UInteger(3));
}

Renderer::IndexAllocation MetalRenderer::indexData_reserveIndexMemory(uint32 size)
{
    auto allocation = m_memoryManager->GetIndexAllocator().AllocateBufferMemory(size, 128);

    return {allocation->memPtr, allocation};
}

void MetalRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FreeReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

void MetalRenderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FlushReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

LatteQueryObject* MetalRenderer::occlusionQuery_create() {
	return new LatteQueryObjectMtl(this);
}

void MetalRenderer::occlusionQuery_destroy(LatteQueryObject* queryObj) {
    auto queryObjMtl = static_cast<LatteQueryObjectMtl*>(queryObj);
    delete queryObjMtl;
}

void MetalRenderer::occlusionQuery_flush() {
    if (m_occlusionQuery.m_lastCommandBuffer)
        m_occlusionQuery.m_lastCommandBuffer->waitUntilCompleted();
}

void MetalRenderer::occlusionQuery_updateState() {
    ProcessFinishedCommandBuffers();
}

void MetalRenderer::SetBuffer(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Buffer* buffer, size_t offset, uint32 index)
{
    auto& boundBuffer = m_state.m_encoderState.m_buffers[shaderType][index];
    if (buffer == boundBuffer.m_buffer && offset == boundBuffer.m_offset)
        return;

    if (buffer == boundBuffer.m_buffer)
    {
        // Update just the offset
        boundBuffer.m_offset = offset;

        switch (shaderType)
        {
        case METAL_SHADER_TYPE_VERTEX:
            renderCommandEncoder->setVertexBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_OBJECT:
            renderCommandEncoder->setObjectBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_MESH:
            renderCommandEncoder->setMeshBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_FRAGMENT:
            renderCommandEncoder->setFragmentBufferOffset(offset, index);
            break;
        }

        return;
    }

    boundBuffer = {buffer, offset};

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentBuffer(buffer, offset, index);
        break;
    }
}

void MetalRenderer::SetTexture(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Texture* texture, uint32 index)
{
    auto& boundTexture = m_state.m_encoderState.m_textures[shaderType][index];
    if (texture == boundTexture)
        return;

    boundTexture = texture;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentTexture(texture, index);
        break;
    }
}

void MetalRenderer::SetSamplerState(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::SamplerState* samplerState, uint32 index)
{
    auto& boundSamplerState = m_state.m_encoderState.m_samplers[shaderType][index];
    if (samplerState == boundSamplerState)
        return;

    boundSamplerState = samplerState;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentSamplerState(samplerState, index);
        break;
    }
}

MTL::CommandBuffer* MetalRenderer::GetCommandBuffer()
{
    bool needsNewCommandBuffer = (!m_currentCommandBuffer.m_commandBuffer || m_currentCommandBuffer.m_commited);
    if (needsNewCommandBuffer)
	{
        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();

        auto pool = NS::AutoreleasePool::alloc()->init();
	    MTL::CommandBuffer* mtlCommandBuffer = m_commandQueue->commandBuffer()->retain();
		pool->release();
		m_currentCommandBuffer = {mtlCommandBuffer};

		// Wait for the previous command buffer
		if (m_eventValue != -1)
		    mtlCommandBuffer->encodeWait(m_event, m_eventValue);

		m_recordedDrawcalls = 0;
		m_commitTreshold = m_defaultCommitTreshlod;

        // Debug
        m_performanceMonitor.m_commandBuffers++;

		return mtlCommandBuffer;
	}
	else
	{
	    return m_currentCommandBuffer.m_commandBuffer;
	}
}

MTL::RenderCommandEncoder* MetalRenderer::GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* renderPassDescriptor)
{
    EndEncoding();

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    flush_pending_clears(commandBuffer, g_merge_clears == 2 ? nullptr : renderPassDescriptor);  // it may sample anything
    dump_pass_begin(renderPassDescriptor);
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(renderPassDescriptor)->retain();
    dump_pass_started(renderPassDescriptor);
    pending_clears_restore(renderPassDescriptor);
    pool->release();
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Temporary render command encoder", renderCommandEncoder));
#endif
    m_commandEncoder = renderCommandEncoder;
    m_encoderType = MetalEncoderType::Render;

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

// Some render passes clear the attachments, forceRecreate is supposed to be used in those cases
MTL::RenderCommandEncoder* MetalRenderer::GetRenderCommandEncoder(bool forceRecreate)
{
    bool fboChanged = m_state.m_fboChanged;
    m_state.m_fboChanged = false;

    // Check if we need to begin a new render pass
    if (m_commandEncoder)
    {
        if (!forceRecreate)
        {
            if (m_encoderType == MetalEncoderType::Render)
            {
                bool needsNewRenderPass = false;
                if (fboChanged)
                {
                    needsNewRenderPass = (m_state.m_lastUsedFBO.m_fbo == nullptr);
                    if (!needsNewRenderPass)
                    {
                        for (uint8 i = 0; i < 8; i++)
                        {
                            if (m_state.m_activeFBO.m_fbo->colorBuffer[i].texture && m_state.m_activeFBO.m_fbo->colorBuffer[i].texture != m_state.m_lastUsedFBO.m_fbo->colorBuffer[i].texture)
                            {
                                needsNewRenderPass = true;
                                break;
                            }
                        }
                    }

                    if (!needsNewRenderPass)
                    {
                        if (m_state.m_activeFBO.m_fbo->depthBuffer.texture && (m_state.m_activeFBO.m_fbo->depthBuffer.texture != m_state.m_lastUsedFBO.m_fbo->depthBuffer.texture || ( m_state.m_activeFBO.m_fbo->depthBuffer.hasStencil && !m_state.m_lastUsedFBO.m_fbo->depthBuffer.hasStencil)))
                        {
                            needsNewRenderPass = true;
                        }
                    }
                }

                if (!needsNewRenderPass)
                {
                    return (MTL::RenderCommandEncoder*)m_commandEncoder;
                }
            }
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    flush_pending_clears(commandBuffer, m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor());
    dump_pass_begin(m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor());
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor())->retain();
    dump_pass_started(m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor());
    pending_clears_restore(m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor());
    pool->release();
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Render command encoder", renderCommandEncoder));
#endif
    m_commandEncoder = renderCommandEncoder;
    m_encoderType = MetalEncoderType::Render;

    // Update state
    m_state.m_lastUsedFBO = m_state.m_activeFBO;
    m_state.m_isFirstDrawInRenderPass = true;

    ResetEncoderState();

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

MTL::ComputeCommandEncoder* MetalRenderer::GetComputeCommandEncoder()
{
    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Compute)
        {
            return (MTL::ComputeCommandEncoder*)m_commandEncoder;
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    flush_pending_clears(commandBuffer, nullptr);
    MTL::ComputeCommandEncoder* computeCommandEncoder;
    if (auto* d = g_dumping ? dump_encoder_descriptor<MTL::ComputePassDescriptor>(2) : nullptr)
    {
        computeCommandEncoder = commandBuffer->computeCommandEncoder(d)->retain();
        d->release();
    }
    else
        computeCommandEncoder = commandBuffer->computeCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = computeCommandEncoder;
    m_encoderType = MetalEncoderType::Compute;

    ResetEncoderState();

    return computeCommandEncoder;
}

MTL::BlitCommandEncoder* MetalRenderer::GetBlitCommandEncoder()
{
    if (g_merge_clears == 2 && !g_pending_clears.empty())
    {
        // a clear of a texture this blit reads or writes runs first; an unknown blit runs them all
        const bool hit = !g_blit_touch_known || pending_clear_touches(g_blit_touch[0]) || pending_clear_touches(g_blit_touch[1]);
        if (hit)
        {
            EndEncoding();
            if (g_blit_touch_known) run_pending_clears_of(GetCommandBuffer(), g_blit_touch[0], g_blit_touch[1]);
            else flush_pending_clears(GetCommandBuffer(), nullptr);
        }
    }
    g_blit_touch_known = false;
    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Blit)
        {
            return (MTL::BlitCommandEncoder*)m_commandEncoder;
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    if (g_merge_clears != 2) flush_pending_clears(commandBuffer, nullptr);
    MTL::BlitCommandEncoder* blitCommandEncoder;
    if (auto* d = g_dumping ? dump_encoder_descriptor<MTL::BlitPassDescriptor>(1) : nullptr)
    {
        blitCommandEncoder = commandBuffer->blitCommandEncoder(d)->retain();
        d->release();
    }
    else
        blitCommandEncoder = commandBuffer->blitCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = blitCommandEncoder;
    m_encoderType = MetalEncoderType::Blit;

    ResetEncoderState();

    return blitCommandEncoder;
}

void MetalRenderer::EndEncoding()
{
    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Render) dump_pass_end();
        m_commandEncoder->endEncoding();
        m_commandEncoder->release();
        m_commandEncoder = nullptr;
        m_encoderType = MetalEncoderType::None;

        // Commit the command buffer if enough draw calls have been recorded
        if (m_recordedDrawcalls >= m_commitTreshold)
            CommitCommandBuffer();
    }
}

void MetalRenderer::CommitCommandBuffer()
{
    if (!m_currentCommandBuffer.m_commandBuffer)
        return;

    EndEncoding();

    ProcessFinishedCommandBuffers();

    // Commit the command buffer
    if (!m_currentCommandBuffer.m_commited)
    {
        if (!g_pending_clears.empty() && g_merge_clears != 2) flush_pending_clears(m_currentCommandBuffer.m_commandBuffer, nullptr);
        // Handled differently, since it seems like Metal doesn't always call the completion handler
        //commandBuffer.m_commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
        //    m_memoryManager->GetTemporaryBufferAllocator().CommandBufferFinished(commandBuffer.m_commandBuffer);
        //});

        // Signal event
        m_eventValue = (m_eventValue + 1) % EVENT_VALUE_WRAP;
        auto mtlCommandBuffer = m_currentCommandBuffer.m_commandBuffer;
        mtlCommandBuffer->encodeSignalEvent(m_event, m_eventValue);

        // BotWRecomp: the game's GPU time (overlay_menu.cpp's [gpu] line), to judge render scale against the frame
        // budget; statistics only, so a handler Metal skips now and then does not matter
        const uint32 drawcallAtCommit = LatteGPUState.drawCallCounter;
        mtlCommandBuffer->addCompletedHandler(^(MTL::CommandBuffer* cb) {
            botw_gpu_busy_add(cb->GPUStartTime(), cb->GPUEndTime());
            // redirect buffer cache: the GPU is past every draw recorded before this commit
            uint32 cur = m_gpuDoneDrawcall.load(std::memory_order_relaxed);
            while ((sint32)(drawcallAtCommit - cur) > 0 && !m_gpuDoneDrawcall.compare_exchange_weak(cur, drawcallAtCommit)) {}
        });
        mtlCommandBuffer->commit();
        m_currentCommandBuffer.m_commited = true;

        m_executingCommandBuffers.push_back(mtlCommandBuffer);
        RedirectRingCommit(mtlCommandBuffer);

        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();
    }
}

void MetalRenderer::ProcessFinishedCommandBuffers()
{
    // Check for finished command buffers
    for (auto it = m_executingCommandBuffers.begin(); it != m_executingCommandBuffers.end();)
    {
        auto commandBuffer = *it;
        if (CommandBufferCompleted(commandBuffer))
        {
            m_memoryManager->CleanupBuffers(commandBuffer);
            commandBuffer->release();
            it = m_executingCommandBuffers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// SheikahVision VR (visionOS): once the game shows only in the headset, its window is closed; from then on nothing is drawn
// into the window's layer or presented there (its nextDrawable would wait up to a second each frame for a window that
// is gone)
std::atomic<bool> g_botw_main_window_closed{false};
// the game's GPU time since the last [gpu] line (seconds), from its command buffers' completion
// Command buffers overlap on the GPU, so each adds only the part of its run not already counted (the union of the
// busy intervals; completions arrive about in order)
static std::mutex g_botw_gpu_lock;
static double g_botw_gpu_busy = 0, g_botw_gpu_last_end = 0;
void botw_gpu_busy_add(double start, double end) {
    if (!(end > start) || end - start > 1) return;
    std::lock_guard<std::mutex> l(g_botw_gpu_lock);
    const double from = std::max(start, g_botw_gpu_last_end);
    if (end > from) g_botw_gpu_busy += end - from;
    g_botw_gpu_last_end = std::max(g_botw_gpu_last_end, end);
}
extern "C" double botw_gpu_busy_take(void) {
    std::lock_guard<std::mutex> l(g_botw_gpu_lock);
    const double v = g_botw_gpu_busy;
    g_botw_gpu_busy = 0;
    return v;
}
extern "C" void botw_main_window_closed(void) { g_botw_main_window_closed = true; }

bool MetalRenderer::AcquireDrawable(bool mainWindow)
{
    if (mainWindow && g_botw_main_window_closed.load(std::memory_order_relaxed))
        return false;
    auto& layer = GetLayer(mainWindow);
    if (!layer.GetLayer())
        return false;

    const bool latteBufferUsesSRGB = mainWindow ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB;
    if (latteBufferUsesSRGB != m_state.m_usesSRGB)
    {
        layer.GetLayer()->setPixelFormat(latteBufferUsesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm);
        m_state.m_usesSRGB = latteBufferUsesSRGB;
    }

    return layer.AcquireDrawable();
}

/*
bool MetalRenderer::CheckIfRenderPassNeedsFlush(LatteDecompilerShader* shader)
{
    sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (int i = 0; i < textureCount; ++i)
	{
		const auto relative_textureUnit = shader->resourceMapping.getTextureUnitFromBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;
		auto textureDim = shader->textureUnitDim[relative_textureUnit];

		// Texture is accessed as a framebuffer fetch, therefore there is no need to flush it
		if (shader->textureRenderTargetIndex[relative_textureUnit] != 255)
		    continue;

		auto texUnitRegIndex = hostTextureUnit * 7;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
			break;
		default:
			UNREACHABLE;
		}

		auto textureView = m_state.m_textures[hostTextureUnit];
		if (!textureView)
            continue;

		LatteTexture* baseTexture = textureView->baseTexture;

	    // If the texture is also used in the current render pass, we need to end the render pass to "flush" the texture
		for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
		{
		    auto colorTarget = m_state.m_activeFBO.m_fbo->colorBuffer[i].texture;
			if (colorTarget && colorTarget->baseTexture == baseTexture)
			    return true;
		}
	}

	return false;
}
*/

void MetalRenderer::BindStageResources(MTL::RenderCommandEncoder* renderCommandEncoder, LatteDecompilerShader* shader, bool usesGeometryShader)
{
    auto mtlShaderType = GetMtlShaderType(shader->shaderType, usesGeometryShader);

    sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (int i = 0; i < textureCount; ++i)
	{
		const auto relative_textureUnit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;

		// Don't bind textures that are accessed with a framebuffer fetch
		if (m_supportsFramebufferFetch && shader->textureRenderTargetIndex[relative_textureUnit] != 255)
            continue;

		auto textureDim = shader->textureUnitDim[relative_textureUnit];
		auto texUnitRegIndex = hostTextureUnit * 7;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
			break;
		default:
			UNREACHABLE;
		}

		// TODO: correct?
		uint32 binding = shader->resourceMapping.getTextureBaseBindingPoint() + i;
		if (binding >= MAX_MTL_TEXTURES)
		{
		    cemuLog_logOnce(LogType::Force, "invalid texture binding {}", binding);
            continue;
		}

		auto textureView = m_state.m_textures[hostTextureUnit];
		if (!textureView)
		{
            if (textureDim == Latte::E_DIM::DIM_1D)
                SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture1D, binding);
           	else
                SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture2D, binding);
            if (binding < 16) SetSamplerState(renderCommandEncoder, mtlShaderType, m_nearestSampler, binding);
            continue;
		}

		if (textureDim == Latte::E_DIM::DIM_1D && (textureView->dim != Latte::E_DIM::DIM_1D))
		{
		    SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture1D, binding);
			continue;
		}
		else if (textureDim == Latte::E_DIM::DIM_2D && (textureView->dim != Latte::E_DIM::DIM_2D && textureView->dim != Latte::E_DIM::DIM_2D_MSAA))
		{
		    SetTexture(renderCommandEncoder, mtlShaderType, m_nullTexture2D, binding);
			continue;
		}

		LatteTexture* baseTexture = textureView->baseTexture;

		uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relative_textureUnit];
		MTL::SamplerState* sampler;
		if (stageSamplerIndex != LATTE_DECOMPILER_SAMPLER_NONE)
		{
		    uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
			_LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;

			// Overwriting

            // Lod bias
            //if (baseTexture->overwriteInfo.hasLodBias)
            //    samplerWords->WORD1.set_LOD_BIAS(baseTexture->overwriteInfo.lodBias);
            //else if (baseTexture->overwriteInfo.hasRelativeLodBias)
            //    samplerWords->WORD1.set_LOD_BIAS(samplerWords->WORD1.get_LOD_BIAS() + baseTexture->overwriteInfo.relativeLodBias);

            // Max anisotropy
            if (baseTexture->overwriteInfo.anisotropicLevel >= 0)
                samplerWords->WORD0.set_MAX_ANISO_RATIO(baseTexture->overwriteInfo.anisotropicLevel);

    		sampler = m_samplerCache->GetSamplerState(LatteGPUState.contextNew, shader->shaderType, stageSamplerIndex, samplerWords);
		}
		else
		{
		    sampler = m_nearestSampler;
		}
        if (binding < 16) SetSamplerState(renderCommandEncoder, mtlShaderType, sampler, binding);  // past 15: shares slot 15 (the shader header)

		// get texture register word 0
		uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
		auto& boundTexture = m_state.m_encoderState.m_textures[mtlShaderType][binding];
		MTL::Texture* mtlTexture = textureView->GetSwizzledView(word4);
		SetTexture(renderCommandEncoder, mtlShaderType, mtlTexture, binding);
	}

	// Support buffer
	auto GET_UNIFORM_DATA_PTR = [&](size_t index) { return supportBufferData + (index / 4); };

	sint32 shaderAluConst;
	sint32 shaderUniformRegisterOffset;

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		shaderUniformRegisterOffset = mmSQ_VTX_UNIFORM_BLOCK_START;
		break;
	case LatteConst::ShaderType::Pixel:
		shaderAluConst = 0;
		shaderUniformRegisterOffset = mmSQ_PS_UNIFORM_BLOCK_START;
		break;
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0; // geometry shader has no ALU const
		shaderUniformRegisterOffset = mmSQ_GS_UNIFORM_BLOCK_START;
		break;
	default:
		UNREACHABLE;
	}

	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		if (shader->uniform.list_ufTexRescale.empty() == false)
		{
			for (auto& entry : shader->uniform.list_ufTexRescale)
			{
				float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
				memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
				memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
			}
		}
		if (shader->uniform.loc_alphaTestRef >= 0)
		{
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
		}
		if (shader->uniform.loc_pointSize >= 0)
		{
			const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
			float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
			if (pointWidth == 0.0f)
				pointWidth = 1.0f / 8.0f; // minimum size
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
		}
		if (shader->uniform.loc_remapped >= 0)
		{
			LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped), true, (1<<LATTE_NUM_MAX_UNIFORM_BUFFERS)-1);
		}
		if (shader->uniform.loc_uniformRegister >= 0)
		{
			uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
			memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
		}
		if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
		{
			sint32 viewportWidth;
			sint32 viewportHeight;
			LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight); // always call after _updateViewport()
			float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
			v[0] = 2.0f / (float)viewportWidth;
			v[1] = 2.0f / (float)viewportHeight;
		}
		if (shader->uniform.loc_fragCoordScale >= 0)
		{
			LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
		}
		for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
		{
			if (shader->uniform.loc_framebufferFetchSize[t] < 0)
				continue;
			const auto& texture = LatteGPUState.contextNew.SQ_TEX_START_PS[t];
			sint32* size = reinterpret_cast<sint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_framebufferFetchSize[t]));
			size[0] = texture.word0.get_WIDTH() + 1;
			size[1] = texture.word1.get_HEIGHT() + 1;
		}
		if (shader->uniform.loc_verticesPerInstance >= 0)
		{
			*(int*)(supportBufferData + ((size_t)shader->uniform.loc_verticesPerInstance / 4)) = m_state.m_streamoutState.verticesPerInstance;
			for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			{
				if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
				{
					*(uint32*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]) = m_state.m_streamoutState.buffers[b].ringBufferOffset;
				}
			}
		}

		size_t size = shader->uniform.uniformRangeSize;
		auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
		auto allocation = bufferAllocator.AllocateBufferMemory(size, 1);
		memcpy(allocation.memPtr, supportBufferData, size);
		bufferAllocator.FlushReservation(allocation);

		SetBuffer(renderCommandEncoder, mtlShaderType, allocation.mtlBuffer, allocation.bufferOffset, shader->resourceMapping.uniformVarsBufferBindingPoint);
	}

	// Uniform buffers
	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (shader->resourceMapping.uniformBuffersBindingPoint[i] >= 0)
		{
    		uint32 binding = shader->resourceMapping.uniformBuffersBindingPoint[i];
    		if (binding >= MAX_MTL_BUFFERS)
    		{
    		    cemuLog_logOnce(LogType::Force, "invalid buffer binding {}", binding);
    			continue;
    		}

    		size_t offset = m_state.m_uniformBufferOffsets[GetMtlGeneralShaderType(shader->shaderType)][i];
    		if (offset == INVALID_OFFSET)
                continue;

            MTL::Buffer* redirect = m_state.m_uniformBufferRedirect[GetMtlGeneralShaderType(shader->shaderType)][i];
            SetBuffer(renderCommandEncoder, mtlShaderType, redirect ? redirect : m_memoryManager->GetBufferCache(), offset, binding);
		}
	}

	// Storage buffer
	if (shader->resourceMapping.tfStorageBindingPoint >= 0)
	{
        SetBuffer(renderCommandEncoder, mtlShaderType, m_xfbRingBuffer, 0, shader->resourceMapping.tfStorageBindingPoint);
	}
}

void MetalRenderer::ClearColorTextureInternal(MTL::Texture* mtlTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(mtlTexture);
    colorAttachment->setClearColor(MTL::ClearColor(r, g, b, a));
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setSlice(sliceIndex);
    colorAttachment->setLevel(mipIndex);

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

void MetalRenderer::CopyBufferToBuffer(MTL::Buffer* src, uint32 srcOffset, MTL::Buffer* dst, uint32 dstOffset, uint32 size, MTL::RenderStages after, MTL::RenderStages before)
{
    g_botw_blit_reason = "buffer copy";
    botw_blit_touches(nullptr, nullptr);
    // TODO: uncomment and fix performance issues
    // Do the copy in a vertex shader on Apple GPUs
    /*
    if (m_isAppleGPU && m_encoderType == MetalEncoderType::Render)
    {
        auto renderCommandEncoder = static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder);

        MTL::Resource* barrierBuffers[] = {src};
        renderCommandEncoder->memoryBarrier(barrierBuffers, 1, after, after | MTL::RenderStageVertex);

		renderCommandEncoder->setRenderPipelineState(m_copyBufferToBufferPipeline->GetRenderPipelineState());
		m_state.m_encoderState.m_renderPipelineState = m_copyBufferToBufferPipeline->GetRenderPipelineState();

		SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, src, srcOffset, GET_HELPER_BUFFER_BINDING(0));
		SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, dst, dstOffset, GET_HELPER_BUFFER_BINDING(1));

		renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(size));

		barrierBuffers[0] = dst;
        renderCommandEncoder->memoryBarrier(barrierBuffers, 1, before | MTL::RenderStageVertex, before);
    }
    else
    {
    */
        auto blitCommandEncoder = GetBlitCommandEncoder();

        blitCommandEncoder->copyFromBuffer(src, srcOffset, dst, dstOffset, size);
    //}
}

void MetalRenderer::SwapBuffer(bool mainWindow)
{
    if (!AcquireDrawable(mainWindow))
        return;

    auto commandBuffer = GetCommandBuffer();
    GetLayer(mainWindow).PresentDrawable(commandBuffer);
}

void MetalRenderer::EnsureImGuiBackend()
{
    if (!ImGui::GetIO().BackendRendererUserData)
    {
        ImGui_ImplMetal_Init(m_device);
        //ImGui_ImplMetal_CreateFontsTexture(m_device);
    }
}

void MetalRenderer::StartCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    auto desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(m_device);

    // Check if a debugger with support for GPU capture is attached
    if (captureManager->supportsDestination(MTL::CaptureDestinationDeveloperTools))
    {
        desc->setDestination(MTL::CaptureDestinationDeveloperTools);
    }
    else
    {
        if (GetConfig().gpu_capture_dir.GetValue().empty())
        {
            cemuLog_log(LogType::Force, "No GPU capture directory specified, cannot do a GPU capture");
            return;
        }

        // Check if the GPU trace document destination is available
        if (!captureManager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument))
        {
            cemuLog_log(LogType::Force, "GPU trace document destination is not available, cannot do a GPU capture");
            return;
        }

        // Get current date and time as a string
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
        std::string now_str = oss.str();

        std::string capturePath = fmt::format("{}/cemu_{}.gputrace", GetConfig().gpu_capture_dir.GetValue(), now_str);
        desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
        desc->setOutputURL(ToNSURL(capturePath));
    }

    NS::Error* error = nullptr;
    captureManager->startCapture(desc, &error);
    if (error)
    {
        cemuLog_log(LogType::Force, "Failed to start GPU capture: {}", error->localizedDescription()->utf8String());
    }

    m_capturing = true;
}

void MetalRenderer::EndCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    captureManager->stopCapture();

    m_capturing = false;
}
