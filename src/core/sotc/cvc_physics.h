// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

/// Colossus vs colossus as two physical bodies: each colossus is the game's own collision
/// (convex hulls of its per-bone collision parts, re/cvc/hulls.py, resources/cvc_hulls.txt) riding
/// its animated bones; where the two sets of hulls meet (GJK, depth and normal by EPA) the bodies
/// are pushed out of each other and exchange momentum by their masses (hull volumes).
namespace Sotc::CvcPhysics {

using Vec3 = std::array<float, 3>;

/// One meeting of a part of each.
struct Hit {
    int striker = 0;   // 0 or 1: the one driving into the other there
    int bone = -1;     // the striker's bone whose part met
    int struck_bone = -1; // the struck one's bone whose part was met
    Vec3 point{};      // where (world)
    Vec3 normal{};     // from the striker into the struck (unit)
    float closing = 0; // how fast they came together along it (units/s)
    float impulse = 0; // momentum exchanged (volume units x units/s)
    float depth = 0;   // how far the two parts are inside each other (units)
    bool first = false; // these two parts just met (a blow), not resting against each other
};

struct Result {
    std::vector<Hit> hits;
    Vec3 shift[2]{}; // move each root by this now (out of the other; ground plane only)
    Vec3 kick[2]{};  // each root's change of velocity (units/s; ground plane only)
    Vec3 spin[2]{};  // each body's change of angular velocity about its root (rad/s, world axis)
    float gap = 1e9f; // how near the two came: the least gap between the boxes of a part of each (0: met)
};

/// One step. kinds: each colossus's boss index (2 Gaius, 8 Basaran); bones: its 35 bone matrices
/// (4x4, row vectors: axes in rows 0-2, position in row 3); roots: its root position. False
/// when either has no hulls (nothing done).
bool Step(const int kinds[2], const float* const bones[2], const float* const roots[2], float dt, Result& out);

/// A colossus's mass (its hulls' volume), 0 when unknown.
float Mass(int kind);

/// A colossus's skeleton: each of its 35 bones' parent (-1 the root), null when unknown.
const std::array<int, 35>* Parents(int kind);

/// Two convex point sets overlapping: true with the direction to move b out of a (unit) and how
/// far. (Exposed for the self-check.)
bool Overlap(const std::vector<Vec3>& a, const std::vector<Vec3>& b, Vec3& normal, float& depth);

} // namespace Sotc::CvcPhysics
