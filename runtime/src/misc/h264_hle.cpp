// h264.rpl (H264DEC*): the Wii U's H.264 decoder library, used by the game for its pre-rendered movies
// (content/Movie/*.mp4, 1280x720 High profile with B-frames; the game demuxes the MP4 itself and hands
// this library one Annex-B access unit per H264DECExecute).
//
// Decoding is done by VideoToolbox (the hardware decoder): the parameter sets become a format description,
// the slices of an access unit one AVCC sample, decoded synchronously on the calling (game decoder) thread.
// The decoded NV12 picture is copied straight into the guest frame buffer passed to that H264DECExecute,
// in the Wii U layout: luma rows at a pitch of the width rounded up to 256, the interleaved chroma plane
// at pitch * height. Frames are handed back through the game's output callback in display order (picture
// order count from the slice header), after the same 5-frame delay Cemu uses in buffered mode.
//
// API semantics follow Cemu (src/Cafe/OS/libs/h264_avc/H264Dec.cpp), including its Breath of the Wild
// allowances (small work memory, 1920x1088 reported as 1080).
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../hle.h"

namespace {
constexpr uint32_t kOk = 0;
constexpr uint32_t kBadStream = 0x1000000;
constexpr uint32_t kInvalidParam = 0x1010000;
constexpr uint32_t kWorkMemorySize = 0x1000;  // the decoder state lives on the host; the game only keeps the handle
constexpr uint32_t kBufferedFrames = 5;       // frames held back before the first output (OUTPUT_PER_FRAME = 0)

// ---- bitstream parsing ---------------------------------------------------------------------------
// RBSP bit reader over a NAL payload (skips emulation prevention bytes: 00 00 03 -> 00 00).
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    int zeros = 0, bit = 0;
    uint8_t cur = 0;
    bool eof = false;
    Bits(const uint8_t* data, size_t len) : p(data), n(len) {}
    uint32_t u1() {
        if (bit == 0) {
            if (pos < n && zeros >= 2 && p[pos] == 3) {
                pos++;
                zeros = 0;
            }
            if (pos >= n) {
                eof = true;
                return 0;
            }
            cur = p[pos++];
            zeros = cur == 0 ? zeros + 1 : 0;
            bit = 8;
        }
        return (cur >> --bit) & 1;
    }
    uint32_t u(int k) {
        uint32_t v = 0;
        while (k-- > 0) v = (v << 1) | u1();
        return v;
    }
    uint32_t ue() {
        int lz = 0;
        while (!eof && lz < 32 && u1() == 0) lz++;
        if (lz >= 32) return 0xFFFFFFFFu;
        return (uint32_t)(((uint64_t)1 << lz) - 1 + u(lz));
    }
    int32_t se() {
        uint32_t v = ue();
        return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
    }
};

struct Sps {
    bool valid = false;
    uint32_t profile = 0, level = 0;
    uint32_t width = 0, height = 0;  // coded size (macroblock aligned)
    bool frame_mbs_only = true, separate_colour_plane = false;
    uint32_t log2_max_frame_num = 4, poc_type = 0, log2_max_poc_lsb = 4;
    bool crop = false;
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
};

void skip_scaling_list(Bits& b, int size) {
    int last = 8, next = 8;
    for (int i = 0; i < size; i++) {
        if (next != 0) next = (last + b.se() + 256) % 256;
        if (next != 0) last = next;
    }
}

