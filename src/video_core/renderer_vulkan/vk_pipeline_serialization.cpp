// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <SDL3/SDL.h>
#include <fmt/format.h>
#include "sdl_window.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern Frontend::WindowSDL* g_window;

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 7u;
static constexpr u32 ShaderMetaVersion = 6u;
static constexpr u32 PipelineKeyVersion = 3u;
} // namespace Serialization

namespace Vulkan {

/// Worker threads for the pipeline preload. Nearly all of the time goes to the graphics driver
/// compiling each pipeline, and the driver does that for several pipelines at once.
struct PreloadPool {
    explicit PreloadPool(u32 num_threads) {
        for (u32 i = 0; i < num_threads; i++) {
            threads.emplace_back([this] {
                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock{mutex};
                        wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                        if (jobs.empty()) {
                            return;
                        }
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    space.notify_one();
                    job();
                    completed.fetch_add(1, std::memory_order_release);
                }
            });
        }
    }

    ~PreloadPool() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        wake.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    /// Queues a job; waits while the queue is long, so the pending data stays small.
    void Submit(std::function<void()>&& job) {
        {
            std::unique_lock lock{mutex};
            space.wait(lock, [&] { return jobs.size() < threads.size() * 4; });
            jobs.push_back(std::move(job));
        }
        ++submitted;
        wake.notify_one();
    }

    struct Built {
        GraphicsPipelineKey key;
        std::unique_ptr<GraphicsPipeline> pipeline;
    };
    struct BuiltCompute {
        ComputePipelineKey key;
        std::unique_ptr<ComputePipeline> pipeline;
    };

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable space;
    std::deque<std::function<void()>> jobs;
    bool stopping{};
    u32 submitted{};
    std::atomic<u32> completed{};
    /// One entry per queued pipeline; a deque keeps the entries in place while it grows.
    std::deque<Built> built;
    std::deque<BuiltCompute> built_compute;
};

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    if (preload_pool) {
        auto& built = preload_pool->built_compute.emplace_back();
        built.key = compute_key;
        preload_pool->Submit(
            [this, &built, key = compute_key, info = infos[0], module = modules[0], sdata]() mutable {
                built.pipeline = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile, *pipeline_cache, key, *info, module,
                    sdata, true);
            });
    } else {
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, true);
    }

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    if (preload_pool) {
        // Everything the constructor reads is copied, because the members below are reused for
        // the next pipeline. The shader infos themselves stay where they are for good.
        auto& built = preload_pool->built.emplace_back();
        built.key = graphics_key;
        preload_pool->Submit(
            [this, &built, key = graphics_key, job_infos = infos, job_runtime_infos = runtime_infos,
             job_modules = modules,
             job_fetch = fetch_shader ? std::optional{*fetch_shader} : std::nullopt,
             job_sdata = std::move(sdata)]() mutable {
                built.pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, key, *pipeline_cache, job_infos,
                    job_runtime_infos, job_fetch ? &*job_fetch : nullptr, job_modules, job_sdata,
                    true);
            });
    } else {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, true);
    }

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

