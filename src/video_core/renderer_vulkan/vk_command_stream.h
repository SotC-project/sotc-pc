// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// Records the commands issued for every draw on a second thread.
///
/// The GPU command thread spends a good part of its time inside the Vulkan driver recording
/// commands. The per-draw ones (binds, dynamic state, draws, dispatches, barriers) are written
/// here as small records instead and replayed, in order, into the real command buffer by a
/// worker. Anything else calls Drain() first and then uses the command buffer directly, so the
/// order of commands never changes. SOTC_WORKER_RECORD=0 makes every call run in place.
class CommandStream {
public:
    CommandStream();
    ~CommandStream();

    /// The command buffer the following calls go to. Changing it requires a Drain() first.
    void SetTarget(vk::CommandBuffer cmdbuf) {
        target = cmdbuf;
    }

    /// Returns once every queued command has been recorded into its command buffer.
    void Drain();

    [[nodiscard]] bool IsAsync() const {
        return async;
    }
    /// Counters for the periodic statistics line: commands queued and drains that had to wait.
    u64 queued_commands{};
    u64 waited_drains{};

    // ---- Queued commands. Signatures follow vk::CommandBuffer where the renderer uses them. ----

    void bindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) {
        Push(0, [=](vk::CommandBuffer c) { c.bindPipeline(point, pipeline); });
    }
    void draw(u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) {
        Push(0, [=](vk::CommandBuffer c) { c.draw(vertices, instances, first_vertex, first_instance); });
    }
    void drawIndexed(u32 indices, u32 instances, u32 first_index, s32 vertex_offset,
                     u32 first_instance) {
        Push(0, [=](vk::CommandBuffer c) {
            c.drawIndexed(indices, instances, first_index, vertex_offset, first_instance);
        });
    }
    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count, u32 stride) {
        Push(0, [=](vk::CommandBuffer c) { c.drawIndirect(buffer, offset, count, stride); });
    }
    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count, u32 stride) {
        Push(0, [=](vk::CommandBuffer c) { c.drawIndexedIndirect(buffer, offset, count, stride); });
    }
    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, u32 max_count, u32 stride) {
        Push(0, [=](vk::CommandBuffer c) {
            c.drawIndirectCount(buffer, offset, count_buffer, count_offset, max_count, stride);
        });
    }
    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                                  vk::DeviceSize count_offset, u32 max_count, u32 stride) {
        Push(0, [=](vk::CommandBuffer c) {
            c.drawIndexedIndirectCount(buffer, offset, count_buffer, count_offset, max_count,
                                       stride);
        });
    }
    void dispatch(u32 x, u32 y, u32 z) {
        Push(0, [=](vk::CommandBuffer c) { c.dispatch(x, y, z); });
    }
    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) {
        Push(0, [=](vk::CommandBuffer c) { c.dispatchIndirect(buffer, offset); });
    }
    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) {
        Push(0, [=](vk::CommandBuffer c) { c.bindIndexBuffer(buffer, offset, type); });
    }
    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) {
        Reserve(count * (sizeof(vk::Buffer) + sizeof(vk::DeviceSize)));
        const auto* b = Copy(buffers, count);
        const auto* o = Copy(offsets, count);
        Push(0, [=](vk::CommandBuffer c) { c.bindVertexBuffers(first, count, b, o); });
    }
    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) {
        Reserve(count * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)));
        const auto* b = Copy(buffers, count);
        const auto* o = Copy(offsets, count);
        const auto* s = Copy(sizes, count);
        const auto* t = Copy(strides, count);
        Push(0, [=](vk::CommandBuffer c) { c.bindVertexBuffers2(first, count, b, o, s, t); });
    }
    void setVertexInputEXT(std::span<const vk::VertexInputBindingDescription2EXT> bindings,
                           std::span<const vk::VertexInputAttributeDescription2EXT> attributes) {
        Reserve(bindings.size_bytes() + attributes.size_bytes());
        const auto* b = Copy(bindings.data(), bindings.size());
        const auto* a = Copy(attributes.data(), attributes.size());
        const u32 nb = static_cast<u32>(bindings.size()), na = static_cast<u32>(attributes.size());
        Push(0, [=](vk::CommandBuffer c) { c.setVertexInputEXT(nb, b, na, a); });
    }
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* values) {
        Reserve(size);
        const auto* data = Copy(static_cast<const u8*>(values), size);
        Push(0, [=](vk::CommandBuffer c) { c.pushConstants(layout, stages, offset, size, data); });
    }
    void pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout, u32 set,
                              std::span<const vk::WriteDescriptorSet> writes);
    void bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout, u32 first,
                            vk::DescriptorSet set) {
        Push(0, [=](vk::CommandBuffer c) { c.bindDescriptorSets(point, layout, first, set, {}); });
    }
    void beginRendering(const vk::RenderingInfo& info);
    void endRendering() {
        Push(0, [](vk::CommandBuffer c) { c.endRendering(); });
    }
    void pipelineBarrier2(const vk::DependencyInfo& info);

    // Dynamic state.
    void setViewportWithCount(std::span<const vk::Viewport> viewports) {
        Reserve(viewports.size_bytes());
        const auto* v = Copy(viewports.data(), viewports.size());
        const u32 n = static_cast<u32>(viewports.size());
        Push(0, [=](vk::CommandBuffer c) { c.setViewportWithCount(n, v); });
    }
    void setScissorWithCount(std::span<const vk::Rect2D> scissors) {
        Reserve(scissors.size_bytes());
        const auto* s = Copy(scissors.data(), scissors.size());
        const u32 n = static_cast<u32>(scissors.size());
        Push(0, [=](vk::CommandBuffer c) { c.setScissorWithCount(n, s); });
    }
    void setColorWriteMaskEXT(u32 first, std::span<const vk::ColorComponentFlags> masks) {
        Reserve(masks.size_bytes());
        const auto* m = Copy(masks.data(), masks.size());
        const u32 n = static_cast<u32>(masks.size());
        Push(0, [=](vk::CommandBuffer c) { c.setColorWriteMaskEXT(first, n, m); });
    }
    void setBlendConstants(const float* constants) {
        const std::array<float, 4> k{constants[0], constants[1], constants[2], constants[3]};
        Push(0, [=](vk::CommandBuffer c) { c.setBlendConstants(k.data()); });
    }
    // State setters that take plain values are forwarded as they are.
    template <typename... Args>
    void setDepthTestEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthTestEnable(args...); });
    }
    template <typename... Args>
    void setDepthWriteEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthWriteEnable(args...); });
    }
    template <typename... Args>
    void setDepthCompareOp(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthCompareOp(args...); });
    }
    template <typename... Args>
    void setDepthBoundsTestEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthBoundsTestEnable(args...); });
    }
    template <typename... Args>
    void setDepthBounds(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthBounds(args...); });
    }
    template <typename... Args>
    void setDepthBiasEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthBiasEnable(args...); });
    }
    template <typename... Args>
    void setDepthBias(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setDepthBias(args...); });
    }
    template <typename... Args>
    void setStencilTestEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setStencilTestEnable(args...); });
    }
    template <typename... Args>
    void setStencilOp(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setStencilOp(args...); });
    }
    template <typename... Args>
    void setStencilWriteMask(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setStencilWriteMask(args...); });
    }
    template <typename... Args>
    void setStencilReference(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setStencilReference(args...); });
    }
    template <typename... Args>
    void setStencilCompareMask(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setStencilCompareMask(args...); });
    }
    template <typename... Args>
    void setPrimitiveRestartEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setPrimitiveRestartEnable(args...); });
    }
    template <typename... Args>
    void setRasterizerDiscardEnable(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setRasterizerDiscardEnable(args...); });
    }
    template <typename... Args>
    void setCullMode(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setCullMode(args...); });
    }
    template <typename... Args>
    void setFrontFace(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setFrontFace(args...); });
    }
    template <typename... Args>
    void setLineWidth(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setLineWidth(args...); });
    }
    template <typename... Args>
    void setAttachmentFeedbackLoopEnableEXT(Args... args) {
        Push(0, [=](vk::CommandBuffer c) { c.setAttachmentFeedbackLoopEnableEXT(args...); });
    }
    /// Any other state setter that takes plain values: stream.Set([=](auto c) { c.setX(v); }).
    template <typename F>
    void Set(F&& func) {
        Push(0, std::forward<F>(func));
    }