// `data` starts after the NAL header byte.
bool parse_sps(const uint8_t* data, size_t len, Sps& s) {
    s = {};
    Bits b(data, len);
    s.profile = b.u(8);
    b.u(8);  // constraint flags
    s.level = b.u(8);
    if (b.ue() >= 32) return false;  // seq_parameter_set_id
    uint32_t chroma = 1;
    switch (s.profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134:
        chroma = b.ue();
        if (chroma > 3) return false;
        if (chroma == 3) s.separate_colour_plane = b.u1();
        b.ue();  // bit_depth_luma_minus8
        b.ue();  // bit_depth_chroma_minus8
        b.u1();  // qpprime_y_zero_transform_bypass_flag
        if (b.u1())  // seq_scaling_matrix_present_flag
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
                if (b.u1()) skip_scaling_list(b, i < 6 ? 16 : 64);
        break;
    default: break;
    }
    s.log2_max_frame_num = b.ue() + 4;
    if (s.log2_max_frame_num > 16) return false;
    s.poc_type = b.ue();
    if (s.poc_type > 2) return false;
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = b.ue() + 4;
        if (s.log2_max_poc_lsb > 16) return false;
    } else if (s.poc_type == 1) {
        b.u1();
        b.se();
        b.se();
        uint32_t n = b.ue();
        if (n > 255) return false;
        for (uint32_t i = 0; i < n; i++) b.se();
    }
    if (b.ue() > 16) return false;  // max_num_ref_frames
    b.u1();                         // gaps_in_frame_num_value_allowed_flag
    uint32_t w_mbs = b.ue() + 1, h_units = b.ue() + 1;
    s.frame_mbs_only = b.u1();
    if (!s.frame_mbs_only) b.u1();  // mb_adaptive_frame_field_flag
    b.u1();                         // direct_8x8_inference_flag
    s.crop = b.u1();
    if (s.crop) {
        s.crop_left = b.ue();
        s.crop_right = b.ue();
        s.crop_top = b.ue();
        s.crop_bottom = b.ue();
    }
    if (b.eof || w_mbs > 512 || h_units > 512) return false;
    s.width = 16 * w_mbs;
    s.height = 16 * h_units * (s.frame_mbs_only ? 1 : 2);
    s.valid = true;
    return true;
}

struct Nal {
    const uint8_t* data;  // at the NAL header byte
    size_t len;
    uint8_t type() const { return data[0] & 0x1F; }
    uint8_t ref_idc() const { return (data[0] >> 5) & 3; }
};

// Splits an Annex-B buffer at its start codes; trailing zero bytes of a NAL are dropped.
void split_annexb(const uint8_t* p, size_t n, std::vector<Nal>& out) {
    out.clear();
    size_t i = 0, start = SIZE_MAX;
    auto close = [&](size_t end) {
        if (start == SIZE_MAX) return;
        while (end > start && p[end - 1] == 0) end--;
        if (end > start) out.push_back({p + start, end - start});
    };
    while (i + 3 <= n) {
        if (p[i + 2] > 1) {
            i += 3;
        } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            close(i);
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    close(n);
}

// The size the game allocates its frame buffers for: the coded size, 1920x1088 reported as 1080 (Cemu's
// allowance for the Switch version's movies; the picture is then written cropped).
void image_size(const Sps& s, uint32_t& w, uint32_t& h) {
    w = s.width;
    h = s.height;
    if (w == 1920 && h == 1088) h = 1080;
}

// ---- decoder ---------------------------------------------------------------------------------------
struct Frame {
    uint32_t image = 0;  // guest frame buffer of this picture
    double timestamp = 0;
    uint64_t order = 0;  // display order: (sequence << 32) | biased picture order count
    uint32_t width = 0, height = 0, pitch = 0;
    Sps sps;
};

struct Decoder {
    // parameters (H264DECSetParam_*)
    uint32_t output_fn = 0, user = 0;
    bool per_frame = false;
    // H264DECSetBitstream
    uint32_t bs_ptr = 0, bs_len = 0;
    double bs_time = 0;
    // stream state
    bool opened = false, first_begin = true, recovering = false;
    std::vector<uint8_t> sps_nal;
    std::vector<std::vector<uint8_t>> pps_nals;
    bool params_changed = false;
    Sps sps;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    // picture order count (8.2.1)
    uint32_t sequence = 0, prev_poc_msb = 0, prev_poc_lsb = 0, prev_frame_num = 0, frame_num_offset = 0;
    uint64_t decode_count = 0;
    std::vector<Frame> held;  // decoded, not yet handed to the game
    uint32_t cb_block = 0;    // guest memory for the output callback's arguments
    // statistics
    uint64_t frames = 0, failed = 0, output = 0, decode_ticks = 0;
    bool logged_format = false;
    std::vector<Nal> nals;
    std::vector<uint8_t> sample;

    ~Decoder() {
        drop_session();
        if (format) CFRelease(format);
        if (cb_block) mem::sys_free(cb_block);
    }
    void drop_session() {
        if (!session) return;
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
    }
};

std::mutex g_lock;
std::unordered_map<uint32_t, std::unique_ptr<Decoder>> g_decoders;  // by work memory address

Decoder* find(uint32_t work) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_decoders.find(work);
    return it == g_decoders.end() ? nullptr : it->second.get();
}