namespace {

/// A small window with a progress bar, shown while the saved pipelines load. The game's own
/// window cannot draw yet at that point.
class LoadingSplash {
public:
    explicit LoadingSplash(u32 total_) : total{std::max(total_, 1u)} {
        const bool hidden = std::getenv("SOTC_HEADLESS") != nullptr;
        window = SDL_CreateWindow("Shadow of the Colossus - preparing shaders", Width, Height,
                                  SDL_WINDOW_ALWAYS_ON_TOP | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (window && !hidden) {
            SDL_RaiseWindow(window);
        }
        Update(0);
    }

    ~LoadingSplash() {
        if (window) {
            SDL_DestroyWindow(window);
        }
    }

    void Update(u32 done) {
        // Keep answering the system, or it marks the windows as not responding.
        SDL_PumpEvents();
        if (!window) {
            return;
        }
        const u32 percent = std::min(done * 100 / total, 100u);
        SDL_SetWindowTitle(window,
                           fmt::format("Shadow of the Colossus - preparing shaders {}%", percent)
                               .c_str());
        SDL_Surface* surface = SDL_GetWindowSurface(window);
        if (!surface) {
            return;
        }
        const auto fill = [&](int x, int y, int w, int h, u8 r, u8 g, u8 b) {
            const SDL_Rect rect{x, y, w, h};
            SDL_FillSurfaceRect(surface, &rect, SDL_MapSurfaceRGB(surface, r, g, b));
        };
        // Weathered stone, a carved slot, moss filling it.
        fill(0, 0, Width, Height, 30, 31, 28);
        const int x = 28, y = 34, w = Width - 56, h = 28;
        fill(x - 3, y - 3, w + 6, h + 6, 120, 124, 108);
        fill(x, y, w, h, 46, 47, 43);
        fill(x, y, static_cast<int>(static_cast<u64>(w) * done / total), h, 104, 138, 66);
        SDL_UpdateWindowSurface(window);
        if (const char* dump = std::getenv("SOTC_SPLASH_DUMP"); dump && !dumped && percent >= 50) {
            dumped = true;
            SDL_SaveBMP(surface, dump);
        }
    }

private:
    static constexpr int Width = 560;
    static constexpr int Height = 96;
    SDL_Window* window{};
    u32 total;
    bool dumped{};
};

} // namespace

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render,
                    "Pipeline cache profile has unexpected size ({} != {}). Ignoring the cache",
                    profile_data.size(), sizeof(Shader::Profile));
        Storage::DataBase::Instance().Close();
        return;
    }

    Shader::Profile cached_profile{};
    std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (cached_profile != profile) {
        LOG_WARNING(Render,
                    "Pipeline cache isn't compatible with current system. Ignoring the cache");
        Storage::DataBase::Instance().Close();
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    // Loading a few thousand pipelines takes tens of seconds on the window's own thread, and
    // minutes the first time a new program file runs (the graphics driver compiles them all
    // again). Show how far it is.
    u32 expected_pipelines{};
    Storage::DataBase::Instance().ForEachBlob(Storage::BlobType::PipelineKey,
                                              [&](std::vector<u8>&&) { ++expected_pipelines; });
    LoadingSplash splash{expected_pipelines};
    auto last_update = std::chrono::steady_clock::now();

    // Two threads are left for the rest of the system. SOTC_PRELOAD_THREADS=0 builds every
    // pipeline on this thread, as before.
    u32 num_threads = std::clamp(std::thread::hardware_concurrency(), 3u, 18u) - 2;
    if (const char* setting = std::getenv("SOTC_PRELOAD_THREADS")) {
        num_threads = std::min<u32>(std::atoi(setting), 32);
    }
    std::optional<PreloadPool> pool;
    if (num_threads > 0) {
        pool.emplace(num_threads);
        preload_pool = &*pool;
    }
    const auto start_time = std::chrono::steady_clock::now();
    // Pipelines finished so far: built ones when the pool builds them, otherwise visited ones.
    const auto progress = [&] {
        if (!pool) {
            return num_total_pipelines;
        }
        const u32 waiting = pool->submitted - pool->completed.load(std::memory_order_acquire);
        return num_total_pipelines - std::min(waiting, num_total_pipelines);
    };

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            if (const auto now = std::chrono::steady_clock::now();
                now - last_update > std::chrono::milliseconds(100)) {
                last_update = now;
                splash.Update(progress());
            }

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    if (pool) {
        while (pool->completed.load(std::memory_order_acquire) != pool->submitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            splash.Update(progress());
        }
        preload_pool = nullptr;
        for (auto& built : pool->built) {
            graphics_pipelines.at(built.key) = std::move(built.pipeline);
        }
        for (auto& built : pool->built_compute) {
            compute_pipelines.at(built.key) = std::move(built.pipeline);
        }
        pool.reset();
    }

    LOG_INFO(Render, "Preloaded {} pipelines in {:.1f} s on {} threads", num_pipelines,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count(),
             std::max(num_threads, 1u));
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
