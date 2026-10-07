// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Software implementation of the NGS2 voice graph: sampler voices decode waveform blocks,
// are resampled to the system rate and mixed through patched submix voices into the mastering
// voice, whose bus is written to the render buffers.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <libatrac9.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_submixer.h"
#include "core/libraries/ngs2/ngs2_synth.h"

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
    std::vector<float> pcm; // interleaved decode of the current block
    size_t pcm_pos{};       // frame cursor into pcm
    double phase{};
    float pitch = 1.0f;
    float last[MaxChannels]{};
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
    v.blocks.clear();
    v.pcm.clear();
    v.pcm_pos = 0;
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

void DecodeAtrac9(Voice& v, const Block& b, std::vector<float>& out) {
    if (!v.at9) {
        v.at9 = Atrac9GetHandle();
        // The config word holds the four ATRAC9 config bytes most significant first.
        const u32 word = v.format.configData;
        u8 config[4] = {u8(word >> 24), u8(word >> 16), u8(word >> 8), u8(word)};
        if (Atrac9InitDecoder(v.at9, config) != 0) {
            LOG_ERROR(Lib_Ngs2, "ATRAC9 init failed, config {:#x}", v.format.configData);
            Atrac9ReleaseHandle(v.at9);
            v.at9 = nullptr;
            return;
        }
    }
    Atrac9CodecInfo info{};
    Atrac9GetCodecInfo(v.at9, &info);
    const u32 frame_samples = info.frameSamples;
    const u8* src = b.data + b.info.dataOffset;
    s32 remaining = b.info.dataSize;
    while (remaining > 0) {
        out.resize(out.size() + frame_samples * info.channels);
        int used = 0;
        const int ret = Atrac9DecodeF32(v.at9, src, remaining,
                                        out.data() + out.size() - frame_samples * info.channels,
                                        &used, 0);
        if (ret != 0 || used <= 0) {
            out.resize(out.size() - frame_samples * info.channels);
            break;
        }
        src += used;
        remaining -= used;
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

// Decodes the block at the front of the queue into v.pcm.
void DecodeFront(Voice& v) {
    const Block& b = v.blocks.front();
    const u32 ch = std::clamp<u32>(v.format.numChannels, 1, MaxChannels);
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
        DecodeAtrac9(v, b, v.pcm);
        break;
    default:
        LOG_ERROR(Lib_Ngs2, "Unsupported waveform type {:#x}", v.format.waveformType);
        break;
    }
    // Trim to the samples the block declares.
    const size_t skip = size_t(b.info.numSkipSamples) * ch;
    if (skip && skip <= v.pcm.size()) {
        v.pcm.erase(v.pcm.begin(), v.pcm.begin() + skip);
    }
    if (b.info.numSamples && size_t(b.info.numSamples) * ch < v.pcm.size()) {
        v.pcm.resize(size_t(b.info.numSamples) * ch);
    }
    v.waveform_data = b.data + b.info.dataOffset;
    v.user_data = b.info.userData;
}

// Fetches the next source frame, advancing through blocks. Returns false when out of data.
bool NextFrame(Voice& v, float* frame, u32 ch) {
    while (v.pcm_pos * ch >= v.pcm.size()) {
        if (v.blocks.empty()) {
            return false;
        }
        if (!v.pcm.empty()) {
            // Finished the current block.
            Block& b = v.blocks.front();
            v.decoded_bytes += b.info.dataSize;
            if (b.info.numRepeats == RepeatInfinite) {
                v.pcm_pos = 0;
                continue;
            }
            if (b.info.numRepeats > 0) {
                --b.info.numRepeats;
                v.pcm_pos = 0;
                continue;
            }
            v.blocks.pop_front();
            v.pcm.clear();
            v.pcm_pos = 0;
            if (v.blocks.empty()) {
                return false;
            }
        }
        DecodeFront(v);
        if (v.pcm.empty()) {
            v.blocks.pop_front();
        }
    }
    std::memcpy(frame, v.pcm.data() + v.pcm_pos * ch, ch * sizeof(float));
    ++v.pcm_pos;
    ++v.decoded_samples;
    return true;
}

// Renders a sampler voice into planar `out` (ch * frames). Returns the channel count.
u32 RenderSampler(Voice& v, float* out, u32 frames) {
    const u32 ch = std::clamp<u32>(v.format.numChannels, 1, MaxChannels);
    const double step =
        double(v.pitch) * (v.format.sampleRate ? v.format.sampleRate : SystemRate) / SystemRate;
    float next[MaxChannels];
    for (u32 n = 0; n < frames; ++n) {
        // ponytail: zero-order hold resampling, switch to linear/sinc if aliasing is audible.
        while (v.phase >= 1.0) {
            if (!NextFrame(v, next, ch)) {
                // Out of data: the voice finishes and frees itself.
                std::fill(std::begin(v.last), std::end(v.last), 0.0f);
                v.state = 0;
                for (u32 c = 0; c < ch; ++c) {
                    std::fill(out + c * frames + n, out + (c + 1) * frames, 0.0f);
                }
                v.phase = 1.0;
                return ch;
            }
            std::memcpy(v.last, next, ch * sizeof(float));
            v.phase -= 1.0;
        }
        for (u32 c = 0; c < ch; ++c) {
            out[c * frames + n] = v.last[c];
        }
        v.phase += step;
    }
    return ch;
}

// Mixes planar `src` through each patched port of `v` into the destination buses.
void Route(Voice& v, const float* src, u32 src_ch, u32 frames) {
    for (const Port& port : v.ports) {
        Voice* dest = port.dest;
        if (!dest || port.volume == 0.0f) {
            continue;
        }
        if (dest->bus.size() != size_t(MaxChannels) * MaxGrain) {
            dest->bus.assign(size_t(MaxChannels) * MaxGrain, 0.0f);
        }
        dest->bus_used = true;
        const u32 dest_ch = std::clamp<u32>(dest->channels, 1, MaxChannels);
        const std::vector<float>* matrix = nullptr;
        if (port.matrix_id >= 0 && size_t(port.matrix_id) < v.matrices.size() &&
            !v.matrices[port.matrix_id].empty()) {
            matrix = &v.matrices[port.matrix_id];
        }
        for (u32 in = 0; in < src_ch; ++in) {
            for (u32 o = 0; o < dest_ch; ++o) {
                float level;
                if (matrix) {
                    const u32 outs = u32(matrix->size()) / src_ch;
                    level = o < outs ? (*matrix)[in * outs + o] : 0.0f;
                } else if (src_ch == 1) {
                    level = o < 2 ? 0.7071f : 0.0f;
                } else {
                    level = in == o ? 1.0f : 0.0f;
                }
                level *= port.volume;
                if (level == 0.0f) {
                    continue;
                }
                const float* s = src + in * frames;
                float* d = dest->bus.data() + o * MaxGrain;
                for (u32 n = 0; n < frames; ++n) {
                    d[n] += s[n] * level;
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
            const float s =
                c < MaxChannels && v.bus_used ? std::clamp(v.bus[c * MaxGrain + n], -1.0f, 1.0f)
                                              : 0.0f;
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
        break;
    }
    case VoicePatch: {
        const auto& m = As<OrbisNgs2VoicePatchParam>(p);
        GetPort(v, m.port).dest = FindVoice(m.destHandle);
        break;
    }
    case VoiceKickEvent:
        switch (As<OrbisNgs2VoiceEventParam>(p).eventId) {
        case Play:
            v.state = (v.state | InUse | Playing) & ~(Stopped | Paused | Empty);
            break;
        case Stop:
        case StopImm:
        case Kill:
            // ponytail: no release envelope, stop is immediate.
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
        v.pitch = 1.0f;
        v.phase = 1.0;
        v.state = InUse;
        break;
    case SamplerWaveformBlocks: {
        const auto& m = As<OrbisNgs2SamplerVoiceWaveformBlocksParam>(p);
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
        sampler->envelopeHeight = 1.0f;
        sampler->numDecodedSamples = voice->decoded_samples;
        sampler->decodedDataSize = voice->decoded_bytes;
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
            if ((voice->state & (Playing | Paused)) != Playing) {
                continue;
            }
            const u32 ch = RenderSampler(*voice, scratch.data(), frames);
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
        Route(*voice, scratch.data(), ch, frames);
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
        } else {
            std::memset(buffers[i].buffer, 0, buffers[i].bufferSize);
        }
    }
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2::Synth