double ticks_ms(uint64_t t) {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t v;
        mach_timebase_info(&v);
        return v;
    }();
    return (double)t * tb.numer / tb.denom / 1e6;
}

struct DecodeTarget {
    Decoder* d;
    Frame* f;
    bool done;
};

// Copies the decoded picture into the guest frame buffer (Y plane, then interleaved CbCr at pitch * height).
void copy_picture(Decoder* d, Frame* f, CVPixelBufferRef pb) {
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    if (!d->logged_format) {
        d->logged_format = true;
        LOG("h264: decoding %ux%u (profile %u level %u, picture order type %u), decoder output %zux%zu '%c%c%c%c', "
            "guest pitch %u",
            f->sps.width, f->sps.height, f->sps.profile, f->sps.level, f->sps.poc_type, CVPixelBufferGetWidth(pb),
            CVPixelBufferGetHeight(pb), (char)(fmt >> 24), (char)(fmt >> 16), (char)(fmt >> 8), (char)fmt, f->pitch);
    }
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    uint8_t* y_out = mem::host(f->image);
    uint8_t* uv_out = y_out + (size_t)f->pitch * f->height;
    const size_t planes = CVPixelBufferGetPlaneCount(pb);
    if (planes >= 2) {
        const uint8_t* y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        const size_t w = std::min<size_t>(CVPixelBufferGetWidthOfPlane(pb, 0), f->width);
        const size_t h = std::min<size_t>(CVPixelBufferGetHeightOfPlane(pb, 0), f->height);
        for (size_t r = 0; r < h; r++) memcpy(y_out + r * f->pitch, y + r * ys, w);
        const uint8_t* c0 = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        const size_t cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        const size_t ch = std::min<size_t>(CVPixelBufferGetHeightOfPlane(pb, 1), f->height / 2);
        if (planes == 2) {  // NV12: the guest layout
            const size_t cw = std::min<size_t>(CVPixelBufferGetWidthOfPlane(pb, 1) * 2, f->width);
            for (size_t r = 0; r < ch; r++) memcpy(uv_out + r * f->pitch, c0 + r * cs, cw);
        } else {  // planar 4:2:0: interleave
            const uint8_t* c1 = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 2);
            const size_t cs1 = CVPixelBufferGetBytesPerRowOfPlane(pb, 2);
            const size_t cw = std::min<size_t>(CVPixelBufferGetWidthOfPlane(pb, 1), f->width / 2);
            for (size_t r = 0; r < ch; r++) {
                uint8_t* o = uv_out + r * f->pitch;
                for (size_t x = 0; x < cw; x++) {
                    o[2 * x] = c0[r * cs + x];
                    o[2 * x + 1] = c1[r * cs1 + x];
                }
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

void on_decoded(void*, void* frame_refcon, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
    DecodeTarget* t = (DecodeTarget*)frame_refcon;
    if (status != noErr || !image) return;
    copy_picture(t->d, t->f, (CVPixelBufferRef)image);
    t->done = true;
}

// (Re)creates the format description and, when the running session cannot take it, the session.
bool prepare_session(Decoder* d) {
    if (d->session && !d->params_changed) return true;
    if (d->sps_nal.empty() || d->pps_nals.empty()) return false;
    std::vector<const uint8_t*> ptrs{d->sps_nal.data()};
    std::vector<size_t> sizes{d->sps_nal.size()};
    for (auto& p : d->pps_nals) {
        ptrs.push_back(p.data());
        sizes.push_back(p.size());
    }
    CMVideoFormatDescriptionRef fmt = nullptr;
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, ptrs.size(), ptrs.data(),
                                                                      sizes.data(), 4, &fmt);
    if (st != noErr || !fmt) {
        LOG("h264: the stream's parameter sets were rejected (%d)", (int)st);
        return false;
    }
    if (d->format) CFRelease(d->format);
    d->format = fmt;
    d->params_changed = false;
    if (d->session && VTDecompressionSessionCanAcceptFormatDescription(d->session, fmt)) return true;
    d->drop_session();
    // 8-bit biplanar 4:2:0 in the stream's own range: the decoder's native output, no conversion pass
    const void* keys[] = {kCVPixelBufferPixelFormatTypeKey};
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (CFDictionaryRef ext = CMFormatDescriptionGetExtensions(fmt)) {
        CFTypeRef full = CFDictionaryGetValue(ext, kCMFormatDescriptionExtension_FullRangeVideo);
        if (full && CFGetTypeID(full) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)full))
            pf = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    }
    CFNumberRef pfn = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
    const void* vals[] = {pfn};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb{on_decoded, d};
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt, nullptr, attrs, &cb, &d->session);
    CFRelease(attrs);
    CFRelease(pfn);
    if (st != noErr || !d->session) {
        LOG("h264: could not create the video decoder (%d)", (int)st);
        d->session = nullptr;
        return false;
    }
    VTSessionSetProperty(d->session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    return true;
}

