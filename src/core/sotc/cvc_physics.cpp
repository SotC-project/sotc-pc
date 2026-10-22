// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#ifndef CVC_PHYSICS_SELF_CHECK
#include <cmrc/cmrc.hpp>
CMRC_DECLARE(res);
#endif

#include "core/sotc/cvc_physics.h"

namespace Sotc::CvcPhysics {

namespace {

Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 operator-(const Vec3& a) {
    return {-a[0], -a[1], -a[2]};
}
Vec3 operator*(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Length(const Vec3& a) {
    return std::sqrt(Dot(a, a));
}

// The point of a set farthest along d.
const Vec3& Far(const std::vector<Vec3>& points, const Vec3& d) {
    size_t best = 0;
    float most = Dot(points[0], d);
    for (size_t i = 1; i < points.size(); i++) {
        if (const float along = Dot(points[i], d); along > most) {
            most = along, best = i;
        }
    }
    return points[best];
}

// A point of the Minkowski difference a - b farthest along d.
Vec3 Support(const std::vector<Vec3>& a, const std::vector<Vec3>& b, const Vec3& d) {
    return Far(a, d) - Far(b, -d);
}

// GJK: the simplex (newest point first) and the next direction to search.
struct Simplex {
    std::array<Vec3, 4> p{};
    int n = 0;
    void Front(const Vec3& v) {
        for (int i = std::min(n, 3); i > 0; i--) {
            p[i] = p[i - 1];
        }
        p[0] = v;
        n = std::min(n + 1, 4);
    }
};

bool SameWay(const Vec3& a, const Vec3& b) {
    return Dot(a, b) > 0.0f;
}

// Any direction across a (when the origin lies on a line, the search goes sideways).
Vec3 Across(const Vec3& a) {
    const Vec3 side = Cross(a, {1.0f, 0.0f, 0.0f});
    return Dot(side, side) > 1e-12f ? side : Cross(a, {0.0f, 1.0f, 0.0f});
}

bool Line(Simplex& s, Vec3& d) {
    const Vec3 a = s.p[0], b = s.p[1], ab = b - a, ao = -a;
    if (SameWay(ab, ao)) {
        d = Cross(Cross(ab, ao), ab);
        if (Dot(d, d) < 1e-12f) {
            d = Across(ab);
        }
    } else {
        s.p = {a}, s.n = 1, d = ao;
    }
    return false;
}

bool Triangle(Simplex& s, Vec3& d) {
    const Vec3 a = s.p[0], b = s.p[1], c = s.p[2];
    const Vec3 ab = b - a, ac = c - a, ao = -a, abc = Cross(ab, ac);
    if (SameWay(Cross(abc, ac), ao)) {
        if (SameWay(ac, ao)) {
            s.p = {a, c}, s.n = 2, d = Cross(Cross(ac, ao), ac);
            return false;
        }
        s.p = {a, b}, s.n = 2;
        return Line(s, d);
    }
    if (SameWay(Cross(ab, abc), ao)) {
        s.p = {a, b}, s.n = 2;
        return Line(s, d);
    }
    if (SameWay(abc, ao)) {
        d = abc;
    } else {
        s.p = {a, c, b}, s.n = 3, d = -abc;
    }
    return false;
}

bool Tetrahedron(Simplex& s, Vec3& d) {
    const Vec3 a = s.p[0], b = s.p[1], c = s.p[2], e = s.p[3], ao = -a;
    const Vec3 abc = Cross(b - a, c - a), ace = Cross(c - a, e - a), aeb = Cross(e - a, b - a);
    if (SameWay(abc, ao)) {
        s.p = {a, b, c}, s.n = 3;
        return Triangle(s, d);
    }
    if (SameWay(ace, ao)) {
        s.p = {a, c, e}, s.n = 3;
        return Triangle(s, d);
    }
    if (SameWay(aeb, ao)) {
        s.p = {a, e, b}, s.n = 3;
        return Triangle(s, d);
    }
    return true;
}

bool Next(Simplex& s, Vec3& d) {
    switch (s.n) {
    case 2:
        return Line(s, d);
    case 3:
        return Triangle(s, d);
    case 4:
        return Tetrahedron(s, d);
    }
    return false;
}

bool Gjk(const std::vector<Vec3>& a, const std::vector<Vec3>& b, Simplex& s) {
    Vec3 d = a[0] - b[0];
    if (Dot(d, d) < 1e-12f) {
        d = {1.0f, 0.0f, 0.0f};
    }
    s = {};
    s.Front(Support(a, b, d));
    d = -s.p[0];
    for (int i = 0; i < 64; i++) {
        if (Dot(d, d) < 1e-12f) {
            return false; // (touching only)
        }
        const Vec3 p = Support(a, b, d);
        if (Dot(p, d) <= 0.0f) {
            return false;
        }
        s.Front(p);
        if (Next(s, d)) {
            return true;
        }
    }
    return false;
}

// EPA: from GJK's enclosing tetrahedron, the face of the Minkowski difference nearest the origin
// (its outward normal and distance: how far b must move along it to be clear of a).
bool Epa(const std::vector<Vec3>& a, const std::vector<Vec3>& b, const Simplex& s, Vec3& normal, float& depth) {
    std::vector<Vec3> poly(s.p.begin(), s.p.end());
    std::vector<std::array<int, 3>> faces{{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
    struct Face {
        Vec3 n;
        float d;
    };
    const auto face_of = [&](const std::array<int, 3>& f) {
        Vec3 n = Cross(poly[f[1]] - poly[f[0]], poly[f[2]] - poly[f[0]]);
        const float l = Length(n);
        if (l < 1e-9f) {
            return Face{{0.0f, 0.0f, 0.0f}, 1e30f}; // (degenerate: never the nearest)
        }
        n = n * (1.0f / l);
        float d = Dot(n, poly[f[0]]);
        if (d < 0.0f) {
            n = -n, d = -d;
        }
        return Face{n, d};
    };
    std::vector<Face> info;
    for (const auto& f : faces) {
        info.push_back(face_of(f));
    }
    for (int iteration = 0; iteration < 64; iteration++) {
        size_t nearest = 0;
        for (size_t i = 1; i < info.size(); i++) {
            if (info[i].d < info[nearest].d) {
                nearest = i;
            }
        }
        const Face best = info[nearest];
        if (best.d >= 1e29f) {
            return false;
        }
        const Vec3 p = Support(a, b, best.n);
        if (Dot(best.n, p) - best.d <= 1e-3f * std::max(1.0f, best.d)) {
            normal = best.n, depth = best.d;
            return true;
        }
        // Grow: drop the faces that see p, keep the rim of the hole, close it on p.
        std::vector<std::pair<int, int>> rim;
        std::vector<std::array<int, 3>> kept_faces;
        std::vector<Face> kept_info;
        for (size_t i = 0; i < faces.size(); i++) {
            if (Dot(info[i].n, p - poly[faces[i][0]]) > 0.0f) {
                for (int e = 0; e < 3; e++) {
                    const std::pair<int, int> edge{faces[i][e], faces[i][(e + 1) % 3]};
                    const auto back = std::find(rim.begin(), rim.end(), std::pair{edge.second, edge.first});
                    if (back != rim.end()) {
                        rim.erase(back);
                    } else {
                        rim.push_back(edge);
                    }
                }
            } else {
                kept_faces.push_back(faces[i]);
                kept_info.push_back(info[i]);
            }
        }
        if (rim.empty()) {
            normal = best.n, depth = best.d;
            return true;
        }
        poly.push_back(p);
        const int at = int(poly.size()) - 1;
        for (const auto& [i, j] : rim) {
            kept_faces.push_back({i, j, at});
            kept_info.push_back(face_of(kept_faces.back()));
        }
        faces.swap(kept_faces);
        info.swap(kept_info);
    }
    return false;
}

struct Part {
    int part = 0, bone = 0;
    float volume = 0.0f;
    std::vector<Vec3> local;
};
std::map<int, std::vector<Part>> g_parts;
std::map<int, std::vector<int>> g_parents;

void Load() {
#ifndef CVC_PHYSICS_SELF_CHECK
    const auto file = cmrc::res::get_filesystem().open("src/resources/cvc_hulls.txt");
    std::istringstream text{std::string(file.begin(), file.end())};
    for (std::string line; std::getline(text, line);) {
        std::istringstream in{line};
        int kind = 0, count = 0;
        if (line.starts_with("P ")) { // a skeleton: P kind and its bones' parents
            in.ignore(2);
            auto& parents = g_parents[(in >> kind, kind)];
            for (int parent; parents.size() < size_t(MaxBones) && in >> parent;) {
                parents.push_back(parent);
            }
            continue;
        }
        Part p;
        if (!(in >> kind >> p.part >> p.bone >> p.volume >> count)) {
            continue;
        }
        p.local.resize(size_t(count));
        for (auto& v : p.local) {
            in >> v[0] >> v[1] >> v[2];
        }
        g_parts[kind].push_back(p);
    }
#endif
}

struct Body {
    int kind = -1;
    std::vector<Vec3> before; // each part's centre the step before
};
Body g_body[2];
std::mutex g_shapes_lock;
std::vector<std::vector<Vec3>> g_shapes[2]; // as last placed (the overlay)
std::map<std::pair<int, int>, std::uint64_t> g_touched; // part pair (A's, B's) -> step it last touched
std::uint64_t g_step = 0;

} // namespace

bool Overlap(const std::vector<Vec3>& a, const std::vector<Vec3>& b, Vec3& normal, float& depth) {
    Simplex s;
    if (a.empty() || b.empty() || !Gjk(a, b, s) || s.n < 4) {
        return false;
    }
    return Epa(a, b, s, normal, depth);
}

const std::vector<int>* Parents(int kind) {
    Mass(kind); // (loaded)
    const auto it = g_parents.find(kind);
    return it == g_parents.end() ? nullptr : &it->second;
}

std::vector<std::vector<Vec3>> Shapes(int k) {
    std::scoped_lock guard{g_shapes_lock};
    return g_shapes[k & 1];
}

float Mass(int kind) {
    static std::once_flag once;
    std::call_once(once, Load);
    const auto it = g_parts.find(kind);
    float mass = 0.0f;
    if (it != g_parts.end()) {
        for (const Part& p : it->second) {
            mass += p.volume;
        }
    }
    return mass;
}

bool Step(const int kinds[2], const float* const bones[2], const float* const roots[2], float dt, Result& out) {
    out = {};
    const float mass[2]{Mass(kinds[0]), Mass(kinds[1])};
    if (mass[0] <= 0.0f || mass[1] <= 0.0f || dt <= 0.0f) {
        return false;
    }
    // Each part where its bone has it now: its points, centre, box and velocity.
    struct Placed {
        std::vector<Vec3> points;
        Vec3 center{}, low{}, high{}, velocity{};
        int bone = 0;
        float volume = 0.0f;
    };
    std::vector<Placed> placed[2];
    float gyration[2]{}; // mean squared distance of its points from its root (inertia = mass x this)
    for (int k = 0; k < 2; k++) {
        const auto& parts = g_parts[kinds[k]];
        Body& body = g_body[k];
        if (body.kind != kinds[k] || body.before.size() != parts.size()) {
            body.kind = kinds[k], body.before.clear();
            g_touched.clear();
        }
        size_t points = 0;
        for (const Part& part : parts) {
            const float* m = bones[k] + part.bone * 16;
            Placed p;
            p.bone = part.bone, p.volume = part.volume;
            p.low = {1e30f, 1e30f, 1e30f}, p.high = {-1e30f, -1e30f, -1e30f};
            for (const Vec3& l : part.local) {
                const Vec3 w{l[0] * m[0] + l[1] * m[4] + l[2] * m[8] + m[12], l[0] * m[1] + l[1] * m[5] + l[2] * m[9] + m[13],
                             l[0] * m[2] + l[1] * m[6] + l[2] * m[10] + m[14]};
                p.points.push_back(w);
                p.center = p.center + w;
                for (int c = 0; c < 3; c++) {
                    p.low[c] = std::min(p.low[c], w[c]), p.high[c] = std::max(p.high[c], w[c]);
                }
                const Vec3 r{w[0] - roots[k][0], w[1] - roots[k][1], w[2] - roots[k][2]};
                gyration[k] += Dot(r, r);
            }
            points += part.local.size();
            p.center = p.center * (1.0f / float(part.local.size()));
            placed[k].push_back(std::move(p));
        }
        gyration[k] /= float(std::max<size_t>(points, 1));
        for (size_t i = 0; i < placed[k].size(); i++) {
            if (body.before.size() == placed[k].size()) {
                Vec3 v = (placed[k][i].center - body.before[i]) * (1.0f / dt);
                if (Length(v) > 450.0f) {
                    v = {}; // (a jump of the pose between motions, not a motion: no blow in it; the fastest
                            // real blow measured 218)
                }
                placed[k][i].velocity = v;
            }
        }
        body.before.clear();
        for (const Placed& p : placed[k]) {
            body.before.push_back(p.center);
        }
        {
            std::scoped_lock guard{g_shapes_lock};
            g_shapes[k].clear();
            for (const Placed& p : placed[k]) {
                g_shapes[k].push_back(p.points);
            }
        }
    }
    // Where the two meet: every pair of parts whose boxes overlap, then their hulls.
    const float share = 1.0f / mass[0] + 1.0f / mass[1];
    constexpr float bounce = 0.15f;
    // The striker's side of a meeting is the part that struck and the limb behind it (driven by
    // its muscles, not the whole body); the struck side resists with its whole body as felt at
    // the point (its mass and its turning about its root).
    constexpr float limb = 4.0f;
    const auto felt = [&](int k, const Vec3& point, const Vec3& n) {
        const Vec3 r{point[0] - roots[k][0], point[1] - roots[k][1], point[2] - roots[k][2]};
        const Vec3 arm = Cross(r, n);
        return 1.0f / (1.0f / mass[k] + Dot(arm, arm) / (mass[k] * std::max(gyration[k], 1.0f)));
    };
    float deepest = 0.0f, strongest = 0.0f, deep_speed = 0.0f;
    int strong_striker = 0;
    Vec3 deep_normal{}, strong_impulse{}, strong_point{};
    for (size_t i = 0; i < placed[0].size(); i++) {
        const Placed& a = placed[0][i];
        for (size_t j = 0; j < placed[1].size(); j++) {
            const Placed& b = placed[1][j];
            float apart = 0.0f;
            for (int c = 0; c < 3; c++) {
                apart = std::max({apart, b.low[c] - a.high[c], a.low[c] - b.high[c]});
            }
            out.gap = std::min(out.gap, apart);
            if (apart > 0.0f) {
                continue;
            }
            Vec3 n{};
            float depth = 0.0f;
            if (!Overlap(a.points, b.points, n, depth)) {
                continue;
            }
            const Vec3 relative = a.velocity - b.velocity;
            const float closing = Dot(relative, n); // > 0: a moving into b along n
            const auto [touch, met] = g_touched.try_emplace({int(i), int(j)}, g_step);
            Hit hit;
            hit.first = met || touch->second + 1 != g_step;
            touch->second = g_step;
            hit.closing = std::max(closing, 0.0f);
            hit.depth = depth;
            const float into_a = Dot(a.velocity, n), into_b = -Dot(b.velocity, n);
            hit.striker = into_a >= into_b ? 0 : 1;
            hit.bone = hit.striker ? b.bone : a.bone;
            hit.struck_bone = hit.striker ? a.bone : b.bone;
            hit.normal = hit.striker ? -n : n;
            hit.point = (Far(a.points, n) + Far(b.points, -n)) * 0.5f;
            const float striking = limb * (hit.striker ? b.volume : a.volume);
            hit.impulse = hit.closing * (1.0f + bounce) / (1.0f / striking + 1.0f / felt(1 - hit.striker, hit.point, n));
            out.hits.push_back(hit);
            if (depth > deepest) {
                deepest = depth, deep_normal = n, deep_speed = Length(relative);
            }
            if (hit.impulse > strongest) {
                strongest = hit.impulse, strong_striker = hit.striker;
                strong_impulse = hit.normal * hit.impulse, strong_point = hit.point;
            }
        }
    }
    // Out of each other (half the depth a step, by inverse mass), on the ground plane: bodies
    // pressed together firmly (up to 1.2 a step), a part swung into the other softly (0.3: a blade
    // swung deep into it is driven by its arm, not to be thrown out with the whole body).
    // (Only deep: a shallow touch is taken by the limbs bending out; moving a whole standing body for
    // it read as a glide.)
    if (deepest > 2.0f) {
        const Vec3 flat = Vec3{deep_normal[0], 0.0f, deep_normal[2]};
        // (Deeper than 3: no longer a blade's edge in it but a body inside the other, out firmly.)
        const float out_by = std::min(deepest * 0.5f, deepest > 3.0f ? 0.6f : deep_speed > 30.0f ? 0.2f : 0.4f);
        out.shift[1] = flat * (out_by * (1.0f / mass[1]) / share);
        out.shift[0] = flat * (-out_by * (1.0f / mass[0]) / share);
    }
    // The strongest meeting's impulse: the struck pushed along it, the striker back (through its
    // arm), each turned about its root. The striker's feet are planted and braced for its own
    // blow: the ground takes most of its recoil (it keeps 15% of the push, 30% of the turn).
    if (strongest > 0.0f) {
        for (int k = 0; k < 2; k++) {
            const bool striker = k == strong_striker;
            const Vec3 j = striker ? -strong_impulse : strong_impulse;
            out.kick[k] = Vec3{j[0], 0.0f, j[2]} * ((striker ? 0.15f : 1.0f) / mass[k]);
            if (const float speed = Length(out.kick[k]); speed > 20.0f) {
                out.kick[k] = out.kick[k] * (20.0f / speed); // (feet on the ground: a shove of a few units, never a glide)
            }
            const Vec3 r{strong_point[0] - roots[k][0], strong_point[1] - roots[k][1], strong_point[2] - roots[k][2]};
            out.spin[k] = Cross(r, j) * ((striker ? 0.3f : 1.0f) / (mass[k] * std::max(gyration[k], 1.0f)));
        }
    }
    g_step++;
    return true;
}

} // namespace Sotc::CvcPhysics