private:
    struct Record {
        void (*invoke)(const void* closure, vk::CommandBuffer cmdbuf);
        u32 size; // of the whole record, aligned
    };
    struct Chunk {
        static constexpr size_t CommandBytes = 48 * 1024;
        static constexpr size_t AuxBytes = 160 * 1024;
        vk::CommandBuffer target{};
        u32 command_used{};
        u32 aux_used{};
        std::unique_ptr<u8[]> commands{new u8[CommandBytes]};
        std::unique_ptr<u8[]> aux{new u8[AuxBytes]};
    };
    static constexpr size_t NumChunks = 16;
    /// Larger copies than this are not queued; the command is recorded in place instead.
    static constexpr size_t MaxQueuedData = Chunk::AuxBytes / 4;
    static constexpr u32 PublishAfter = 16 * 1024;

    /// Makes sure the record about to be pushed, with `aux_bytes` of copied data, fits in the
    /// current chunk; the data and the record that points at it must not end up in two chunks.
    void Reserve(size_t aux_bytes);
    void Publish();
    void WorkerLoop(std::stop_token stop);

    template <typename T>
    const T* Copy(const T* source, size_t count) {
        if (count == 0) {
            return nullptr;
        }
        if (!async) {
            return source;
        }
        Chunk& chunk = chunks[write_index];
        const u32 offset = (chunk.aux_used + alignof(T) - 1) & ~static_cast<u32>(alignof(T) - 1);
        T* dest = reinterpret_cast<T*>(chunk.aux.get() + offset);
        std::memcpy(dest, source, count * sizeof(T));
        chunk.aux_used = offset + static_cast<u32>(count * sizeof(T));
        return dest;
    }

    template <typename F>
    void Push(size_t, F&& func) {
        using Closure = std::decay_t<F>;
        static_assert(std::is_trivially_copyable_v<Closure>);
        if (!async) {
            func(target);
            return;
        }
        const u32 size = static_cast<u32>((sizeof(Record) + sizeof(Closure) + 7) & ~size_t{7});
        Reserve(0);
        Chunk& chunk = chunks[write_index];
        auto* record = reinterpret_cast<Record*>(chunk.commands.get() + chunk.command_used);
        record->invoke = [](const void* closure, vk::CommandBuffer cmdbuf) {
            (*static_cast<const Closure*>(closure))(cmdbuf);
        };
        record->size = size;
        std::memcpy(record + 1, &func, sizeof(Closure));
        chunk.command_used += size;
        ++queued_commands;
        if (chunk.command_used >= PublishAfter) {
            Publish();
        }
    }

    bool async{};
    vk::CommandBuffer target{};
    std::array<Chunk, NumChunks> chunks;
    u32 write_index{};
    // Chunks are handed over in order: `published` counts chunks given to the worker,
    // `completed` those it has replayed.
    std::atomic<u64> published{0};
    std::atomic<u64> completed{0};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread worker;
};

} // namespace Vulkan
