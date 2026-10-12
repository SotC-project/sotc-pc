// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace {

std::mutex submit_mutex{};
u32 num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

// The pack (see DataBase::LoadPack). Layout: the magic, then for every blob its name length
// (u16), name, data size (u32) and data.
constexpr char PackMagic[8] = {'S', 'O', 'T', 'C', 'P', 'K', '0', '1'};
struct PackEntry {
    u64 offset;
    u32 size;
};
std::vector<u8> pack_data{};
std::unordered_map<std::string, PackEntry> pack_index{};
// Blobs read from their own file because the pack lacks them or holds another size.
u32 pack_misses{};

const PackEntry* FindInPack(const std::filesystem::path& path) {
    if (pack_index.empty()) {
        return nullptr;
    }
    const auto it = pack_index.find(path.filename().string());
    return it != pack_index.end() ? &it->second : nullptr;
}

void ParsePack() {
    pack_index.clear();
    const u64 size = pack_data.size();
    if (size < sizeof(PackMagic) || std::memcmp(pack_data.data(), PackMagic, sizeof(PackMagic))) {
        pack_data.clear();
        return;
    }
    u64 pos = sizeof(PackMagic);
    while (pos < size) {
        u16 name_size{};
        u32 blob_size{};
        if (pos + sizeof(name_size) > size) {
            break;
        }
        std::memcpy(&name_size, &pack_data[pos], sizeof(name_size));
        pos += sizeof(name_size);
        if (pos + name_size + sizeof(blob_size) > size) {
            break;
        }
        std::string name(reinterpret_cast<const char*>(&pack_data[pos]), name_size);
        pos += name_size;
        std::memcpy(&blob_size, &pack_data[pos], sizeof(blob_size));
        pos += sizeof(blob_size);
        if (pos + blob_size > size) {
            // Cut short; the blobs before this one are whole.
            break;
        }
        pack_index.insert_or_assign(std::move(name), PackEntry{pos, blob_size});
        pos += blob_size;
    }
}

// Writes a new pack from the blob files, taking from the old pack what it already holds.
void WritePack(std::filesystem::path cache_path, std::filesystem::path pack_path,
               std::vector<u8> old_data, std::unordered_map<std::string, PackEntry> old_index) {
    using namespace Common::FS;
    auto temp_path = pack_path;
    temp_path += ".tmp";
    u32 count{};
    // The pack is dated from before the first blob is read: a blob written again after that
    // is newer than the pack and read from its own file (see LoadPack).
    const auto started = std::filesystem::file_time_type::clock::now();
    {
        const auto out = IOFile{temp_path, FileAccessMode::Create};
        if (!out.IsOpen()) {
            return;
        }
        out.WriteRaw<u8>(PackMagic, sizeof(PackMagic));
        std::error_code ec;
        std::vector<u8> blob;
        for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
            const auto name = entry.path().filename().string();
            const u64 file_size = entry.file_size(ec);
            if (ec || file_size == 0 || file_size > UINT32_MAX || name.size() > UINT16_MAX) {
                continue;
            }
            const u8* data{};
            if (const auto it = old_index.find(name);
                it != old_index.end() && it->second.size == file_size) {
                data = &old_data[it->second.offset];
            } else {
                const auto file = IOFile{entry.path(), FileAccessMode::Read};
                blob.resize(file_size);
                if (!file.IsOpen() || file.ReadRaw<u8>(blob.data(), blob.size()) != blob.size()) {
                    continue;
                }
                data = blob.data();
            }
            const u16 name_size = static_cast<u16>(name.size());
            const u32 blob_size = static_cast<u32>(file_size);
            out.WriteRaw<u8>(&name_size, sizeof(name_size));
            out.WriteRaw<u8>(name.data(), name.size());
            out.WriteRaw<u8>(&blob_size, sizeof(blob_size));
            out.WriteRaw<u8>(data, blob_size);
            ++count;
        }
    }
    // Swapped in whole, so a start never finds a pack that is half written.
    std::error_code ec;
    std::filesystem::rename(temp_path, pack_path, ec);
    if (!ec) {
        std::error_code time_ec;
        std::filesystem::last_write_time(pack_path, started, time_ec);
    }
    LOG_INFO(Render, "Cache pack written: {} blobs{}", count, ec ? " (not put in place)" : "");
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        pack_path = cache_path;
        pack_path += ".pack";
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                const auto file = IOFile{path, FileAccessMode::Create};
                file.Write(v);
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else if (const PackEntry* packed = FindInPack(path)) {
        v.resize(packed->size / sizeof(T));
        std::memcpy(v.data(), &pack_data[packed->offset], v.size() * sizeof(T));
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
        ++pack_misses;
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                // The blob file decides whether a blob exists; the pack only spares reading it.
                std::error_code ec;
                if (const PackEntry* packed = FindInPack(file_name.path());
                    packed && file_name.file_size(ec) == packed->size && !ec) {
                    const u8* begin = &pack_data[packed->offset];
                    func(std::vector<u8>(begin, begin + packed->size));
                    continue;
                }
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    ++pack_misses;
                    func(std::move(data));
                }
            }
        }
    }
}

u32 DataBase::CountBlobs(BlobType type) {
    const auto& ext = GetBlobFileExtension(type);
    u32 count{};
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        ForEachBlob(type, [&](std::vector<u8>&&) { ++count; });
        return count;
    }
    std::error_code ec;
    for (const auto& file_name : std::filesystem::directory_iterator{cache_path, ec}) {
        count += file_name.path().extension().string().ends_with(ext);
    }
    return count;
}

void DataBase::LoadPack(const std::function<void()>& tick) {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    using namespace Common::FS;
    pack_data.clear();
    pack_index.clear();
    pack_misses = 0;
    const auto file = IOFile{pack_path, FileAccessMode::Read};
    if (!file.IsOpen()) {
        return;
    }
    pack_data.resize(file.GetSize());
    constexpr u64 Step = 4_MB;
    for (u64 pos = 0; pos < pack_data.size(); pos += Step) {
        const u64 count = std::min<u64>(Step, pack_data.size() - pos);
        if (file.ReadRaw<u8>(&pack_data[pos], count) != count) {
            pack_data.resize(pos);
            break;
        }
        tick();
    }
    ParsePack();
    // A blob file written after the pack holds something newer than the pack's copy, which may
    // be of the same size (a shader's description made again): the file is the one to read.
    std::error_code ec;
    const auto pack_time = std::filesystem::last_write_time(pack_path, ec);
    u32 newer{};
    if (!ec && !pack_index.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
            std::error_code entry_ec;
            if (entry.last_write_time(entry_ec) > pack_time && !entry_ec) {
                newer += u32(pack_index.erase(entry.path().filename().string()));
            }
        }
    }
    LOG_INFO(Render, "Cache pack: {} blobs, {} MB, {} left out as older than their files",
             pack_index.size(), pack_data.size() >> 20, newer);
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
        return;
    }
    if (!opened) {
        return;
    }
    // The pack is only read while the saved pipelines load. Blobs it lacked go into a new one,
    // written in the background.
    if (pack_misses != 0 || pack_index.empty()) {
        std::thread{WritePack, cache_path, pack_path, std::move(pack_data), std::move(pack_index)}
            .detach();
    }
    pack_data = {};
    pack_index = {};
}

} // namespace Storage
