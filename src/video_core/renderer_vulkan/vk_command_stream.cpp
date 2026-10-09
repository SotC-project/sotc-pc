// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <immintrin.h>

#include "common/assert.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"

namespace Vulkan {

CommandStream::CommandStream() {
    const char* setting = std::getenv("SOTC_WORKER_RECORD");
    async = setting == nullptr || std::atoi(setting) != 0;
    if (async) {
        worker = std::jthread([this](std::stop_token stop) { WorkerLoop(stop); });
    }
}

CommandStream::~CommandStream() {
    if (async) {
        Drain();
        worker.request_stop();
        wake.notify_all();
    }
}

void CommandStream::Reserve(size_t aux_bytes) {
    if (!async) {
        return;
    }
    Chunk& chunk = chunks[write_index];
    ASSERT_MSG(aux_bytes + 256 < Chunk::AuxBytes, "Command data too large for the stream");
    // Room for the copied data (with alignment slack) and for the largest record.
    if (chunk.command_used + 1024 > Chunk::CommandBytes ||
        chunk.aux_used + aux_bytes + 256 > Chunk::AuxBytes) {
        Publish();
    }
    chunks[write_index].target = target;
}

void CommandStream::Publish() {
    Chunk& chunk = chunks[write_index];
    if (chunk.command_used == 0) {
        chunk.aux_used = 0;
        return;
    }
    const u64 sequence = published.load(std::memory_order_relaxed) + 1;
    {
        // The lock pairs with the worker's wait so a wake-up cannot be missed.
        std::scoped_lock lock{mutex};
        published.store(sequence, std::memory_order_release);
    }
    wake.notify_one();
    // The next chunk in the ring is free once the worker is less than a full ring behind.
    while (sequence - completed.load(std::memory_order_acquire) >= NumChunks) {
        _mm_pause();
    }
    write_index = static_cast<u32>(sequence % NumChunks);
    chunks[write_index].command_used = 0;
    chunks[write_index].aux_used = 0;
    chunks[write_index].target = target;
}

void CommandStream::Drain() {
    if (!async) {
        return;
    }
    Publish();
    const u64 goal = published.load(std::memory_order_relaxed);
    if (completed.load(std::memory_order_acquire) == goal) {
        return;
    }
    ++waited_drains;
    u32 spins = 0;
    while (completed.load(std::memory_order_acquire) != goal) {
        if (++spins < 4000) {
            _mm_pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void CommandStream::WorkerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandRecorder");
    u64 next = 0;
    while (!stop.stop_requested()) {
        // Commands arrive in bursts a few microseconds apart: look again for a short while
        // before sleeping, so the producer rarely pays for a wake-up.
        u32 spins = 0;
        while (published.load(std::memory_order_acquire) == next && spins < 20000) {
            _mm_pause();
            ++spins;
        }
        if (published.load(std::memory_order_acquire) == next) {
            std::unique_lock lock{mutex};
            wake.wait(lock, stop, [&] { return published.load(std::memory_order_acquire) != next; });
            if (stop.stop_requested()) {
                break;
            }
        }
        const Chunk& chunk = chunks[next % NumChunks];
        const u8* cursor = chunk.commands.get();
        const u8* const end = cursor + chunk.command_used;
        while (cursor < end) {
            const auto* record = reinterpret_cast<const Record*>(cursor);
            record->invoke(record + 1, chunk.target);
            cursor += record->size;
        }
        ++next;
        completed.store(next, std::memory_order_release);
    }
}

void CommandStream::pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout,
                                         u32 set, std::span<const vk::WriteDescriptorSet> writes) {
    if (!async) {
        target.pushDescriptorSetKHR(point, layout, set, static_cast<u32>(writes.size()),
                                    writes.data());
        return;
    }
    // The writes point at arrays the caller reuses for the next draw: copy those too.
    size_t bytes = writes.size_bytes();
    bool chained = false;
    for (const auto& write : writes) {
        chained |= write.pNext != nullptr;
        bytes += write.descriptorCount *
                     std::max(sizeof(vk::DescriptorImageInfo), sizeof(vk::DescriptorBufferInfo)) +
                 16;
    }
    if (chained || bytes > MaxQueuedData) {
        // Chained, or too much to copy into a chunk: record it in place, in its turn.
        Drain();
        target.pushDescriptorSetKHR(point, layout, set, static_cast<u32>(writes.size()),
                                    writes.data());
        return;
    }
    Reserve(bytes);
    auto* copy = const_cast<vk::WriteDescriptorSet*>(Copy(writes.data(), writes.size()));
    for (size_t i = 0; i < writes.size(); i++) {
        const auto& write = writes[i];
        if (write.pImageInfo) {
            copy[i].pImageInfo = Copy(write.pImageInfo, write.descriptorCount);
        }
        if (write.pBufferInfo) {
            copy[i].pBufferInfo = Copy(write.pBufferInfo, write.descriptorCount);
        }
        if (write.pTexelBufferView) {
            copy[i].pTexelBufferView = Copy(write.pTexelBufferView, write.descriptorCount);
        }
    }
    const u32 count = static_cast<u32>(writes.size());
    Push(0, [=](vk::CommandBuffer c) { c.pushDescriptorSetKHR(point, layout, set, count, copy); });
}

void CommandStream::beginRendering(const vk::RenderingInfo& info) {
    if (!async) {
        target.beginRendering(info);
        return;
    }
    if (info.pNext != nullptr) {
        Drain();
        target.beginRendering(info);
        return;
    }
    Reserve(sizeof(info) + (info.colorAttachmentCount + 2) * sizeof(vk::RenderingAttachmentInfo));
    auto* copy = const_cast<vk::RenderingInfo*>(Copy(&info, 1));
    copy->pColorAttachments = Copy(info.pColorAttachments, info.colorAttachmentCount);
    if (info.pDepthAttachment) {
        copy->pDepthAttachment = Copy(info.pDepthAttachment, 1);
    }
    if (info.pStencilAttachment) {
        copy->pStencilAttachment = Copy(info.pStencilAttachment, 1);
    }
    Push(0, [=](vk::CommandBuffer c) { c.beginRendering(*copy); });
}

void CommandStream::pipelineBarrier2(const vk::DependencyInfo& info) {
    if (!async) {
        target.pipelineBarrier2(info);
        return;
    }
    const size_t bytes = sizeof(info) + info.memoryBarrierCount * sizeof(vk::MemoryBarrier2) +
                         info.bufferMemoryBarrierCount * sizeof(vk::BufferMemoryBarrier2) +
                         info.imageMemoryBarrierCount * sizeof(vk::ImageMemoryBarrier2);
    // A chained structure cannot be copied blindly, and a very long barrier list (one entry per
    // mip and layer of a large image) does not fit a chunk: both are recorded in place.
    if (info.pNext != nullptr || bytes > MaxQueuedData) {
        Drain();
        target.pipelineBarrier2(info);
        return;
    }
    Reserve(bytes);
    auto* copy = const_cast<vk::DependencyInfo*>(Copy(&info, 1));
    copy->pMemoryBarriers = Copy(info.pMemoryBarriers, info.memoryBarrierCount);
    copy->pBufferMemoryBarriers = Copy(info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount);
    copy->pImageMemoryBarriers = Copy(info.pImageMemoryBarriers, info.imageMemoryBarrierCount);
    Push(0, [=](vk::CommandBuffer c) { c.pipelineBarrier2(*copy); });
}

} // namespace Vulkan
