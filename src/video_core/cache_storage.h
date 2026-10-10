// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

    /// Number of stored blobs of a type, found without reading any of them.
    u32 CountBlobs(BlobType type);

    /// Reads the pack: one file holding a copy of every blob, so that a start reads one file
    /// front to back instead of thousands of small ones, which takes minutes on a hard disk
    /// that has not read them since the PC started. `tick` is called while it loads.
    void LoadPack(const std::function<void()>& tick);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::filesystem::path pack_path{};
    bool opened{};
};

} // namespace Storage
