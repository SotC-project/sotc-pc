// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Software implementation of the NGS2 voice graph: sampler voices decode waveform blocks,
// are resampled to the system rate and mixed through patched submix voices into the mastering
// voice, whose bus is written to the render buffers.

#include <algorithm>
#include <atomic>
#include <process.h>
#include <string>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <fmt/format.h>
#include <libatrac9.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"
#include "core/libraries/ngs2/ngs2_synth.h"

/// Sample frames written to the audio dump so far; the footage capture notes it at its first
/// frame so sound and picture can be joined exactly.
std::atomic<u64> g_sotc_audio_frames{0};

namespace Libraries::Ngs2::Synth {

namespace {

constexpr u32 MaxChannels = ORBIS_NGS2_MAX_VOICE_CHANNELS;
constexpr u32 MaxGrain = 1024;
constexpr u32 SystemRate = 48000;

enum RackId : u32 {
    RackSampler = 0x1000,
    RackSubmixer = 0x2000,
    RackMastering = 0x3000,
};

enum ParamId : u32 {
    VoiceMatrixLevels = 0x1,
    VoicePortVolume = 0x2,
    VoicePortMatrix = 0x3,
    VoicePortDelay = 0x4,
    VoicePatch = 0x5,
    VoiceKickEvent = 0x6,
    VoiceSetCallback = 0x7,

    SamplerSetup = 0x10000000,
    SamplerWaveformBlocks = 0x10000001,
    SamplerWaveformAddress = 0x10000002,
    SamplerWaveformFrameOffset = 0x10000003,
    SamplerExitLoop = 0x10000004,
    SamplerPitch = 0x10000005,
    SamplerEnvelope = 0x10000006,

    SubmixerSetup = 0x20000000,
    SubmixerUserFx = 0x20000004,

    MasteringSetup = 0x30000000,
    MasteringOutput = 0x30000005,
};

enum Event : u32 { Play = 0, Stop = 1, StopImm = 2, Kill = 3, Pause = 4, Resume = 5 };

enum StateFlag : u32 {
    InUse = 0x1,
    Playing = 0x2,
    Paused = 0x4,
    Stopped = 0x8,
    Error = 0x10,
    Empty = 0x20,
};

enum WaveformType : u32 {
    PcmI8 = 0x1,
    PcmU8 = 0x2,
    PcmI16L = 0x10,
    PcmI16B = 0x11,
    PcmI24L = 0x12,
    PcmI24B = 0x13,
    PcmI32L = 0x14,
    PcmI32B = 0x15,
    PcmF32L = 0x18,
    PcmF32B = 0x19,
    Vag = 0x1c,
    Atrac9 = 0x40,
};

constexpr u32 RepeatInfinite = 0xffffffff;

struct Voice;

struct Port {
    s32 matrix_id = -1;
    float volume = 1.0f;
    // Volume the last grain ended at; a change is spread over the next grain instead of
    // jumping, which is heard as a click. Negative: nothing was mixed yet.
    float applied = -1.0f;
    Voice* dest = nullptr;
};

struct Block {
    const u8* data;
    OrbisNgs2WaveformBlock info;
};

struct Rack;

struct Voice {
    Rack* rack{};
    u32 state{};
    std::vector<Port> ports;
    std::vector<std::vector<float>> matrices;

    // Bus voices (submixer, mastering, effects): planar input accumulated by upstream voices.
    u32 channels = 2;
    u32 output_id{};
    OrbisNgs2UserFxProcessHandler fx_handler{};
    uintptr_t fx_data[3]{};
    std::vector<float> bus;
    bool bus_used{};
    s32 depth = -1;

    // Sampler voices.
    OrbisNgs2WaveformFormat format{};
    std::deque<Block> blocks;
    std::vector<float> pcm; // interleaved decode of the current part of the front block
    bool block_open{};      // front block has been started
    u32 block_off{};        // bytes of the front block consumed by the decoder
    u64 block_skip{};       // samples still to drop at the start of the front block
    u64 block_left{};       // samples the front block may still produce
    u32 at9_frame{};        // frame index inside the current ATRAC9 superframe
    s32 at9_super_left{};   // bytes of the current superframe not yet consumed
    size_t pcm_pos{};       // frame cursor into pcm
    double phase{};
    float pitch = 1.0f;
    float last[MaxChannels]{};
    float prev[MaxChannels]{}; // frame before `last`, for interpolation

    // Envelope: the forward points run from Play, the release points from Stop. The voice
    // ends when its release has run out.
    std::vector<OrbisNgs2EnvelopePoint> env_forward;
    std::vector<OrbisNgs2EnvelopePoint> env_release;
    std::vector<OrbisNgs2EnvelopePoint> env_points; // the list being played
    u32 env_point{};
    u32 env_pos{};
    float env_from = 1.0f;
    float env_height = 1.0f;
    bool env_running{};
    bool releasing{}; // fading out; the voice stops when the envelope ends
    bool tail{};      // already reported as stopped, still fading out for a few milliseconds
    void* at9{};
    u64 decoded_samples{};
    u64 decoded_bytes{};
    uintptr_t user_data{};
    const void* waveform_data{};