OSStatus decode_sample(Decoder* d, Frame* f, bool& done) {
    CMBlockBufferRef bb = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, d->sample.size(), kCFAllocatorDefault,
                                                     nullptr, 0, d->sample.size(), kCMBlockBufferAssureMemoryNowFlag, &bb);
    if (st == noErr) st = CMBlockBufferReplaceDataBytes(d->sample.data(), bb, 0, d->sample.size());
    CMSampleBufferRef sb = nullptr;
    const size_t size = d->sample.size();
    if (st == noErr) st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, d->format, 1, 0, nullptr, 1, &size, &sb);
    if (st == noErr) {
        DecodeTarget target{d, f, false};
        VTDecodeInfoFlags info = 0;
        // flags 0: synchronous, the output callback has run when this returns
        st = VTDecompressionSessionDecodeFrame(d->session, sb, 0, &target, &info);
        done = target.done;
    }
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    return st;
}

// Display order of the picture whose first slice is `n` (8.2.1; types 0 and 2, decode order otherwise).
uint64_t picture_order(Decoder* d, const Nal& n) {
    const Sps& s = d->sps;
    const bool idr = n.type() == 5;
    if (idr) {
        d->sequence++;
        d->prev_poc_msb = d->prev_poc_lsb = d->prev_frame_num = d->frame_num_offset = 0;
    }
    Bits b(n.data + 1, std::min<size_t>(n.len - 1, 64));
    b.ue();  // first_mb_in_slice
    b.ue();  // slice_type
    b.ue();  // pic_parameter_set_id
    if (s.separate_colour_plane) b.u(2);
    const uint32_t frame_num = b.u(s.log2_max_frame_num);
    if (!s.frame_mbs_only && b.u1()) b.u1();  // field_pic_flag, bottom_field_flag
    if (idr) b.ue();                          // idr_pic_id
    int64_t poc = (int64_t)d->decode_count;
    if (s.poc_type == 0) {
        const uint32_t lsb = b.u(s.log2_max_poc_lsb), max = 1u << s.log2_max_poc_lsb;
        uint32_t msb = d->prev_poc_msb;
        if (lsb < d->prev_poc_lsb && d->prev_poc_lsb - lsb >= max / 2) msb += max;
        else if (lsb > d->prev_poc_lsb && lsb - d->prev_poc_lsb > max / 2) msb -= max;
        poc = (int32_t)(msb + lsb);
        if (n.ref_idc()) {
            d->prev_poc_msb = msb;
            d->prev_poc_lsb = lsb;
        }
    } else if (s.poc_type == 2) {
        if (d->prev_frame_num > frame_num) d->frame_num_offset += 1u << s.log2_max_frame_num;
        poc = 2 * (int64_t)(d->frame_num_offset + frame_num) - (n.ref_idc() ? 0 : 1);
        d->prev_frame_num = frame_num;
    }
    d->decode_count++;
    return ((uint64_t)d->sequence << 32) | (uint32_t)((int32_t)poc + 0x40000000);
}

