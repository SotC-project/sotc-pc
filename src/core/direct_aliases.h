// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "common/types.h"

namespace Core {

/// Tracks which virtual ranges map the same direct (physical) memory. Games may map a physical
/// range more than once; memory tracking has to treat all of those addresses as one.
class DirectAliasMap {
public:
    void Add(VAddr va, u64 pa, u64 size) {
        std::unique_lock lock{mutex};
        Insert({va, pa, size});
    }

    void Remove(VAddr va, u64 size) {
        std::unique_lock lock{mutex};
        // Collect the surviving head/tail pieces of every mapping the range cuts into.
        std::vector<Piece> keep;
        auto it = by_va.upper_bound(va);
        if (it != by_va.begin()) {
            --it;
        }
        while (it != by_va.end() && it->first < va + size) {
            const Piece piece = it->second;
            if (piece.va + piece.size <= va) {
                ++it;
                continue;
            }
            if (piece.va < va) {
                keep.push_back({piece.va, piece.pa, va - piece.va});
            }
            if (piece.va + piece.size > va + size) {
                const u64 skip = va + size - piece.va;
                keep.push_back({va + size, piece.pa + skip, piece.size - skip});
            }
            EraseByPa(piece);
            it = by_va.erase(it);
        }
        for (const Piece& piece : keep) {
            Insert(piece);
        }
    }

    /// Calls func(alias_va, size) for every other virtual range backed by the same physical
    /// memory as [va, va + size).
    template <typename Func>
    void ForEachAlias(VAddr va, u64 size, Func&& func) const {
        std::shared_lock lock{mutex};
        auto it = by_va.upper_bound(va);
        if (it == by_va.begin()) {
            return;
        }
        --it;
        for (; it != by_va.end() && it->first < va + size; ++it) {
            const Piece& self = it->second;
            const VAddr start = std::max(va, self.va);
            const VAddr end = std::min(va + size, self.va + self.size);
            if (start >= end) {
                continue;
            }
            const u64 pa_start = self.pa + (start - self.va);
            const u64 pa_end = pa_start + (end - start);
            const auto visit = [&](const Piece& piece) {
                const u64 lo = std::max(pa_start, piece.pa);
                const u64 hi = std::min(pa_end, piece.pa + piece.size);
                if (lo < hi && piece.va != self.va) {
                    func(piece.va + (lo - piece.pa), hi - lo);
                }
            };
            for (const Piece& piece : large) {
                visit(piece);
            }
            auto pit = by_pa.lower_bound(pa_start > LargeSize ? pa_start - LargeSize : 0);
            for (; pit != by_pa.end() && pit->first < pa_end; ++pit) {
                for (const Piece& piece : pit->second) {
                    visit(piece);
                }
            }
        }
    }

private:
    struct Piece {
        VAddr va;
        u64 pa;
        u64 size;
    };

    void Insert(const Piece& piece) {
        by_va[piece.va] = piece;
        if (piece.size >= LargeSize) {
            large.push_back(piece);
        } else {
            by_pa[piece.pa].push_back(piece);
        }
    }

    void EraseByPa(const Piece& piece) {
        const auto drop = [&](std::vector<Piece>& pieces) {
            std::erase_if(pieces, [&](const Piece& p) { return p.va == piece.va; });
        };
        if (piece.size >= LargeSize) {
            drop(large);
        } else if (auto it = by_pa.find(piece.pa); it != by_pa.end()) {
            drop(it->second);
            if (it->second.empty()) {
                by_pa.erase(it);
            }
        }
    }

    // Mappings at least this big are kept in a flat list so interval queries over the rest only
    // have to look back this far.
    static constexpr u64 LargeSize = 64ULL << 20;

    mutable std::shared_mutex mutex;
    std::map<VAddr, Piece> by_va;
    std::map<u64, std::vector<Piece>> by_pa;
    std::vector<Piece> large;
};

inline DirectAliasMap& DirectAliases() {
    static DirectAliasMap instance;
    return instance;
}

} // namespace Core