    ~Voice() {
        if (at9) {
            Atrac9ReleaseHandle(at9);
        }
    }
};

struct Rack {
    u32 id{};
    std::vector<std::unique_ptr<Voice>> voices;
};

std::recursive_mutex g_mutex;

// Temporary diagnostics for bringing the signal path up.
struct Diag {
    float sampler_peak, sampler_port_volume, matrix_peak, bus_peak, bus_peak_pre, send_peak;
    u64 sampler_ports, pcm_frames, blocks, bus_voices_used, bus_ports, matrices, ports_with_matrix;
    u32 renders;
};
Diag g_diag{};
std::vector<std::unique_ptr<Rack>> g_racks;

// Resolves a guest handle to a voice. Handles are our own pointers, but they come back from the
// game, so only accept ones we handed out; a rack handle stands for its first voice.
Voice* FindVoice(OrbisNgs2Handle handle) {
    for (auto& rack : g_racks) {
        if (reinterpret_cast<OrbisNgs2Handle>(rack.get()) == handle) {
            return rack->voices.empty() ? nullptr : rack->voices.front().get();
        }
        for (auto& voice : rack->voices) {
            if (reinterpret_cast<OrbisNgs2Handle>(voice.get()) == handle) {
                return voice.get();
            }
        }
    }
    return nullptr;
}

bool IsSampler(const Voice& v) {
    return v.rack->id == RackSampler;
}

void ResetSampler(Voice& v) {
    v.env_running = false;
    v.releasing = false;
    v.tail = false;
    v.env_height = 1.0f;
    v.blocks.clear();
    v.pcm.clear();
    v.pcm_pos = 0;
    v.block_open = false;
    v.phase = 0;
    v.decoded_samples = 0;
    v.decoded_bytes = 0;
    v.waveform_data = nullptr;
    std::fill(std::begin(v.last), std::end(v.last), 0.0f);
    if (v.at9) {
        Atrac9ReleaseHandle(v.at9);
        v.at9 = nullptr;
    }
}

// PS-ADPCM (VAG): 16-byte frames of 28 samples, mono per stream.
void DecodeVag(Voice& v, const Block& b, std::vector<float>& out) {
    static constexpr float Coef[5][2] = {
        {0.0f, 0.0f},
        {60.0f / 64, 0.0f},
        {115.0f / 64, -52.0f / 64},
        {98.0f / 64, -55.0f / 64},
        {122.0f / 64, -60.0f / 64},
    };
    float h1 = 0, h2 = 0;
    for (u32 off = 0; off + 16 <= b.info.dataSize; off += 16) {
        const u8* f = b.data + b.info.dataOffset + off;
        const u32 shift = f[0] & 0xf;
        const u32 pred = std::min<u32>(f[0] >> 4, 4);
        for (u32 i = 0; i < 28; ++i) {
            s32 nib = (f[2 + i / 2] >> ((i & 1) * 4)) & 0xf;
            nib = (nib << 28) >> 28;
            const float s = float((nib << 12) >> shift) + h1 * Coef[pred][0] + h2 * Coef[pred][1];
            h2 = h1;
            h1 = s;
            out.push_back(std::clamp(s, -32768.0f, 32767.0f) / 32768.0f);
        }
    }
}

// Decodes up to `max_frames` ATRAC9 frames from the current position in the block.
void DecodeAtrac9(Voice& v, const Block& b, std::vector<float>& out, u32 max_frames) {
    if (!v.at9) {
        v.at9 = Atrac9GetHandle();
        // The config word holds the four ATRAC9 config bytes most significant first.
        const u32 word = v.format.configData;
        u8 config[4] = {u8(word >> 24), u8(word >> 16), u8(word >> 8), u8(word)};
        if (Atrac9InitDecoder(v.at9, config) != 0) {
            LOG_ERROR(Lib_Ngs2, "ATRAC9 init failed, config {:#x}", v.format.configData);
            Atrac9ReleaseHandle(v.at9);
            v.at9 = nullptr;
            v.block_off = b.info.dataSize;
            return;
        }
    }
    Atrac9CodecInfo info{};
    Atrac9GetCodecInfo(v.at9, &info);
    const size_t frame_floats = size_t(info.frameSamples) * info.channels;
    if (v.block_off == 0) {
        v.at9_frame = 0;
        v.at9_super_left = info.superframeSize;
    }
    // One call decodes one frame. The decoder only reports consumed bytes when it finishes a
    // superframe, so zero bytes used is normal for the frames in between.
    for (u32 frame = 0; frame < max_frames && v.block_off < b.info.dataSize; ++frame) {
        out.resize(out.size() + frame_floats);
        int used = 0;
        const int ret = Atrac9DecodeF32(v.at9, b.data + b.info.dataOffset + v.block_off,
                                        int(b.info.dataSize - v.block_off),
                                        out.data() + out.size() - frame_floats, &used, 0);
        if (ret != 0 || used < 0) {
            LOG_ERROR(Lib_Ngs2, "ATRAC9 decode error {:#x} at byte {} of {}", u32(ret), v.block_off,
                      b.info.dataSize);
            out.resize(out.size() - frame_floats);
            v.block_off = b.info.dataSize; // give up on the rest of this block
            return;
        }
        v.block_off += u32(used);
        // Frames are variable length; the superframe is padded to a fixed size.
        v.at9_super_left -= std::min<s32>(v.at9_super_left, used);
        if (++v.at9_frame == u32(std::max(info.framesInSuperframe, 1))) {
            v.block_off += u32(v.at9_super_left);
            v.at9_frame = 0;
            v.at9_super_left = info.superframeSize;
        }
    }
}

template <typename T, typename F>
void DecodePcm(const Block& b, std::vector<float>& out, F&& conv) {
    const u8* src = b.data + b.info.dataOffset;
    const u32 count = b.info.dataSize / sizeof(T);
    out.reserve(out.size() + count);
    for (u32 i = 0; i < count; ++i) {
        T value;
        std::memcpy(&value, src + i * sizeof(T), sizeof(T));
        out.push_back(conv(value));
    }
}

// Decodes the next part of the front block into v.pcm. Returns false once the block is used up.
// Streams are decoded as they play: the game keeps loading a chunk while its start is playing.
bool DecodeMore(Voice& v) {
    const Block& b = v.blocks.front();
    const u32 ch = std::clamp<u32>(v.format.numChannels, 1, MaxChannels);
    if (!v.block_open) {
        v.block_open = true;
        v.block_off = 0;
        v.block_skip = b.info.numSkipSamples;
        v.block_left = b.info.numSamples ? b.info.numSamples : ~0ull;
        v.waveform_data = b.data + b.info.dataOffset;
        v.user_data = b.info.userData;
    }
    if (v.block_off >= b.info.dataSize || v.block_left == 0) {
        return false;
    }
    v.pcm.clear();
    v.pcm_pos = 0;
    switch (v.format.waveformType) {
    case PcmI16L:
        DecodePcm<s16>(b, v.pcm, [](s16 s) { return s / 32768.0f; });
        break;
    case PcmI16B:
        DecodePcm<u16>(b, v.pcm, [](u16 s) { return s16((s >> 8) | (s << 8)) / 32768.0f; });
        break;
    case PcmI8:
        DecodePcm<s8>(b, v.pcm, [](s8 s) { return s / 128.0f; });
        break;
    case PcmU8:
        DecodePcm<u8>(b, v.pcm, [](u8 s) { return (s - 128) / 128.0f; });
        break;
    case PcmI32L:
        DecodePcm<s32>(b, v.pcm, [](s32 s) { return s / 2147483648.0f; });
        break;
    case PcmF32L:
        DecodePcm<float>(b, v.pcm, [](float s) { return s; });
        break;
    case Vag:
        DecodeVag(v, b, v.pcm);
        break;
    case Atrac9:
        DecodeAtrac9(v, b, v.pcm, 16);
        break;
    default:
        LOG_ERROR(Lib_Ngs2, "Unsupported waveform type {:#x}", v.format.waveformType);
        break;
    }
    if (v.format.waveformType != Atrac9) {
        v.block_off = b.info.dataSize; // the other formats are decoded in one step
    }
    // Apply the block's leading skip and its sample count.
    const u64 skip = std::min<u64>(v.block_skip, v.pcm.size() / ch);
    v.pcm.erase(v.pcm.begin(), v.pcm.begin() + skip * ch);
    v.block_skip -= skip;
    if (v.pcm.size() / ch > v.block_left) {
        v.pcm.resize(v.block_left * ch);
    }
    v.block_left -= v.pcm.size() / ch;
    return true;
}

// Fetches the next source frame, advancing through blocks. Returns false when out of data.
bool NextFrame(Voice& v, float* frame, u32 ch) {
    while (v.pcm_pos * ch >= v.pcm.size()) {
        if (v.blocks.empty()) {
            return false;
        }
        if (DecodeMore(v)) {
            continue;
        }
        // The front block is used up.
        Block& b = v.blocks.front();
        v.decoded_bytes += b.info.dataSize;
        v.block_open = false;
        v.pcm.clear();
        v.pcm_pos = 0;
        if (b.info.numRepeats == RepeatInfinite) {
            continue;
        }
        if (b.info.numRepeats > 0) {
            --b.info.numRepeats;
            continue;
        }
        v.blocks.pop_front();
    }
    std::memcpy(frame, v.pcm.data() + v.pcm_pos * ch, ch * sizeof(float));
    ++v.pcm_pos;
    ++v.decoded_samples;
    return true;
}

void StartEnvelope(Voice& v, const std::vector<OrbisNgs2EnvelopePoint>& points, float from) {
    v.env_points = points;
    v.env_point = 0;
    v.env_pos = 0;
    v.env_from = from;
    v.env_height = from;
    v.env_running = !points.empty();
}

/// Fades the voice out and ends it: over its release points, or over a few milliseconds when
/// it has none or is cut off, since a wave chopped mid-swing is heard as a click.
void StartRelease(Voice& v, bool use_release_points) {
    static const std::vector<OrbisNgs2EnvelopePoint> Declick{{0, 240, 0.0f}};
    StartEnvelope(v, use_release_points && !v.env_release.empty() ? v.env_release : Declick,
                  v.env_height);
    v.releasing = true;
}

/// Envelope height for the next output frame.
float EnvelopeStep(Voice& v) {
    if (!v.env_running) {
        return v.env_height;
    }
    const OrbisNgs2EnvelopePoint& point = v.env_points[v.env_point];
    const u32 duration = std::max(point.duration, 1u);
    ++v.env_pos;
    v.env_height = v.env_from + (point.height - v.env_from) * (float(v.env_pos) / duration);
    if (v.env_pos >= duration) {
        v.env_from = v.env_height = point.height;
        v.env_pos = 0;
        if (++v.env_point >= v.env_points.size()) {
            v.env_running = false;
        }
    }
    return v.env_height;
}

// Renders a sampler voice into planar `out` (ch * frames). Returns the channel count.
u32 RenderSampler(Voice& v, float* out, u32 frames) {
    const u32 ch = std::clamp<u32>(v.format.numChannels, 1, MaxChannels);
    const double step =
        double(v.pitch) * (v.format.sampleRate ? v.format.sampleRate : SystemRate) / SystemRate;
    float next[MaxChannels];
    for (u32 n = 0; n < frames; ++n) {
        // Linear interpolation between the two source frames around the play position. Holding
        // the last frame instead turned slowed-down sounds (stomps, falls) into a staircase,
        // which is heard as a harsh crunch.
        while (v.phase >= 1.0) {
            if (!NextFrame(v, next, ch)) {
                {
                    static int ended = 0;
                    if (ended++ < 60) {
                        LOG_ERROR(Lib_Ngs2, "voice {:x} out of data after {} samples, {} bytes",
                                  reinterpret_cast<uintptr_t>(&v) & 0xfffff, v.decoded_samples,
                                  v.decoded_bytes);
                    }
                }
                // Out of data: the voice finishes and frees itself.
                std::fill(std::begin(v.last), std::end(v.last), 0.0f);
                std::fill(std::begin(v.prev), std::end(v.prev), 0.0f);
                v.state = 0;
                for (u32 c = 0; c < ch; ++c) {
                    std::fill(out + c * frames + n, out + (c + 1) * frames, 0.0f);
                }
                v.phase = 1.0;
                return ch;
            }
            std::memcpy(v.prev, v.last, ch * sizeof(float));
            std::memcpy(v.last, next, ch * sizeof(float));
            v.phase -= 1.0;
        }
        const float t = static_cast<float>(v.phase);
        const float gain = EnvelopeStep(v);
        for (u32 c = 0; c < ch; ++c) {
            out[c * frames + n] = (v.prev[c] + (v.last[c] - v.prev[c]) * t) * gain;
        }
        v.phase += step;
        if (v.releasing && !v.env_running) {
            // The release has run out: the voice is over.
            for (u32 c = 0; c < ch; ++c) {
                std::fill(out + c * frames + n + 1, out + (c + 1) * frames, 0.0f);
            }
            v.state = 0;
            ResetSampler(v);
            return ch;
        }
    }
    return ch;
}

// Mixes planar `src` through each patched port of `v` into the destination buses.
void Route(Voice& v, const float* src, u32 src_ch, u32 frames) {
    for (Port& port : v.ports) {
        Voice* dest = port.dest;
        const float start_volume = port.applied < 0.0f ? port.volume : port.applied;
        port.applied = port.volume;
        if (!dest || (port.volume == 0.0f && start_volume == 0.0f)) {
            continue;
        }
        const float volume_step = (port.volume - start_volume) / float(frames);
        if (dest->bus.size() != size_t(MaxChannels) * MaxGrain) {
            dest->bus.assign(size_t(MaxChannels) * MaxGrain, 0.0f);
        }
        dest->bus_used = true;
        const u32 dest_ch = std::clamp<u32>(dest->channels, 1, MaxChannels);
        const std::vector<float>* matrix = nullptr;
        static const bool ignore_matrix = std::getenv("SOTC_AUDIO_NO_MATRIX") != nullptr;
        if (!ignore_matrix && port.matrix_id >= 0 && size_t(port.matrix_id) < v.matrices.size() &&
            !v.matrices[port.matrix_id].empty()) {
            matrix = &v.matrices[port.matrix_id];
        }
        for (u32 in = 0; in < src_ch; ++in) {
            for (u32 o = 0; o < dest_ch; ++o) {
                float level;
                if (matrix) {
                    // Levels are stored one row per output channel: level[out * inputs + in].
                    const u32 outs = u32(matrix->size()) / src_ch;
                    level = o < outs ? (*matrix)[o * src_ch + in] : 0.0f;
                } else if (src_ch == 1) {
                    level = o < 2 ? 0.7071f : 0.0f;
                } else {
                    level = in == o ? 1.0f : 0.0f;
                }
                if (level == 0.0f) {
                    continue;
                }
                const float* s = src + in * frames;
                float* d = dest->bus.data() + o * MaxGrain;
                if (volume_step == 0.0f) {
                    level *= port.volume;
                    for (u32 n = 0; n < frames; ++n) {
                        d[n] += s[n] * level;
                    }
                } else {
                    for (u32 n = 0; n < frames; ++n) {
                        d[n] += s[n] * level * (start_volume + volume_step * float(n + 1));
                    }
                }
            }
        }
    }
}

// Longest path from a voice to the end of the graph; upstream voices have a larger depth.
s32 Depth(Voice& v, u32 guard = 0) {
    if (v.depth >= 0 || guard > 32) {
        return std::max(v.depth, 0);
    }
    s32 depth = 0;
    for (const Port& port : v.ports) {
        if (port.dest) {
            depth = std::max(depth, Depth(*port.dest, guard + 1) + 1);
        }
    }
    return v.depth = depth;
}

void WriteOutput(const Voice& v, const OrbisNgs2RenderBufferInfo& info, u32 frames) {
    const u32 ch = info.numChannels;
    const bool is_float = info.waveformType == PcmF32L;
    for (u32 n = 0; n < frames; ++n) {
        for (u32 c = 0; c < ch; ++c) {
            // Peaks reach about 1.3 in busy scenes; bend them over above 0.8 instead of
            // clipping them flat, which is audible as crackle.
            float s = c < MaxChannels && v.bus_used ? v.bus[c * MaxGrain + n] : 0.0f;
            if (const float mag = std::abs(s); mag > 0.8f) {
                s = std::copysign(0.8f + 0.2f * std::tanh((mag - 0.8f) * 5.0f), s);
            }
            if (is_float) {
                static_cast<float*>(info.buffer)[n * ch + c] = s;
            } else {
                static_cast<s16*>(info.buffer)[n * ch + c] = s16(s * 32767.0f);
            }
        }
    }
}

template <typename T>
const T& As(const OrbisNgs2VoiceParamHeader* p) {
    return *reinterpret_cast<const T*>(p);
}

Port& GetPort(Voice& v, u32 index) {
    if (index >= v.ports.size()) {
        v.ports.resize(std::min<u32>(index, 63) + 1);
    }
    return v.ports[std::min<u32>(index, 63)];
}

void ApplyParam(Voice& v, const OrbisNgs2VoiceParamHeader* p) {
    switch (p->id) {
    case VoiceMatrixLevels: {
        const auto& m = As<OrbisNgs2VoiceMatrixLevelsParam>(p);
        if (m.matrixId < 64 && m.aLevel && m.numLevels <= ORBIS_NGS2_MAX_MATRIX_LEVELS) {
            if (m.matrixId >= v.matrices.size()) {
                v.matrices.resize(m.matrixId + 1);
            }
            v.matrices[m.matrixId].assign(m.aLevel, m.aLevel + m.numLevels);
        }
        break;
    }
    case VoicePortMatrix: {
        const auto& m = As<OrbisNgs2VoicePortMatrixParam>(p);
        GetPort(v, m.port).matrix_id = m.matrixId;
        break;
    }
    case VoicePortVolume: {
        const auto& m = As<OrbisNgs2VoicePortVolumeParam>(p);
        GetPort(v, m.port).volume = m.level;
        {
            static u32 vol_zero = 0, vol_nonzero = 0, traced = 0;
            (m.level == 0.0f ? vol_zero : vol_nonzero)++;
            if (IsSampler(v) && traced < 60 && m.port == 0) {
                ++traced;
                LOG_ERROR(Lib_Ngs2, "voice {:x} port0 volume {:.4f} (calls zero {} nonzero {})",
                          reinterpret_cast<uintptr_t>(&v) & 0xfffff, m.level, vol_zero, vol_nonzero);
            }
        }
        break;
    }
    case VoicePatch: {
        const auto& m = As<OrbisNgs2VoicePatchParam>(p);
        GetPort(v, m.port).dest = FindVoice(m.destHandle);
        break;
    }
    case VoiceKickEvent:
        {
            static int events = 0;
            if (IsSampler(v) && events++ < 400) {
                LOG_ERROR(Lib_Ngs2, "voice {:x} event {} state {:#x} decoded {} render {}",
                          reinterpret_cast<uintptr_t>(&v) & 0xfffff,
                          As<OrbisNgs2VoiceEventParam>(p).eventId, v.state, v.decoded_samples,
                          g_diag.renders);
            }
        }
        switch (As<OrbisNgs2VoiceEventParam>(p).eventId) {
        case Play:
            v.state = (v.state | InUse | Playing) & ~(Stopped | Paused | Empty);
            if (IsSampler(v)) {
                v.releasing = false;
                v.tail = false;
                v.env_height = 1.0f;
                v.env_running = false;
                if (!v.env_forward.empty()) {
                    StartEnvelope(v, v.env_forward, 0.0f);
                }
            }
            break;
        case Stop:
            // Stop lets the sound go through its release; the voice stays in use meanwhile.
            if (IsSampler(v) && (v.state & Playing) && !(v.state & Paused)) {
                if (!v.releasing) {
                    StartRelease(v, true);
                }
                break;
            }
            [[fallthrough]];
        case StopImm:
        case Kill:
            // The voice is free at once for the game; what was sounding fades out over a few
            // milliseconds instead of being chopped off.
            if (IsSampler(v) && (v.state & Playing) && !(v.state & Paused) && !v.tail) {
                StartRelease(v, false);
                v.tail = true;
                v.state = 0;
                break;
            }
            v.state = 0;
            if (IsSampler(v)) {
                ResetSampler(v);
            }
            break;
        case Pause:
            v.state |= Paused;
            break;
        case Resume:
            v.state &= ~Paused;
            break;
        }
        break;
    case SamplerSetup:
        ResetSampler(v);
        v.format = As<OrbisNgs2SamplerVoiceSetupParam>(p).format;
        {
            static int setups = 0;
            if (setups++ < 60) {
                LOG_ERROR(Lib_Ngs2, "voice {:x} setup type {:#x} ch {} rate {} flags {:#x}",
                          reinterpret_cast<uintptr_t>(&v) & 0xfffff, v.format.waveformType,
                          v.format.numChannels, v.format.sampleRate,
                          As<OrbisNgs2SamplerVoiceSetupParam>(p).flags);
            }
        }
        v.pitch = 1.0f;
        v.phase = 1.0;
        v.state = InUse;
        v.env_forward.clear();
        v.env_release.clear();
        for (Port& port : v.ports) {
            port.applied = -1.0f;
        }
        break;
    case SamplerWaveformBlocks: {
        const auto& m = As<OrbisNgs2SamplerVoiceWaveformBlocksParam>(p);
        {
            static int traced = 0;
            if (traced < 70) {
                ++traced;
                std::string text;
                for (u32 i = 0; m.aBlock && i < std::min<u32>(m.numBlocks, 4); ++i) {
                    const auto& blk = m.aBlock[i];
                    text += fmt::format("[off {:#x} size {:#x} rep {:#x} skip {} n {} ud {:#x}] ",
                                        blk.dataOffset, blk.dataSize, blk.numRepeats,
                                        blk.numSkipSamples, blk.numSamples, blk.userData);
                }
                LOG_ERROR(Lib_Ngs2, "voice {:x} blocks data {} flags {:#x} n {} queued {} state {:#x}: {}",
                          reinterpret_cast<uintptr_t>(&v) & 0xfffff, fmt::ptr(m.data), m.flags,
                          m.numBlocks, v.blocks.size(), v.state, text);
            }
        }
        for (u32 i = 0; m.data && m.aBlock && i < m.numBlocks; ++i) {
            v.blocks.push_back({static_cast<const u8*>(m.data), m.aBlock[i]});
        }
        v.state &= ~Empty;
        break;
    }
    case SamplerExitLoop:
        if (!v.blocks.empty()) {
            v.blocks.front().info.numRepeats = 0;
        }
        break;
    case SamplerPitch:
        v.pitch = As<OrbisNgs2SamplerVoicePitchParam>(p).ratio;
        break;
    case SamplerEnvelope: {
        const auto& m = As<OrbisNgs2SamplerVoiceEnvelopeParam>(p);
        v.env_forward.clear();
        v.env_release.clear();
        if (m.aPoint && m.numForwardPoints <= 64 && m.numReleasePoints <= 64) {
            v.env_forward.assign(m.aPoint, m.aPoint + m.numForwardPoints);
            v.env_release.assign(m.aPoint + m.numForwardPoints,
                                 m.aPoint + m.numForwardPoints + m.numReleasePoints);
        }
        static int traced = 0;
        if (traced++ < 20) {
            std::string text;
            for (const auto& point : v.env_forward) {
                text += fmt::format("[{} {} {:.2f}] ", point.curve, point.duration, point.height);
            }
            text += "| ";
            for (const auto& point : v.env_release) {
                text += fmt::format("[{} {} {:.2f}] ", point.curve, point.duration, point.height);
            }
            LOG_ERROR(Lib_Ngs2, "voice {:x} envelope (curve duration height): {}",
                      reinterpret_cast<uintptr_t>(&v) & 0xfffff, text);
        }
        break;
    }
    case SubmixerSetup:
        v.channels = As<OrbisNgs2SubmixerVoiceSetupParam>(p).numIoChannels;
        v.state = InUse;
        break;
    case SubmixerUserFx: {
        const auto& m = As<OrbisNgs2SubmixerVoiceUserFxParam>(p);
        v.fx_handler = m.handler;
        v.fx_data[0] = m.userData0;
        v.fx_data[1] = m.userData1;
        v.fx_data[2] = m.userData2;
        break;
    }
    case MasteringSetup:
        v.channels = As<OrbisNgs2MasteringVoiceSetupParam>(p).numInputChannels;
        v.state = InUse;
        break;
    case MasteringOutput:
        v.output_id = As<OrbisNgs2MasteringVoiceOutputParam>(p).outputId;
        break;
    default:
        if (IsSampler(v)) {
            // Sampler settings this synth does not act on; each kind is named once.
            static std::set<u32> named;
            if (named.insert(p->id).second) {
                LOG_ERROR(Lib_Ngs2, "sampler parameter {:#x} is not handled", p->id);
            }
        }
        // Setup of other bus racks (reverb, custom): first two fields are the channel counts.
        if ((p->id & 0xffff) == 0 && !IsSampler(v)) {
            v.channels = std::clamp<u32>(reinterpret_cast<const u32*>(p)[2], 1, MaxChannels);
            v.state = InUse;
        }
        break;
    }
}

} // namespace

s32 RackCreate(u32 rack_id, const OrbisNgs2RackOption* option, OrbisNgs2Handle* out_handle) {
    std::scoped_lock lock{g_mutex};
    auto rack = std::make_unique<Rack>();
    rack->id = rack_id;
    const u32 max_voices = option && option->maxVoices ? option->maxVoices : 1;
    for (u32 i = 0; i < max_voices; ++i) {
        auto& voice = rack->voices.emplace_back(std::make_unique<Voice>());
        voice->rack = rack.get();
        voice->channels = rack_id == RackSampler ? 1 : 2;
    }
    if (out_handle) {
        *out_handle = reinterpret_cast<OrbisNgs2Handle>(rack.get());
    }
    g_racks.push_back(std::move(rack));
    return ORBIS_OK;
}

s32 RackDestroy(OrbisNgs2Handle rack_handle) {
    std::scoped_lock lock{g_mutex};
    auto* rack = reinterpret_cast<Rack*>(rack_handle);
    for (auto& other : g_racks) {
        for (auto& voice : other->voices) {
            for (Port& port : voice->ports) {
                if (port.dest && port.dest->rack == rack) {
                    port.dest = nullptr;
                }
            }
        }
    }
    std::erase_if(g_racks, [&](const auto& r) { return r.get() == rack; });
    return ORBIS_OK;
}

s32 RackGetVoiceHandle(OrbisNgs2Handle rack_handle, u32 index, OrbisNgs2Handle* out_handle) {
    auto* rack = reinterpret_cast<Rack*>(rack_handle);
    if (!rack) {
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    if (index >= rack->voices.size() || !out_handle) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX;
    }
    *out_handle = reinterpret_cast<OrbisNgs2Handle>(rack->voices[index].get());
    return ORBIS_OK;
}

s32 VoiceControl(OrbisNgs2Handle voice_handle, const OrbisNgs2VoiceParamHeader* params) {
    std::scoped_lock lock{g_mutex};
    auto* voice = FindVoice(voice_handle);
    if (!voice) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    for (const auto* p = params; p;) {
        ApplyParam(*voice, p);
        if (p->next == 0) {
            break;
        }
        p = reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(reinterpret_cast<const u8*>(p) +
                                                                 p->next);
    }
    return ORBIS_OK;
}

s32 VoiceGetState(OrbisNgs2Handle voice_handle, OrbisNgs2VoiceState* out_state, size_t size) {
    std::scoped_lock lock{g_mutex};
    auto* voice = FindVoice(voice_handle);
    if (!voice) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    if (!out_state || size < sizeof(OrbisNgs2VoiceState)) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    std::memset(out_state, 0, size);
    out_state->stateFlags = voice->state;
    if (IsSampler(*voice) && size >= sizeof(OrbisNgs2SamplerVoiceState)) {
        auto* sampler = reinterpret_cast<OrbisNgs2SamplerVoiceState*>(out_state);
        sampler->envelopeHeight = voice->env_height;
        sampler->numDecodedSamples = voice->decoded_samples;
        // Streams watch this to know when to queue the next chunk, so it has to advance while
        // a block plays, not only when it ends.
        sampler->decodedDataSize = voice->decoded_bytes;
        if (voice->block_open && !voice->blocks.empty()) {
            const u64 size = voice->blocks.front().info.dataSize;
            const u64 total = voice->pcm.size();
            sampler->decodedDataSize +=
                voice->format.waveformType == Atrac9
                    ? voice->block_off
                    : (total ? size * std::min<u64>(voice->pcm_pos * voice->format.numChannels, total) / total : 0);
        }
        sampler->userData = voice->user_data;
        sampler->waveformData = voice->waveform_data;
    }
    return ORBIS_OK;
}

s32 VoiceGetStateFlags(OrbisNgs2Handle voice_handle, u32* out_flags) {
    std::scoped_lock lock{g_mutex};
    auto* voice = FindVoice(voice_handle);
    if (!voice || !out_flags) {
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    }
    *out_flags = voice->state;
    return ORBIS_OK;
}

s32 Render(const OrbisNgs2RenderBufferInfo* buffers, u32 num_buffers) {
    std::scoped_lock lock{g_mutex};
    // Debug switches while the synth is being brought up.
    static const bool mute = std::getenv("SOTC_NO_AUDIO") != nullptr;
    static const bool no_fx = std::getenv("SOTC_NO_FX") != nullptr;
    if (mute) {
        for (u32 i = 0; buffers && i < num_buffers; ++i) {
            if (buffers[i].buffer) {
                std::memset(buffers[i].buffer, 0, buffers[i].bufferSize);
            }
        }
        return ORBIS_OK;
    }
    u32 frames = 256;
    if (buffers && num_buffers && buffers[0].numChannels) {
        const u32 sample_size = buffers[0].waveformType == PcmF32L ? 4 : 2;
        frames = u32(buffers[0].bufferSize / (sample_size * buffers[0].numChannels));
    }
    frames = std::min(frames, MaxGrain);

    std::vector<Voice*> bus_voices;
    for (auto& rack : g_racks) {
        for (auto& voice : rack->voices) {
            voice->depth = -1;
            if (!IsSampler(*voice)) {
                if (voice->bus_used) {
                    std::fill(voice->bus.begin(), voice->bus.end(), 0.0f);
                    voice->bus_used = false;
                }
                bus_voices.push_back(voice.get());
            }
        }
    }

    // Guarded by g_mutex. Not thread_local: host TLS is not usable on guest threads.
    static std::vector<float> scratch(size_t(MaxChannels) * MaxGrain);
    for (auto& rack : g_racks) {
        if (rack->id != RackSampler) {
            continue;
        }
        for (auto& voice : rack->voices) {
            if (!voice->tail && (voice->state & (Playing | Paused)) != Playing) {
                continue;
            }
            const u32 ch = RenderSampler(*voice, scratch.data(), frames);
            for (u32 n = 0; n < ch * frames; ++n) {
                g_diag.sampler_peak = std::max(g_diag.sampler_peak, std::abs(scratch[n]));
            }
            for (const Port& port : voice->ports) {
                g_diag.sampler_ports += port.dest != nullptr;
                g_diag.sampler_port_volume = std::max(g_diag.sampler_port_volume, port.volume);
                if (port.matrix_id >= 0 && size_t(port.matrix_id) < voice->matrices.size()) {
                    for (float level : voice->matrices[port.matrix_id]) {
                        g_diag.matrix_peak = std::max(g_diag.matrix_peak, level);
                    }
                }
            }
            {
                // Loudest thing this voice sends anywhere: source peak x port volume x matrix.
                float src_peak = 0;
                for (u32 n = 0; n < ch * frames; ++n) {
                    src_peak = std::max(src_peak, std::abs(scratch[n]));
                }
                for (const Port& port : voice->ports) {
                    if (!port.dest || port.matrix_id < 0 ||
                        size_t(port.matrix_id) >= voice->matrices.size()) {
                        continue;
                    }
                    for (float level : voice->matrices[port.matrix_id]) {
                        g_diag.send_peak =
                            std::max(g_diag.send_peak, src_peak * port.volume * std::abs(level));
                    }
                }
            }
            g_diag.matrices += voice->matrices.size();
            for (const Port& port : voice->ports) {
                g_diag.ports_with_matrix += port.matrix_id >= 0;
            }
            g_diag.pcm_frames += voice->pcm.size();
            g_diag.blocks += voice->blocks.size();
            Route(*voice, scratch.data(), ch, frames);
        }
    }

    std::ranges::stable_sort(bus_voices,
                             [](Voice* a, Voice* b) { return Depth(*a) > Depth(*b); });
    for (Voice* voice : bus_voices) {
        if (!voice->bus_used) {
            continue;
        }
        if (voice->rack->id == RackMastering) {
            continue;
        }
        // Planar bus rows are MaxGrain apart; repack to `frames` stride for routing.
        const u32 ch = std::clamp<u32>(voice->channels, 1, MaxChannels);
        for (u32 c = 0; c < ch; ++c) {
            std::memcpy(scratch.data() + c * frames, voice->bus.data() + c * MaxGrain,
                        frames * sizeof(float));
        }
        for (u32 n = 0; n < ch * frames; ++n) {
            g_diag.bus_peak_pre = std::max(g_diag.bus_peak_pre, std::abs(scratch[n]));
        }
        if (voice->fx_handler && !no_fx) {
            // The game's own DSP (mixing, gain, compression) runs as a callback on the bus.
            float* channel_data[MaxChannels];
            for (u32 c = 0; c < MaxChannels; ++c) {
                channel_data[c] = scratch.data() + std::min(c, ch - 1) * frames;
            }
            OrbisNgs2UserFxProcessContext context{
                .aChannelData = channel_data,
                .userData0 = voice->fx_data[0],
                .userData1 = voice->fx_data[1],
                .userData2 = voice->fx_data[2],
                .flags = 0,
                .numChannels = ch,
                .numGrainSamples = frames,
                .sampleRate = SystemRate,
            };
            voice->fx_handler(&context);
        }
        ++g_diag.bus_voices_used;
        for (const Port& port : voice->ports) {
            g_diag.bus_ports += port.dest != nullptr;
        }
        for (u32 n = 0; n < ch * frames; ++n) {
            g_diag.bus_peak = std::max(g_diag.bus_peak, std::abs(scratch[n]));
        }
        Route(*voice, scratch.data(), ch, frames);
    }
    if (++g_diag.renders % 1000 == 0) {
        LOG_ERROR(Lib_Ngs2, "synth diag: sampler peak {:.3f} patched ports {} port vol {:.2f} "
                            "matrix peak {:.3f} pcm {} blocks {} | bus voices used {} patched {} "
                            "peak after fx {:.3f}",
                  g_diag.sampler_peak, g_diag.sampler_ports, g_diag.sampler_port_volume,
                  g_diag.matrix_peak, g_diag.pcm_frames, g_diag.blocks, g_diag.bus_voices_used,
                  g_diag.bus_ports, g_diag.bus_peak);
        LOG_ERROR(Lib_Ngs2, "synth diag2: matrices {} ports with matrix {} send peak {:.3f} bus "
                            "peak before fx {:.3f}",
                  g_diag.matrices, g_diag.ports_with_matrix, g_diag.send_peak, g_diag.bus_peak_pre);
        const u32 renders = g_diag.renders;
        g_diag = {};
        g_diag.renders = renders;
    }

    for (u32 i = 0; buffers && i < num_buffers; ++i) {
        if (!buffers[i].buffer) {
            continue;
        }
        const Voice* master = nullptr;
        for (Voice* voice : bus_voices) {
            if (voice->rack->id == RackMastering && voice->output_id == i) {
                master = voice;
                break;
            }
        }
        if (master) {
            WriteOutput(*master, buffers[i], frames);
            // Test aid: SOTC_AUDIODUMP=<file> appends the first two channels of every rendered grain
            // as raw float32 (pre-limiter values would hide what the listener gets, so this is post).
            static FILE* dump = [] {
                // %p in the name becomes the process number; <file>.ms holds the clock time of
                // the first grain, to line the sound up with captured frames.
                const char* env = std::getenv("SOTC_AUDIODUMP");
                if (!env) {
                    return static_cast<FILE*>(nullptr);
                }
                std::string path = env;
                if (const auto at = path.find("%p"); at != std::string::npos) {
                    path.replace(at, 2, std::to_string(_getpid()));
                }
                if (FILE* mark = std::fopen((path + ".ms").c_str(), "w")) {
                    std::fprintf(mark, "%lld\n",
                                 static_cast<long long>(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count()));
                    std::fclose(mark);
                }
                return std::fopen(path.c_str(), "wb");
            }();
            if (dump && i == 0 && buffers[i].numChannels >= 2 && buffers[i].waveformType == PcmF32L) {
                static bool announced = false;
                if (!announced) {
                    announced = true;
                    LOG_INFO(Lib_Ngs2, "Audio dump: first grain at {} ms, {} frames per grain",
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count(),
                             frames);
                }
                const float* out = static_cast<const float*>(buffers[i].buffer);
                for (u32 n = 0; n < frames; ++n) {
                    std::fwrite(out + n * buffers[i].numChannels, sizeof(float), 2, dump);
                }
                g_sotc_audio_frames += frames;
            }
            // Temporary: output level meter, to confirm the synth produces signal.
            static float peak[4]{};
            static u32 playing_max = 0, renders = 0;
            if (i < 4 && master->bus_used) {
                for (u32 c = 0; c < 2; ++c) {
                    for (u32 n = 0; n < frames; ++n) {
                        peak[i] = std::max(peak[i], std::abs(master->bus[c * MaxGrain + n]));
                    }
                }
            }
            if (i == 0) {
                u32 playing = 0;
                for (auto& rack : g_racks) {
                    if (rack->id == RackSampler) {
                        for (auto& voice : rack->voices) {
                            playing += (voice->state & Playing) != 0;
                        }
                    }
                }
                playing_max = std::max(playing_max, playing);
                // Wall-clock gap between renders: a grain lasts 5.3 ms, so a long gap is a
                // dropout the listener hears.
                static auto last_render = std::chrono::steady_clock::now();
                static double worst_gap_ms = 0;
                static u32 late_renders = 0;
                const auto render_now = std::chrono::steady_clock::now();
                const double gap_ms =
                    std::chrono::duration<double, std::milli>(render_now - last_render).count();
                last_render = render_now;
                worst_gap_ms = std::max(worst_gap_ms, gap_ms);
                late_renders += gap_ms > 40.0;
                if (++renders % 1000 == 0) {
                    LOG_ERROR(Lib_Ngs2, "audio gaps/1000 grains: worst {:.0f} ms, over 40 ms: {}",
                              worst_gap_ms, late_renders);
                    worst_gap_ms = 0;
                    late_renders = 0;
                    LOG_ERROR(Lib_Ngs2, "audio/1000 grains: peak out0 {:.3f} out1 {:.3f} out2 {:.3f}, "
                                        "max playing sampler voices {}",
                              peak[0], peak[1], peak[2], playing_max);
                    peak[0] = peak[1] = peak[2] = peak[3] = 0;
                    playing_max = 0;
                }
            }
        } else {
            std::memset(buffers[i].buffer, 0, buffers[i].bufferSize);
        }
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2::Synth