// Hands the earliest held frame to the game's output callback.
void output_frame(PPCContext* c, Decoder* d) {
    auto it = std::min_element(d->held.begin(), d->held.end(),
                               [](const Frame& a, const Frame& b) { return a.order < b.order; });
    Frame f = *it;
    d->held.erase(it);
    d->output++;
    // BOTW_H264_DUMP=<folder>: the first 12 frames and every 100th as handed to the game (raw NV12 at the guest
    // pitch, named by output index and timestamp), to compare with a reference decoder
    static const char* dump = getenv("BOTW_H264_DUMP");
    if (dump && (d->output <= 12 || d->output % 100 == 0)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/out_%05llu_ts%.0f_%ux%u_pitch%u.nv12", dump, (unsigned long long)d->output,
                 f.timestamp, f.width, f.height, f.pitch);
        if (FILE* fp = fopen(path, "wb")) {
            fwrite(mem::host(f.image), 1, (size_t)f.pitch * f.height * 3 / 2, fp);
            fclose(fp);
        }
    }
    if (!d->output_fn) return;
    // H264OutputCBStruct {count, H264DECFrameOutput** frames, user} + the pointer array + one H264DECFrameOutput
    if (!d->cb_block) d->cb_block = mem::sys_alloc(0x10 + 0x10 + 0x80, 16);
    const uint32_t cb = d->cb_block, arr = cb + 0x10, out = cb + 0x20;
    memset(mem::host(cb), 0, 0x10 + 0x10 + 0x80);
    mem::w32(cb + 0, 1);
    mem::w32(cb + 4, arr);
    mem::w32(cb + 8, d->user);
    mem::w32(arr, out);
    uint64_t ts;
    memcpy(&ts, &f.timestamp, 8);
    mem::w32(out + 0x00, 100);  // result
    mem::w64(out + 0x08, ts);
    mem::w32(out + 0x10, f.width);
    mem::w32(out + 0x14, f.height);
    mem::w32(out + 0x18, f.pitch);
    const bool cropped_already = f.height != f.sps.height;  // written at the display size
    mem::w32(out + 0x1C, f.sps.crop && !cropped_already);
    if (f.sps.crop && !cropped_already) {
        mem::w32(out + 0x20, f.sps.crop_top);
        mem::w32(out + 0x24, f.sps.crop_bottom);
        mem::w32(out + 0x28, f.sps.crop_left);
        mem::w32(out + 0x2C, f.sps.crop_right);
    }
    mem::w32(out + 0x44, f.image);
    guest_call(c, d->output_fn, {cb});
}

void log_stats(Decoder* d, const char* when) {
    if (!d->frames) return;
    LOG("h264: %s: %llu frames decoded (%llu failed), %llu shown, %.2f ms a frame in the decoder", when,
        (unsigned long long)d->frames, (unsigned long long)d->failed, (unsigned long long)d->output,
        ticks_ms(d->decode_ticks) / (double)d->frames);
    d->frames = d->failed = d->output = d->decode_ticks = 0;
}
}  // namespace

// H264DECMemoryRequirement(profile, level, maxWidth, maxHeight, uint32* size)
HLE_EXPORT(h264, H264DECMemoryRequirement) {
    if (!ARG(4)) { RET(kInvalidParam); return; }
    mem::w32(ARG(4), kWorkMemorySize);
    RET(kOk);
}

// H264DECCheckMemSegmentation(mem, size): 0 = usable
HLE_EXPORT(h264, H264DECCheckMemSegmentation) { RET(0); }

// H264DECGetImageSize(stream, size, offset, uint32* width, uint32* height): size from the first SPS at or
// after `offset`
HLE_EXPORT(h264, H264DECGetImageSize) {
    const uint32_t stream = ARG(0), w_out = ARG(3), h_out = ARG(4);
    const int32_t size = (int32_t)ARG(1), offset = (int32_t)ARG(2);
    if (!stream || size < 4 || offset < 0 || !w_out || !h_out || offset + 4 >= size) { RET(kInvalidParam); return; }
    const uint8_t* p = mem::host(stream);
    for (int32_t i = offset; i + 4 < size; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1 || (p[i + 3] & 0x1F) != 7) continue;
        Sps s;
        if (!parse_sps(p + i + 4, (size_t)(size - i - 4), s)) break;
        uint32_t w, h;
        image_size(s, w, h);
        mem::w32(w_out, w);
        mem::w32(h_out, h);
        RET(kOk);
        return;
    }
    LOG("h264: H264DECGetImageSize found no usable sequence parameter set in %d bytes", size);
    RET(kBadStream);
}

// H264DECInitParam(size, mem)
HLE_EXPORT(h264, H264DECInitParam) {
    const uint32_t work = ARG(1);
    if (!work) { RET(kInvalidParam); return; }
    memset(mem::host(work), 0, std::min<uint32_t>(ARG(0), 0x100));
    std::lock_guard<std::mutex> l(g_lock);
    g_decoders[work] = std::make_unique<Decoder>();
    RET(kOk);
}

HLE_EXPORT(h264, H264DECSetParam_FPTR_OUTPUT) {
    Decoder* d = find(ARG(0));
    if (!d) { RET(kInvalidParam); return; }
    d->output_fn = ARG(1);
    RET(kOk);
}

HLE_EXPORT(h264, H264DECSetParam_OUTPUT_PER_FRAME) {
    Decoder* d = find(ARG(0));
    if (!d) { RET(kInvalidParam); return; }
    d->per_frame = ARG(1) != 0;
    RET(kOk);
}

// H264DECSetParam_USER_MEMORY(mem, void** user): the value handed back in the output callback
HLE_EXPORT(h264, H264DECSetParam_USER_MEMORY) {
    Decoder* d = find(ARG(0));
    if (!d || !ARG(1)) { RET(kInvalidParam); return; }
    d->user = mem::r32(ARG(1));
    RET(kOk);
}

HLE_EXPORT(h264, H264DECOpen) {
    Decoder* d = find(ARG(0));
    if (!d) { RET(kInvalidParam); return; }
    d->opened = true;
    LOG("h264: decoder opened (work memory %08x, output callback %08x, %s)", ARG(0), d->output_fn,
        d->per_frame ? "a frame per call" : "buffered");
    RET(kOk);
}

HLE_EXPORT(h264, H264DECClose) {
    std::unique_ptr<Decoder> d;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_decoders.find(ARG(0));
        if (it != g_decoders.end()) {
            d = std::move(it->second);
            g_decoders.erase(it);
        }
    }
    if (d) {
        log_stats(d.get(), "closed");
        LOG("h264: decoder closed (work memory %08x)", ARG(0));
    }
    RET(kOk);
}

// H264DECBegin(mem): start of a stream; a later Begin (seek, loop) decodes again from the next IDR picture
HLE_EXPORT(h264, H264DECBegin) {
    Decoder* d = find(ARG(0));
    if (d) {
        if (d->first_begin) d->first_begin = false;
        else d->recovering = true;
        d->sequence++;
    }
    RET(kOk);
}

// H264DECEnd(mem): hands out every frame still held
HLE_EXPORT(h264, H264DECEnd) {
    const uint32_t work = ARG(0);
    Decoder* d = find(work);
    if (d) {
        while (!d->held.empty()) {
            output_frame(c, d);
            if (!(d = find(work))) break;  // closed from the callback
        }
        if (d) log_stats(d, "end of stream");
    }
    RET(kOk);
}

// H264DECSetBitstream(mem, data, length, double timestamp)
HLE_EXPORT(h264, H264DECSetBitstream) {
    Decoder* d = find(ARG(0));
    if (!d) { RET(kInvalidParam); return; }
    d->bs_ptr = ARG(1);
    d->bs_len = ARG(2);
    d->bs_time = ARGF(0);
    RET(kOk);
}

// H264DECExecute(mem, frameBuffer): decodes the access unit set by H264DECSetBitstream into frameBuffer.
// Returns the status with bit 0x80 set when the frame buffer now holds a picture (the game keeps it out of
// its pool until the picture has been shown); without the bit the game takes the buffer back at once.
HLE_EXPORT(h264, H264DECExecute) {
    const uint32_t work = ARG(0), image = ARG(1);
    Decoder* d = find(work);
    if (!d || !d->bs_ptr || !d->bs_len) { RET(0); return; }
    split_annexb(mem::host(d->bs_ptr), d->bs_len, d->nals);
    const Nal* first_slice = nullptr;
    d->sample.clear();
    for (const Nal& n : d->nals) {
        const uint8_t t = n.type();
        if (t == 7) {
            if (d->sps_nal.size() != n.len || memcmp(d->sps_nal.data(), n.data, n.len)) {
                Sps s;
                if (!parse_sps(n.data + 1, n.len - 1, s)) continue;
                d->sps = s;
                d->sps_nal.assign(n.data, n.data + n.len);
                d->pps_nals.clear();
                d->params_changed = true;
            }
        } else if (t == 8) {
            bool known = false;
            for (auto& p : d->pps_nals) known |= p.size() == n.len && !memcmp(p.data(), n.data, n.len);
            if (!known) {
                d->pps_nals.emplace_back(n.data, n.data + n.len);
                d->params_changed = true;
            }
        } else if (t == 1 || t == 5 || t == 6) {
            if (t != 6 && !first_slice) first_slice = &n;
            const uint32_t len = (uint32_t)n.len;
            const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
            d->sample.insert(d->sample.end(), be, be + 4);
            d->sample.insert(d->sample.end(), n.data, n.data + n.len);
        }
    }
    if (!first_slice) { RET(100); return; }  // parameter sets only: nothing written to the frame buffer
    if (d->recovering) {
        if (first_slice->type() != 5) { RET(0x400); return; }
        d->recovering = false;
    }
    if (!d->sps.valid || !image) { RET(0x400); return; }

    Frame f;
    f.image = image;
    f.timestamp = d->bs_time;
    f.sps = d->sps;
    image_size(d->sps, f.width, f.height);
    f.pitch = (f.width + 0xFF) & ~0xFFu;
    f.order = picture_order(d, *first_slice);

    const uint64_t t0 = mach_absolute_time();
    bool done = false;
    OSStatus st = -1;
    for (int attempt = 0; attempt < 2 && !done; attempt++) {
        if (!prepare_session(d)) break;
        st = decode_sample(d, &f, done);
        // the system takes the session away while the app is in the background: make a new one (the picture
        // comes back at the next IDR)
        if (st == kVTInvalidSessionErr) d->drop_session();
        else break;
    }
    d->decode_ticks += mach_absolute_time() - t0;
    d->frames++;
    if (!done) {
        if (d->failed++ < 5) LOG("h264: a frame failed to decode (%d); its buffer keeps the previous contents", (int)st);
    }
    d->held.push_back(f);
    const uint32_t hold = d->per_frame ? 0 : kBufferedFrames;
    while (d->held.size() > hold) {
        output_frame(c, d);
        if (!(d = find(work))) break;  // closed from the callback
    }
    RET(0x80 | 100);
}
