/**
 * Auto Decorator  (developer.autodecorator)
 * Geode v5 / Geometry Dash 2.2081  -  Windows (x64) + Android (armv7/arm64)
 *
 * Design overview
 * ---------------
 *  - Analyzer   : reads LevelEditorLayer::getAllObjects(), classifies objects by m_objectID
 *                 (+ GameObjectType fallback) into blocks / spikes / portals / pads / orbs and
 *                 builds a 30-unit occupancy grid.
 *  - Generator  : turns the grid into a Plan = list of abstract Items (position, scale, rotation,
 *                 role) + colour triggers.  Deterministic (seeded), so preview == apply.
 *  - Preview    : the Plan is drawn into a CCDrawNode overlay.  Nothing is ever added to the level,
 *                 so it cannot be saved.
 *  - Apply      : the Plan is serialised to GD object strings and created in small chunks per frame
 *                 through LevelEditorLayer::createObjectsFromString (no undo entries).  Every created
 *                 object is tracked in a registry (Ref<GameObject>) owned by the Session node.
 *  - Restore    : removes the registry objects AND any leftover tagged objects from earlier sessions
 *                 (blocks/triggers whose zOrder lies in a reserved +-4000..4099 band).
 *
 * Injected decoration objects are plain block (ID 1) objects with "No Touch" (key 121) enabled,
 * scaled / rotated into bars and diamonds, so they never affect gameplay.
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace autodeco {

// ============================================================================
//  Constants
// ============================================================================

constexpr float kCell = 30.f;               // one editor grid square
constexpr int kChannelCount = 7;            // consecutive colour channels used
constexpr int kMaxRun = 12;                 // max cells per merged edge strip (avoids culling pop-in)
constexpr size_t kMaxCells = 400000;        // analysis safety valve (RAM)

// Reserved zOrder bands that identify injected objects (also after save + reload).
constexpr int kBackTagLo = -4099;
constexpr int kBackTagHi = -4000;
constexpr int kFrontTagLo = 4000;
constexpr int kFrontTagHi = 4099;

constexpr char const* kSessionID = "developer.autodecorator/session";

#ifdef GEODE_IS_ANDROID
constexpr size_t kApplyChunk = 120;         // objects created per frame
constexpr size_t kRemoveChunk = 240;        // objects removed per frame
constexpr size_t kPlatformCap = 3500;       // hard cap on constrained devices
#else
constexpr size_t kApplyChunk = 300;
constexpr size_t kRemoveChunk = 600;
constexpr size_t kPlatformCap = 100000;
#endif

// ============================================================================
//  Small utilities
// ============================================================================

inline ccColor3B rgb(uint32_t h) {
    return ccColor3B{
        static_cast<GLubyte>((h >> 16) & 0xFF),
        static_cast<GLubyte>((h >> 8) & 0xFF),
        static_cast<GLubyte>(h & 0xFF)
    };
}

/// Tiny deterministic PRNG (splitmix64): identical output on MSVC and libc++.
class Rng {
public:
    explicit Rng(uint64_t seed) : m_state(seed ^ 0x9E3779B97F4A7C15ULL) {}

    uint64_t next() {
        m_state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    float unit() { return static_cast<float>(next() >> 40) / 16777216.f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
    bool chance(float p) { return unit() < p; }

private:
    uint64_t m_state;
};

inline uint64_t mixSeed(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

inline uint32_t hash2(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline int cellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }

inline uint64_t cellKey(int gx, int gy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) | static_cast<uint32_t>(gy);
}

// ============================================================================
//  Style presets
// ============================================================================

enum class Motif { Ember, Radiant, Candy, Frost, Bolt, Rivet, Circuit };
enum class Skyline { Pillars, Diamonds, Slabs };

struct Palette {
    ccColor3B bg, ground, line, farColor, detail, edge, accent, glow, mote;
};

struct StyleDef {
    char const* name;
    char const* blurb;
    Palette pal;
    Motif motif;
    Skyline skyline;
    float edgeThickness;   // fraction of one grid cell
    float cornerChance;    // probability of a sparkle on a convex corner
    float moteDensity;     // air motes per 100 cells of open area
    float glowOpacity;
    float haloOpacity;
    float farOpacity;
    int skylineSpacing;    // cells between skyline shapes
};

inline std::vector<StyleDef> const& allStyles() {
    static const std::vector<StyleDef> styles = {
        StyleDef{"Acharon", "Ember-red forge with rising sparks",
            Palette{rgb(0x0d0203), rgb(0x1a0508), rgb(0xff2e4d), rgb(0x240509), rgb(0x4a0f1c),
                    rgb(0xff2e4d), rgb(0xff9a3d), rgb(0xff5a2e), rgb(0xffc27a)},
            Motif::Ember, Skyline::Pillars, 0.14f, 0.35f, 2.2f, 0.30f, 0.24f, 0.90f, 9},
        StyleDef{"Moonlight", "Silver stars over deep navy",
            Palette{rgb(0x060b1d), rgb(0x0f1a38), rgb(0x9fc4ff), rgb(0x101c3e), rgb(0x2a3f73),
                    rgb(0x9fc4ff), rgb(0xffffff), rgb(0xcfe3ff), rgb(0xffffff)},
            Motif::Radiant, Skyline::Diamonds, 0.12f, 0.40f, 2.6f, 0.28f, 0.22f, 0.85f, 10},
        StyleDef{"Candy", "Sugary pastels, stripes and polka dots",
            Palette{rgb(0x3a1650), rgb(0xff7ac8), rgb(0xfff1a8), rgb(0x7a2d8f), rgb(0xffb3e1),
                    rgb(0xffe27a), rgb(0x7af0ff), rgb(0xffc2ec), rgb(0xffffff)},
            Motif::Candy, Skyline::Diamonds, 0.18f, 0.45f, 2.0f, 0.30f, 0.25f, 0.70f, 8},
        StyleDef{"Polar", "Frozen cyan slabs with icicles",
            Palette{rgb(0x08233d), rgb(0x1d5a8a), rgb(0xbfe9ff), rgb(0x14507c), rgb(0x8fd0ff),
                    rgb(0xe6f7ff), rgb(0x7fd0ff), rgb(0xd9f2ff), rgb(0xffffff)},
            Motif::Frost, Skyline::Slabs, 0.16f, 0.30f, 2.8f, 0.30f, 0.24f, 0.75f, 9},
        StyleDef{"Voltage", "Electric yellow bolts on violet",
            Palette{rgb(0x090516), rgb(0x1c1040), rgb(0xffe600), rgb(0x1b0f3d), rgb(0x3d2a8c),
                    rgb(0xffe600), rgb(0x00f0ff), rgb(0xfff27a), rgb(0x00f0ff)},
            Motif::Bolt, Skyline::Pillars, 0.13f, 0.40f, 2.0f, 0.32f, 0.26f, 0.85f, 8},
        StyleDef{"Charcoal", "Riveted industrial greys",
            Palette{rgb(0x0a0a0d), rgb(0x17171c), rgb(0x9a9aa6), rgb(0x15151a), rgb(0x3a3a44),
                    rgb(0x8a8a96), rgb(0xd0d0da), rgb(0xc9c9d1), rgb(0xffffff)},
            Motif::Rivet, Skyline::Slabs, 0.10f, 0.25f, 1.2f, 0.18f, 0.15f, 0.90f, 10},
        StyleDef{"Circuit", "Green PCB traces and data nodes",
            Palette{rgb(0x010a07), rgb(0x04281c), rgb(0x00ff9c), rgb(0x04160f), rgb(0x0b5c40),
                    rgb(0x00ff9c), rgb(0x00e5ff), rgb(0x7dffc9), rgb(0x00e5ff)},
            Motif::Circuit, Skyline::Pillars, 0.11f, 0.30f, 1.8f, 0.26f, 0.22f, 0.90f, 7},
        StyleDef{"Solar", "Molten gold sunburst",
            Palette{rgb(0x1a0800), rgb(0x5a1f00), rgb(0xffb300), rgb(0x3a1400), rgb(0xa64a00),
                    rgb(0xffb300), rgb(0xfff6c0), rgb(0xffe082), rgb(0xfff6c0)},
            Motif::Radiant, Skyline::Diamonds, 0.15f, 0.40f, 2.4f, 0.34f, 0.28f, 0.80f, 9},
    };
    return styles;
}

// ============================================================================
//  Plan data model
// ============================================================================

enum class Role : uint8_t { Far, Halo, Glow, Mote, Detail, Edge, Accent };

struct RoleInfo {
    int zLayer;        // GD z-layer value (B4=-3, B3=-1, B1=3)
    int zOrder;        // inside the reserved tag bands
    int channelOffset; // 0..6 from the configured channel base
};

/// Back roles sit behind gameplay blocks; front roles sit above blocks but below the player (B1).
inline RoleInfo roleInfo(Role r) {
    switch (r) {
        case Role::Far:    return {-3, -4090, 0};
        case Role::Glow:   return {-1, -4050, 1};
        case Role::Edge:   return { 3,  4040, 2};
        case Role::Accent: return { 3,  4050, 3};
        case Role::Detail: return { 3,  4030, 4};
        case Role::Mote:   return {-1, -4030, 5};
        case Role::Halo:   return {-1, -4060, 6};
    }
    return {-1, -4050, 1};
}

struct ChannelDef {
    int offset;
    ccColor3B color;
    float opacity;
    bool blend;
};

inline std::array<ChannelDef, kChannelCount> channelDefs(StyleDef const& st) {
    return {{
        {0, st.pal.farColor,    st.farOpacity,  false},
        {1, st.pal.glow,   st.glowOpacity, true},
        {2, st.pal.edge,   1.0f,           false},
        {3, st.pal.accent, 1.0f,           false},
        {4, st.pal.detail, 0.85f,          false},
        {5, st.pal.mote,   0.90f,          true},
        {6, st.pal.glow,   st.haloOpacity, true},
    }};
}

struct Item {
    float x, y;      // world position
    float sx, sy;    // scale relative to a 30x30 block
    float rot;       // degrees, clockwise (GD convention)
    Role role;
};

struct Trigger {
    float x, y;
    int channel;     // absolute colour channel id
    ccColor3B color;
    float duration;
    float opacity;
    bool blend;
};

struct Plan {
    std::vector<Item> items;
    std::vector<Trigger> triggers;
    int channelBase = 900;
    size_t layoutObjects = 0, blocks = 0, hazards = 0, portals = 0, pads = 0, orbs = 0;
    bool trimmed = false;
    size_t total() const { return items.size() + triggers.size(); }
};

struct BuildLimits {
    size_t maxObjects = 6000;
    int channelBase = 900;
};

inline BuildLimits readLimits() {
    BuildLimits lim;
    auto* mod = Mod::get();
    int64_t maxObj = mod->getSettingValue<int64_t>("max-objects");
    int64_t base = mod->getSettingValue<int64_t>("channel-base");
    lim.maxObjects = static_cast<size_t>(std::clamp<int64_t>(maxObj, 200, 40000));
    lim.channelBase = static_cast<int>(std::clamp<int64_t>(base, 1, 990));
    lim.maxObjects = std::min(lim.maxObjects, kPlatformCap);
    return lim;
}

inline void add(Plan& plan, float x, float y, float sx, float sy, float rot, Role role) {
    plan.items.push_back(Item{x, y, sx, sy, rot, role});
}

// ============================================================================
//  Layout analysis
// ============================================================================

enum class Kind { None, Block, Spike, Portal, Pad, Orb };

struct PointObj { float x, y, sx, sy; };

struct Layout {
    std::unordered_set<uint64_t> solid;
    std::vector<std::pair<int, int>> cells;   // unique solid cells, sorted
    std::vector<PointObj> spikes, portals, pads, orbs;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    size_t analysed = 0;

    bool usable() const { return analysed > 0 && maxX >= minX; }
    bool has(int gx, int gy) const { return solid.find(cellKey(gx, gy)) != solid.end(); }
    void grow(float x, float y) {
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
};

inline Kind classifyById(int id) {
    static const std::unordered_set<int> spikes{8, 9, 39, 103, 392};
    static const std::unordered_set<int> portals{10, 11, 12, 13, 45, 46, 47, 99, 101, 111, 200, 201,
                                                 202, 203, 286, 287, 660, 745, 747, 1331, 1334, 1933, 2926};
    static const std::unordered_set<int> pads{35, 67, 140, 1332, 3005};
    static const std::unordered_set<int> orbs{36, 84, 141, 1022, 1330, 1333, 1704, 1751, 3004};
    if (spikes.count(id)) return Kind::Spike;
    if (portals.count(id)) return Kind::Portal;
    if (pads.count(id)) return Kind::Pad;
    if (orbs.count(id)) return Kind::Orb;
    return Kind::None;
}

/// Object-ID tables first (explicit), GameObjectType numeric fallback second (covers every skin/variant).
inline Kind classify(GameObject* obj) {
    Kind k = classifyById(obj->m_objectID);
    if (k != Kind::None) return k;
    switch (static_cast<int>(obj->m_objectType)) {
        case 0: case 25:
            return Kind::Block;                                   // Solid, Slope
        case 2:
            return Kind::Spike;                                   // Hazard
        case 3: case 4: case 5: case 6: case 14: case 15: case 16: case 17: case 18: case 19:
        case 23: case 24: case 26: case 27: case 28: case 33: case 41: case 42:
            return Kind::Portal;
        case 8: case 9: case 10: case 34: case 44:
            return Kind::Pad;
        case 11: case 12: case 13: case 29: case 32: case 35: case 36: case 37: case 38: case 43:
            return Kind::Orb;
        default:
            return Kind::None;
    }
}

/// True for objects created by this mod (survives save + reload).
inline bool isTagged(GameObject* obj) {
    int id = obj->m_objectID;
    if (id != 1 && id != 899) return false;
    int z = obj->m_zOrder;
    return (z >= kFrontTagLo && z <= kFrontTagHi) || (z >= kBackTagLo && z <= kBackTagHi);
}

inline void markBlock(Layout& L, float x, float y, float sx, float sy) {
    if (L.cells.size() >= kMaxCells) return;
    int gx0 = cellOf(x - 15.f * sx + 0.5f), gx1 = cellOf(x + 15.f * sx - 0.5f);
    int gy0 = cellOf(y - 15.f * sy + 0.5f), gy1 = cellOf(y + 15.f * sy - 0.5f);
    gx1 = std::clamp(gx1, gx0, gx0 + 15);
    gy1 = std::clamp(gy1, gy0, gy0 + 15);
    for (int gx = gx0; gx <= gx1; ++gx) {
        for (int gy = gy0; gy <= gy1; ++gy) {
            if (L.solid.insert(cellKey(gx, gy)).second) L.cells.emplace_back(gx, gy);
        }
    }
    L.grow(x - 15.f * sx, y - 15.f * sy);
    L.grow(x + 15.f * sx, y + 15.f * sy);
}

inline Layout analyze(LevelEditorLayer* editor, std::unordered_set<GameObject*> const* skip) {
    Layout L;
    if (!editor) return L;
    CCArray* all = editor->getAllObjects();
    if (!all) return L;

    for (auto* obj : CCArrayExt<GameObject*>(all)) {
        if (!obj) continue;
        if (skip && skip->count(obj)) continue;   // registry objects of this session
        if (isTagged(obj)) continue;              // injected in an earlier session
        Kind k = classify(obj);
        if (k == Kind::None) continue;

        CCPoint p = obj->getPosition();
        float sx = std::fabs(obj->getScaleX());
        float sy = std::fabs(obj->getScaleY());
        if (sx < 0.01f) sx = 1.f;
        if (sy < 0.01f) sy = 1.f;

        switch (k) {
            case Kind::Block:
                if (std::max(sx, sy) < 0.45f) continue;   // tiny scaled blocks are detail, not layout
                markBlock(L, p.x, p.y, sx, sy);
                break;
            case Kind::Spike:  L.spikes.push_back({p.x, p.y, sx, sy});  L.grow(p.x, p.y); break;
            case Kind::Portal: L.portals.push_back({p.x, p.y, sx, sy}); L.grow(p.x, p.y); break;
            case Kind::Pad:    L.pads.push_back({p.x, p.y, sx, sy});    L.grow(p.x, p.y); break;
            case Kind::Orb:    L.orbs.push_back({p.x, p.y, sx, sy});    L.grow(p.x, p.y); break;
            default: break;
        }
        ++L.analysed;
    }
    std::sort(L.cells.begin(), L.cells.end());
    return L;
}

// ============================================================================
//  Decoration generator
// ============================================================================

template <class Fn>
void forEachRun(std::unordered_map<int, std::vector<int>>& m, Fn&& fn) {
    std::vector<int> keys;
    keys.reserve(m.size());
    for (auto const& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (int key : keys) {
        auto& v = m[key];
        std::sort(v.begin(), v.end());
        size_t i = 0;
        while (i < v.size()) {
            size_t j = i;
            while (j + 1 < v.size() && v[j + 1] == v[j] + 1) ++j;
            int start = v[i];
            int len = static_cast<int>(j - i + 1);
            while (len > 0) {
                int n = std::min(len, kMaxRun);
                fn(key, start, n);
                start += n;
                len -= n;
            }
            i = j + 1;
        }
    }
}

/// Merged edge lighting + outer glow along every exposed block face.
inline void addEdges(Layout const& L, StyleDef const& st, Rng& rng, Plan& plan) {
    std::unordered_map<int, std::vector<int>> top, bottom, left, right;
    for (auto const& c : L.cells) {
        int gx = c.first, gy = c.second;
        if (!L.has(gx, gy + 1)) top[gy].push_back(gx);
        if (!L.has(gx, gy - 1)) bottom[gy].push_back(gx);
        if (!L.has(gx - 1, gy)) left[gx].push_back(gy);
        if (!L.has(gx + 1, gy)) right[gx].push_back(gy);
    }
    float t = st.edgeThickness;

    forEachRun(top, [&](int gy, int gx, int n) {
        float cx = gx * kCell + n * kCell * 0.5f;
        float edge = (gy + 1) * kCell;
        add(plan, cx, edge - t * kCell * 0.5f, static_cast<float>(n), t, 0.f, Role::Edge);
        add(plan, cx, edge + 3.f, static_cast<float>(n), 0.3f, 0.f, Role::Glow);
    });

    forEachRun(bottom, [&](int gy, int gx, int n) {
        float cx = gx * kCell + n * kCell * 0.5f;
        float edge = gy * kCell;
        add(plan, cx, edge + t * kCell * 0.5f, static_cast<float>(n), t, 0.f, Role::Edge);
        add(plan, cx, edge - 3.f, static_cast<float>(n), 0.3f, 0.f, Role::Glow);
        if (st.motif == Motif::Frost) {                     // icicles hanging from ceilings
            for (int i = 0; i < n; ++i) {
                if (!rng.chance(0.3f)) continue;
                float h = rng.range(0.25f, 0.55f);
                add(plan, (gx + i) * kCell + 15.f, edge - 15.f * h, 0.12f, h, 0.f, Role::Accent);
            }
        }
    });

    forEachRun(left, [&](int gx, int gy, int n) {
        float cy = gy * kCell + n * kCell * 0.5f;
        float edge = gx * kCell;
        add(plan, edge + t * kCell * 0.5f, cy, t, static_cast<float>(n), 0.f, Role::Edge);
        add(plan, edge - 3.f, cy, 0.3f, static_cast<float>(n), 0.f, Role::Glow);
    });

    forEachRun(right, [&](int gx, int gy, int n) {
        float cy = gy * kCell + n * kCell * 0.5f;
        float edge = (gx + 1) * kCell;
        add(plan, edge - t * kCell * 0.5f, cy, t, static_cast<float>(n), 0.f, Role::Edge);
        add(plan, edge + 3.f, cy, 0.3f, static_cast<float>(n), 0.f, Role::Glow);
    });
}

/// Sparkle diamonds on convex corners.
inline void addCorners(Layout const& L, StyleDef const& st, Rng& rng, Plan& plan) {
    if (st.cornerChance <= 0.f) return;
    for (auto const& c : L.cells) {
        int gx = c.first, gy = c.second;
        bool t = !L.has(gx, gy + 1), b = !L.has(gx, gy - 1);
        bool l = !L.has(gx - 1, gy), r = !L.has(gx + 1, gy);
        float x0 = gx * kCell, x1 = x0 + kCell, y0 = gy * kCell, y1 = y0 + kCell;
        if (t && l && rng.chance(st.cornerChance)) add(plan, x0, y1, 0.3f, 0.3f, 45.f, Role::Accent);
        if (t && r && rng.chance(st.cornerChance)) add(plan, x1, y1, 0.3f, 0.3f, 45.f, Role::Accent);
        if (b && l && rng.chance(st.cornerChance)) add(plan, x0, y0, 0.3f, 0.3f, 45.f, Role::Accent);
        if (b && r && rng.chance(st.cornerChance)) add(plan, x1, y0, 0.3f, 0.3f, 45.f, Role::Accent);
    }
}

/// Motif-specific surface detail on fully enclosed block cells.
inline void addInterior(Layout const& L, StyleDef const& st, Plan& plan) {
    for (auto const& c : L.cells) {
        int gx = c.first, gy = c.second;
        if (!(L.has(gx + 1, gy) && L.has(gx - 1, gy) && L.has(gx, gy + 1) && L.has(gx, gy - 1))) continue;
        uint32_t h = hash2(gx, gy);
        float cx = gx * kCell + 15.f, cy = gy * kCell + 15.f;
        switch (st.motif) {
            case Motif::Circuit:
                if (h % 5 == 0) {
                    bool horiz = ((h >> 8) & 1u) != 0;
                    add(plan, cx, cy, horiz ? 0.8f : 0.07f, horiz ? 0.07f : 0.8f, 0.f, Role::Detail);
                    add(plan, cx + (horiz ? 12.f : 0.f), cy + (horiz ? 0.f : 12.f), 0.2f, 0.2f, 45.f, Role::Accent);
                }
                break;
            case Motif::Rivet:
                if (((gx + gy) & 3) == 0 && (h % 3) != 0) add(plan, cx, cy, 0.2f, 0.2f, 0.f, Role::Detail);
                break;
            case Motif::Candy:
                if (h % 6 == 0) add(plan, cx, cy, 0.28f, 0.28f, 0.f, Role::Accent);
                else if (h % 6 == 1) add(plan, cx, cy, 1.1f, 0.12f, 45.f, Role::Detail);
                break;
            case Motif::Frost:
                if (h % 7 == 0) add(plan, cx, cy, 0.22f, 0.22f, 45.f, Role::Detail);
                break;
            case Motif::Bolt:
                if (h % 9 == 0) {
                    add(plan, cx - 3.f, cy + 5.f, 0.6f, 0.08f, 35.f, Role::Detail);
                    add(plan, cx + 3.f, cy - 5.f, 0.6f, 0.08f, -35.f, Role::Detail);
                }
                break;
            case Motif::Ember:
                if (h % 8 == 0) add(plan, cx, cy, 0.16f, 0.16f, 0.f, Role::Accent);
                break;
            case Motif::Radiant:
                if (h % 10 == 0) {
                    add(plan, cx, cy, 0.5f, 0.07f, 0.f, Role::Accent);
                    add(plan, cx, cy, 0.07f, 0.5f, 0.f, Role::Accent);
                }
                break;
        }
    }
}

/// Soft glows around hazards, pads, orbs and portals (stacked blending shapes brighten toward the centre).
inline void addGameplayAccents(Layout const& L, Plan& plan) {
    for (auto const& s : L.spikes) {
        add(plan, s.x, s.y + 3.f, 1.7f * s.sx, 1.7f * s.sy, 45.f, Role::Halo);
        add(plan, s.x, s.y + 3.f, 1.0f * s.sx, 1.0f * s.sy, 45.f, Role::Halo);
    }
    for (auto const& p : L.pads) {
        add(plan, p.x, p.y - 6.f, 1.6f, 0.4f, 0.f, Role::Halo);
        add(plan, p.x, p.y - 6.f, 0.9f, 0.18f, 0.f, Role::Glow);
    }
    for (auto const& o : L.orbs) {
        add(plan, o.x, o.y, 2.4f, 2.4f, 45.f, Role::Halo);
        add(plan, o.x, o.y, 1.6f, 1.6f, 45.f, Role::Halo);
        add(plan, o.x, o.y, 0.9f, 0.9f, 45.f, Role::Halo);
        add(plan, o.x - 26.f, o.y, 0.18f, 0.18f, 45.f, Role::Accent);
        add(plan, o.x + 26.f, o.y, 0.18f, 0.18f, 45.f, Role::Accent);
        add(plan, o.x, o.y + 26.f, 0.18f, 0.18f, 45.f, Role::Accent);
        add(plan, o.x, o.y - 26.f, 0.18f, 0.18f, 45.f, Role::Accent);
    }
    for (auto const& p : L.portals) {
        add(plan, p.x, p.y, 0.9f, 3.5f, 0.f, Role::Halo);
        add(plan, p.x - 20.f, p.y, 0.07f, 3.1f, 0.f, Role::Edge);
        add(plan, p.x + 20.f, p.y, 0.07f, 3.1f, 0.f, Role::Edge);
        add(plan, p.x, p.y + 50.f, 0.3f, 0.3f, 45.f, Role::Accent);
        add(plan, p.x, p.y - 50.f, 0.3f, 0.3f, 45.f, Role::Accent);
    }
}

/// Atmospheric air deco: seeded random motes in free space around the layout.
inline void addMotes(Layout const& L, StyleDef const& st, Rng& rng, Plan& plan) {
    float x0 = L.minX - 60.f, x1 = L.maxX + 120.f;
    float y0 = std::max(L.minY - 60.f, -30.f), y1 = L.maxY + 210.f;
    float cells = ((x1 - x0) / kCell) * ((y1 - y0) / kCell);
    int n = static_cast<int>(std::clamp(cells * st.moteDensity / 100.f, 24.f, 1500.f));
    for (int i = 0; i < n; ++i) {
        float x = 0.f, y = 0.f;
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
            x = rng.range(x0, x1);
            y = rng.range(y0, y1);
            ok = !L.has(cellOf(x), cellOf(y));
        }
        if (!ok) continue;
        float s = rng.range(0.12f, 0.30f);
        switch (st.motif) {
            case Motif::Ember:
                add(plan, x, y, 0.09f, rng.range(0.2f, 0.45f), rng.range(-12.f, 12.f), Role::Mote);
                break;
            case Motif::Radiant:
            case Motif::Frost:
                add(plan, x, y, s * 2.2f, 0.06f, 0.f, Role::Mote);
                add(plan, x, y, 0.06f, s * 2.2f, 0.f, Role::Mote);
                break;
            default:
                add(plan, x, y, s, s, 45.f, Role::Mote);
                break;
        }
    }
}

/// Large far-background silhouettes (B4 layer).
inline void addSkyline(Layout const& L, StyleDef const& st, Rng& rng, Plan& plan) {
    float yBase = std::min(L.minY, 15.f) - 15.f;
    float x = L.minX - 120.f;
    float xEnd = L.maxX + 240.f;
    int guard = 0;
    while (x < xEnd && guard++ < 4000) {
        switch (st.skyline) {
            case Skyline::Pillars: {
                float w = rng.range(1.2f, 3.2f), h = rng.range(4.f, 12.f);
                add(plan, x, yBase + h * 15.f, w, h, 0.f, Role::Far);
                break;
            }
            case Skyline::Diamonds: {
                float s = rng.range(2.5f, 6.5f);
                add(plan, x, yBase + rng.range(150.f, 420.f), s, s, 45.f, Role::Far);
                break;
            }
            case Skyline::Slabs: {
                float w = rng.range(3.f, 8.f), h = rng.range(1.2f, 4.5f);
                add(plan, x, yBase + h * 15.f, w, h, 0.f, Role::Far);
                break;
            }
        }
        x += st.skylineSpacing * kCell * rng.range(0.7f, 1.4f);
    }
}

/// Colour triggers: theme palette, optional BG/ground swap, optional twinkle on the mote channel.
inline void addTriggers(Layout const& L, StyleDef const& st, bool bgSwap, bool particles, Plan& plan) {
    float tx = std::min(15.f, L.minX - 15.f);
    float ty = L.maxY + 150.f;
    int row = 0;
    auto defs = channelDefs(st);
    for (auto const& d : defs) {
        plan.triggers.push_back(Trigger{tx, ty + 30.f * row++, plan.channelBase + d.offset,
                                        d.color, 0.f, d.opacity, d.blend});
    }
    if (bgSwap) {
        // 1000 = BG, 1001 = Ground 1, 1009 = Ground 2, 1002 = Line
        plan.triggers.push_back(Trigger{tx, ty + 30.f * row++, 1000, st.pal.bg,     0.f, 1.f, false});
        plan.triggers.push_back(Trigger{tx, ty + 30.f * row++, 1001, st.pal.ground, 0.f, 1.f, false});
        plan.triggers.push_back(Trigger{tx, ty + 30.f * row++, 1009, st.pal.ground, 0.f, 1.f, false});
        plan.triggers.push_back(Trigger{tx, ty + 30.f * row++, 1002, st.pal.line,   0.f, 1.f, false});
    }
    if (particles) {
        bool dim = true;
        int count = 0;
        for (float x = L.minX + 120.f; x < L.maxX && count < 300; x += 150.f, ++count) {
            plan.triggers.push_back(Trigger{x, ty - 45.f, plan.channelBase + 5, st.pal.mote,
                                            0.6f, dim ? 0.25f : 0.95f, true});
            dim = !dim;
        }
    }
}

/// Removes `removeCount` items of `role`, evenly spread (so thinning never leaves holes).
inline void thinRole(std::vector<Item>& items, Role role, size_t removeCount) {
    size_t total = 0;
    for (auto const& it : items) if (it.role == role) ++total;
    if (total == 0 || removeCount == 0) return;
    size_t keep = removeCount >= total ? 0 : total - removeCount;
    std::vector<Item> out;
    out.reserve(items.size() - std::min(removeCount, total));
    size_t seen = 0;
    for (auto const& it : items) {
        if (it.role != role) { out.push_back(it); continue; }
        size_t before = (seen * keep) / total;
        size_t after = ((seen + 1) * keep) / total;
        if (after > before) out.push_back(it);
        ++seen;
    }
    items.swap(out);
}

/// Enforces the object budget. Lowest-value decoration goes first.
inline void trimToBudget(Plan& plan, size_t cap) {
    if (plan.total() <= cap) return;
    plan.trimmed = true;
    size_t maxTriggers = std::max<size_t>(cap / 3, 16);   // palette/BG triggers come first and always survive
    if (plan.triggers.size() > maxTriggers) plan.triggers.resize(maxTriggers);
    size_t allowed = cap > plan.triggers.size() ? cap - plan.triggers.size() : 0;
    static constexpr std::array<Role, 7> dropOrder = {
        Role::Mote, Role::Detail, Role::Far, Role::Accent, Role::Halo, Role::Glow, Role::Edge};
    for (Role r : dropOrder) {
        if (plan.items.size() <= allowed) break;
        thinRole(plan.items, r, plan.items.size() - allowed);
    }
}

inline Plan buildPlan(LevelEditorLayer* editor, std::unordered_set<GameObject*> const* skip,
                      size_t styleIdx, bool bgSwap, bool particles, BuildLimits lim) {
    Plan plan;
    plan.channelBase = lim.channelBase;
    auto const& styles = allStyles();
    if (styleIdx >= styles.size()) styleIdx = 0;
    StyleDef const& st = styles[styleIdx];

    Layout L = analyze(editor, skip);
    plan.layoutObjects = L.analysed;
    plan.blocks = L.cells.size();
    plan.hazards = L.spikes.size();
    plan.portals = L.portals.size();
    plan.pads = L.pads.size();
    plan.orbs = L.orbs.size();
    if (!L.usable()) return plan;

    uint64_t seed = 0xA17D0C0DEULL;
    seed = mixSeed(seed, styleIdx);
    seed = mixSeed(seed, L.cells.size());
    seed = mixSeed(seed, L.spikes.size());
    seed = mixSeed(seed, static_cast<uint64_t>(static_cast<int64_t>(L.minX)));
    seed = mixSeed(seed, static_cast<uint64_t>(static_cast<int64_t>(L.maxX)));
    seed = mixSeed(seed, static_cast<uint64_t>(static_cast<int64_t>(L.maxY)));
    Rng rng(seed);

    addEdges(L, st, rng, plan);
    addCorners(L, st, rng, plan);
    addInterior(L, st, plan);
    addGameplayAccents(L, plan);
    if (particles) addMotes(L, st, rng, plan);
    addSkyline(L, st, rng, plan);
    addTriggers(L, st, bgSwap, particles, plan);

    trimToBudget(plan, lim.maxObjects);
    return plan;
}

// ============================================================================
//  Serialisation to GD object strings
//  keys: 1 id, 2 x, 3 y, 6 rot, 21/22 colour channels, 24 z layer, 25 z order, 96 no glow,
//        103 high detail, 121 no touch, 128/129 scale x/y; triggers: 7/8/9 rgb, 10 fade, 17 blending,
//        23 target channel, 35 opacity, 36 active
// ============================================================================

inline void appendItem(std::string& out, Item const& it, int channelBase) {
    RoleInfo info = roleInfo(it.role);
    int ch = channelBase + info.channelOffset;
    fmt::format_to(std::back_inserter(out),
        "1,1,2,{:.2f},3,{:.2f},6,{:.2f},21,{},22,{},24,{},25,{},128,{:.3f},129,{:.3f},121,1,103,1,96,1;",
        it.x, it.y, it.rot, ch, ch, info.zLayer, info.zOrder, it.sx, it.sy);
}

inline void appendTrigger(std::string& out, Trigger const& t) {
    fmt::format_to(std::back_inserter(out),
        "1,899,2,{:.2f},3,{:.2f},7,{},8,{},9,{},10,{:.2f},23,{},35,{:.2f},17,{},36,1,25,{};",
        t.x, t.y, static_cast<int>(t.color.r), static_cast<int>(t.color.g), static_cast<int>(t.color.b),
        t.duration, t.channel, t.opacity, t.blend ? 1 : 0, kFrontTagHi);
}

// ============================================================================
//  Session: registry + preview overlay + chunked jobs.
//  Child of the editor's object layer, so its lifetime == the editor's lifetime.
// ============================================================================

inline void drawItem(CCDrawNode* node, Item const& it, std::array<ChannelDef, kChannelCount> const& defs) {
    ChannelDef const& def = defs[static_cast<size_t>(roleInfo(it.role).channelOffset)];
    float alpha = std::clamp(def.opacity * (def.blend ? 0.85f : 1.f), 0.12f, 1.f);
    float hx = it.sx * 15.f, hy = it.sy * 15.f;
    float rad = it.rot * 0.0174532925f;
    float c = std::cos(rad), s = std::sin(rad);
    float lx[4] = {-hx, hx, hx, -hx};
    float ly[4] = {-hy, -hy, hy, hy};
    CCPoint pts[4];
    for (int i = 0; i < 4; ++i) {
        pts[i] = ccp(it.x + lx[i] * c + ly[i] * s, it.y - lx[i] * s + ly[i] * c);   // clockwise rotation
    }
    ccColor4F fill{def.color.r / 255.f, def.color.g / 255.f, def.color.b / 255.f, alpha};
    ccColor4F none{0.f, 0.f, 0.f, 0.f};
    node->drawPolygon(pts, 4, fill, 0.f, none);
}

class Session : public CCNode {
public:
    enum class Job { Idle, Restoring, Applying };

    static Session* obtain(LevelEditorLayer* editor) {
        if (!editor || !editor->m_objectLayer) return nullptr;
        if (auto* existing = editor->m_objectLayer->getChildByID(kSessionID)) {
            return static_cast<Session*>(existing);   // ID is unique to this mod
        }
        auto* s = new Session();
        if (s->initSession(editor)) {
            s->autorelease();
            s->setID(kSessionID);
            editor->m_objectLayer->addChild(s, 100000);
            return s;
        }
        delete s;
        return nullptr;
    }

    bool busy() const { return m_job != Job::Idle; }
    bool previewShown() const { return m_previewShown; }
    size_t ownedCount() const { return m_owned.size(); }
    std::unordered_set<GameObject*> const& ownedSet() const { return m_ownedSet; }

    // ---- preview (never touches the level) --------------------------------

    void showPreview(Plan const& plan, StyleDef const& style) {
        if (!m_draw) {
            m_draw = CCDrawNode::create();
            this->addChild(m_draw.data());
        }
        m_draw->clear();
        auto defs = channelDefs(style);
        for (auto const& it : plan.items) drawItem(m_draw.data(), it, defs);
        m_previewShown = true;
    }

    void clearPreview() {
        if (m_draw) m_draw->clear();
        m_previewShown = false;
    }

    // ---- jobs --------------------------------------------------------------

    /// Replaces any previous injected decoration with `plan`.
    void startApply(Plan plan, StyleDef const* style) {
        this->cancelJob();
        this->clearPreview();
        m_plan = std::move(plan);
        m_style = style;
        m_itemCursor = m_triggerCursor = 0;
        m_createdThisJob = m_removedThisJob = 0;
        m_applyAfterRestore = true;
        this->queueRemovals();
        m_job = m_removeQueue.empty() ? Job::Applying : Job::Restoring;
        this->schedule(schedule_selector(Session::tick));
    }

    void startRestore() {
        this->cancelJob();
        this->clearPreview();
        m_createdThisJob = m_removedThisJob = 0;
        m_applyAfterRestore = false;
        this->queueRemovals();
        if (m_removeQueue.empty()) {
            Notification::create("Nothing to restore: no injected decoration found.",
                                 NotificationIcon::Info)->show();
            return;
        }
        m_job = Job::Restoring;
        this->schedule(schedule_selector(Session::tick));
    }

    void cancelJob() {
        if (m_job != Job::Idle) this->unschedule(schedule_selector(Session::tick));
        m_job = Job::Idle;
        m_plan = Plan{};
        m_removeQueue.clear();
        m_removeCursor = 0;
        m_applyAfterRestore = false;
    }

private:
    Session() = default;

    bool initSession(LevelEditorLayer* editor) {
        if (!CCNode::init()) return false;
        m_editor = editor;
        return true;
    }

    /// Collects registry objects that are still alive in the editor + tagged leftovers from old sessions.
    void queueRemovals() {
        m_removeQueue.clear();
        m_removeCursor = 0;
        if (!m_editor) return;
        if (m_editor->m_editorUI) m_editor->m_editorUI->deselectAll();   // never leave dangling selections

        std::unordered_set<GameObject*> live;
        std::vector<Ref<GameObject>> tagged;
        if (CCArray* all = m_editor->getAllObjects()) {
            for (auto* obj : CCArrayExt<GameObject*>(all)) {
                if (!obj) continue;
                live.insert(obj);
                if (!m_ownedSet.count(obj) && isTagged(obj)) tagged.push_back(Ref<GameObject>(obj));
            }
        }
        for (auto const& ref : m_owned) {
            if (ref && live.count(ref.data())) m_removeQueue.push_back(ref);
        }
        for (auto const& ref : tagged) m_removeQueue.push_back(ref);
        m_owned.clear();
        m_ownedSet.clear();
    }

    void tick(float) {
        if (m_job == Job::Idle) {
            this->unschedule(schedule_selector(Session::tick));
            return;
        }
        auto* editor = LevelEditorLayer::get();
        if (!editor || editor != m_editor) {
            this->cancelJob();
            return;
        }
        try {
            if (m_job == Job::Restoring) this->stepRestore(editor);
            else this->stepApply(editor);
        } catch (std::exception const& e) {
            log::error("Auto Decorator job failed: {}", e.what());
            this->cancelJob();
            Notification::create("Auto Decorator stopped: out of memory or internal error.",
                                 NotificationIcon::Error)->show();
        }
    }

    void stepRestore(LevelEditorLayer* editor) {
        size_t n = 0;
        while (n < kRemoveChunk && m_removeCursor < m_removeQueue.size()) {
            Ref<GameObject> obj = std::move(m_removeQueue[m_removeCursor++]);
            if (obj) {
                editor->removeObject(obj.data(), true);   // true = no undo entry
                ++m_removedThisJob;
            }
            ++n;
        }
        if (m_removeCursor < m_removeQueue.size()) return;

        m_removeQueue.clear();
        m_removeCursor = 0;
        if (m_applyAfterRestore) {
            m_job = Job::Applying;
            return;
        }
        m_job = Job::Idle;
        this->unschedule(schedule_selector(Session::tick));
        Notification::create(fmt::format("Restored original layout: removed {} decoration objects.", m_removedThisJob),
                             NotificationIcon::Success)->show();
    }

    void stepApply(LevelEditorLayer* editor) {
        std::string buf;
        buf.reserve(kApplyChunk * 110);
        size_t produced = 0;
        while (produced < kApplyChunk && m_triggerCursor < m_plan.triggers.size()) {
            appendTrigger(buf, m_plan.triggers[m_triggerCursor++]);
            ++produced;
        }
        while (produced < kApplyChunk && m_itemCursor < m_plan.items.size()) {
            appendItem(buf, m_plan.items[m_itemCursor++], m_plan.channelBase);
            ++produced;
        }
        if (!buf.empty()) {
            // noUndo = true (decoration must not pollute the undo stack), noLimit = false (respect GD's object limit)
            if (CCArray* made = editor->createObjectsFromString(buf, true, false)) {
                for (auto* obj : CCArrayExt<GameObject*>(made)) {
                    if (!obj) continue;
                    if (m_ownedSet.insert(obj).second) m_owned.push_back(Ref<GameObject>(obj));
                    ++m_createdThisJob;
                }
            }
        }
        if (m_itemCursor < m_plan.items.size() || m_triggerCursor < m_plan.triggers.size()) return;

        bool trimmed = m_plan.trimmed;
        m_plan = Plan{};
        m_job = Job::Idle;
        this->unschedule(schedule_selector(Session::tick));
        if (m_createdThisJob == 0) {
            Notification::create("No decoration objects could be created (object limit reached?).",
                                 NotificationIcon::Warning)->show();
        } else {
            Notification::create(
                fmt::format("Decoration applied: {} objects{}.", m_createdThisJob,
                            trimmed ? " (trimmed to your object limit)" : ""),
                NotificationIcon::Success)->show();
        }
    }

    LevelEditorLayer* m_editor = nullptr;
    Ref<CCDrawNode> m_draw;
    bool m_previewShown = false;

    std::vector<Ref<GameObject>> m_owned;               // registry of injected objects
    std::unordered_set<GameObject*> m_ownedSet;

    Job m_job = Job::Idle;
    bool m_applyAfterRestore = false;
    Plan m_plan;
    StyleDef const* m_style = nullptr;
    size_t m_itemCursor = 0, m_triggerCursor = 0;
    size_t m_createdThisJob = 0, m_removedThisJob = 0;
    std::vector<Ref<GameObject>> m_removeQueue;
    size_t m_removeCursor = 0;
};

// ============================================================================
//  Popup UI
// ============================================================================

class AutoDecoPopup : public Popup {
public:
    static AutoDecoPopup* create() {
        auto* ret = new AutoDecoPopup();
        if (ret->initPopup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

private:
    enum ToggleTag { TagPreview = 1, TagBgSwap = 2, TagParticles = 3 };

    size_t m_style = 0;
    bool m_previewOn = false;
    bool m_bgSwap = false;
    bool m_particles = true;
    std::vector<CCPoint> m_cellOffsets;
    CCLayerColor* m_highlight = nullptr;
    CCLabelBMFont* m_infoLabel = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    std::array<CCLayerColor*, 6> m_swatches{};

    static size_t savedStyleIndex() {
        auto name = Mod::get()->getSavedValue<std::string>("style", "Acharon");
        auto const& styles = allStyles();
        for (size_t i = 0; i < styles.size(); ++i) {
            if (name == styles[i].name) return i;
        }
        return 0;
    }

    CCLabelBMFont* addLabel(char const* text, float scale, CCPoint offset, CCPoint anchor) {
        auto* label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(scale);
        label->setAnchorPoint(anchor);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, offset);
        return label;
    }

    CCMenuItemToggler* addToggle(char const* text, int tag, bool on, float y) {
        auto* toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AutoDecoPopup::onToggle), 0.6f);
        toggler->setTag(tag);
        toggler->toggle(on);
        m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, ccp(-185.f, y));
        this->addLabel(text, 0.42f, ccp(-165.f, y), ccp(0.f, 0.5f));
        return toggler;
    }

    bool initPopup() {
        if (!Popup::init(430.f, 300.f)) return false;
        this->setTitle("Auto Decorator");

        m_style = savedStyleIndex();
        m_bgSwap = Mod::get()->getSavedValue<bool>("bg-swap", false);
        m_particles = Mod::get()->getSavedValue<bool>("particles", true);
        if (auto* editor = LevelEditorLayer::get()) {
            if (auto* session = Session::obtain(editor)) m_previewOn = session->previewShown();
        }

        auto size = m_mainLayer->getContentSize();
        (void)size;

        // --- style highlight (behind the buttons) ---
        m_highlight = CCLayerColor::create(ccc4(120, 255, 140, 200), 92.f, 32.f);
        m_mainLayer->addChild(m_highlight, 1);

        // --- style grid: 4 x 2 ---
        auto const& styles = allStyles();
        for (size_t i = 0; i < styles.size(); ++i) {
            int col = static_cast<int>(i % 4), row = static_cast<int>(i / 4);
            CCPoint off = ccp(-144.f + 96.f * col, 96.f - 34.f * row);
            m_cellOffsets.push_back(off);
            auto* spr = ButtonSprite::create(styles[i].name, 88, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.5f);
            auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AutoDecoPopup::onStyle));
            btn->setTag(static_cast<int>(i));
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, off);
        }

        // --- selected style info + palette swatches ---
        m_infoLabel = this->addLabel("", 0.42f, ccp(0.f, 32.f), ccp(0.5f, 0.5f));
        for (size_t i = 0; i < m_swatches.size(); ++i) {
            auto* sw = CCLayerColor::create(ccc4(0, 0, 0, 255), 28.f, 14.f);
            sw->setPosition(ccp(size.width * 0.5f - 94.f + 32.f * static_cast<float>(i),
                                size.height * 0.5f + 6.f));
            m_mainLayer->addChild(sw, 2);
            m_swatches[i] = sw;
        }

        // --- toggles ---
        this->addToggle("Preview on Layout", TagPreview, m_previewOn, -16.f);
        this->addToggle("Auto Background Swap", TagBgSwap, m_bgSwap, -42.f);
        this->addToggle("Include Particles", TagParticles, m_particles, -68.f);

        m_statusLabel = this->addLabel("Preview appears in the editor once you close the pause menu.",
                                       0.34f, ccp(0.f, -92.f), ccp(0.5f, 0.5f));

        // --- actions ---
        auto* applySpr = ButtonSprite::create("Apply Decoration", 180, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.5f);
        auto* applyBtn = CCMenuItemSpriteExtra::create(applySpr, this, menu_selector(AutoDecoPopup::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Center, ccp(-105.f, -120.f));

        auto* restoreSpr = ButtonSprite::create("Restore Original Layout", 190, true, "bigFont.fnt", "GJ_button_06.png", 30.f, 0.4f);
        auto* restoreBtn = CCMenuItemSpriteExtra::create(restoreSpr, this, menu_selector(AutoDecoPopup::onRestore));
        m_buttonMenu->addChildAtPosition(restoreBtn, Anchor::Center, ccp(105.f, -120.f));

        this->refreshSelection();
        return true;
    }

    void placeHighlight() {
        if (!m_highlight || m_style >= m_cellOffsets.size()) return;
        auto size = m_mainLayer->getContentSize();
        auto hs = m_highlight->getContentSize();
        CCPoint off = m_cellOffsets[m_style];
        m_highlight->setPosition(ccp(size.width * 0.5f + off.x - hs.width * 0.5f,
                                     size.height * 0.5f + off.y - hs.height * 0.5f));
    }

    void refreshSelection() {
        auto const& st = allStyles()[m_style];
        if (m_infoLabel) {
            auto text = fmt::format("{}  -  {}", st.name, st.blurb);
            m_infoLabel->setString(text.c_str());
            m_infoLabel->limitLabelWidth(370.f, 0.42f, 0.2f);
        }
        ccColor3B colours[6] = {st.pal.bg, st.pal.farColor, st.pal.detail, st.pal.edge, st.pal.accent, st.pal.mote};
        for (size_t i = 0; i < m_swatches.size(); ++i) {
            if (m_swatches[i]) m_swatches[i]->setColor(colours[i]);
        }
        this->placeHighlight();
    }

    void setStatus(std::string const& text) {
        if (!m_statusLabel) return;
        m_statusLabel->setString(text.c_str());
        m_statusLabel->limitLabelWidth(380.f, 0.34f, 0.15f);
    }

    void refreshPreview() {
        auto* editor = LevelEditorLayer::get();
        auto* session = Session::obtain(editor);
        if (!editor || !session) return;
        if (!m_previewOn) {
            session->clearPreview();
            this->setStatus("Preview off.");
            return;
        }
        if (session->busy()) {
            this->setStatus("Busy applying/restoring - try again in a moment.");
            return;
        }
        auto const& st = allStyles()[m_style];
        Plan plan = buildPlan(editor, &session->ownedSet(), m_style, m_bgSwap, m_particles, readLimits());
        if (plan.items.empty()) {
            session->clearPreview();
            this->setStatus("No layout objects (blocks/spikes/portals/orbs) found to decorate.");
            return;
        }
        session->showPreview(plan, st);
        this->setStatus(fmt::format("Preview: {} objects{}. Close menus to look at it.", plan.total(),
                                    plan.trimmed ? " (trimmed to limit)" : ""));
    }

    // ---- callbacks ---------------------------------------------------------

    void onStyle(CCObject* sender) {
        size_t idx = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (idx >= allStyles().size()) return;
        m_style = idx;
        Mod::get()->setSavedValue<std::string>("style", allStyles()[idx].name);
        this->refreshSelection();
        if (m_previewOn) this->refreshPreview();
    }

    void onToggle(CCObject* sender) {
        auto* toggler = static_cast<CCMenuItemToggler*>(sender);
        bool on = !toggler->isToggled();   // GD reports the state *before* the click inside the callback
        switch (toggler->getTag()) {
            case TagPreview:
                m_previewOn = on;
                this->refreshPreview();
                break;
            case TagBgSwap:
                m_bgSwap = on;
                Mod::get()->setSavedValue<bool>("bg-swap", on);
                break;
            case TagParticles:
                m_particles = on;
                Mod::get()->setSavedValue<bool>("particles", on);
                if (m_previewOn) this->refreshPreview();
                break;
            default: break;
        }
    }

    void onApply(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        auto* session = Session::obtain(editor);
        if (!editor || !session) {
            FLAlertLayer::create("Auto Decorator", "Open a level in the editor first.", "OK")->show();
            return;
        }
        if (session->busy()) {
            Notification::create("Auto Decorator is still working...", NotificationIcon::Info)->show();
            return;
        }
        auto const& st = allStyles()[m_style];
        Plan plan = buildPlan(editor, &session->ownedSet(), m_style, m_bgSwap, m_particles, readLimits());
        if (plan.items.empty()) {
            FLAlertLayer::create("Auto Decorator",
                "No blocks, spikes, portals, pads or orbs were found, so there is nothing to decorate.",
                "OK")->show();
            return;
        }
        size_t total = plan.total();
        this->setStatus(fmt::format("Applying {} objects...", total));
        session->startApply(std::move(plan), &st);
        Notification::create(fmt::format("Applying {} decoration objects...", total),
                             NotificationIcon::Loading, 2.f)->show();
    }

    void onRestore(CCObject*) {
        auto* editor = LevelEditorLayer::get();
        auto* session = Session::obtain(editor);
        if (!editor || !session) {
            FLAlertLayer::create("Auto Decorator", "Open a level in the editor first.", "OK")->show();
            return;
        }
        session->startRestore();
        this->setStatus("Restoring original layout...");
    }
};

/// Finds the pause layer's busiest CCMenu so our button inherits its (correct) touch priority.
inline CCMenu* pickHostMenu(CCNode* root) {
    if (!root) return nullptr;
    CCArray* children = root->getChildren();
    if (!children) return nullptr;
    CCMenu* best = nullptr;
    unsigned bestCount = 0;
    for (auto* child : CCArrayExt<CCNode*>(children)) {
        auto* menu = typeinfo_cast<CCMenu*>(child);
        if (!menu) continue;
        unsigned n = menu->getChildrenCount();
        if (!best || n > bestCount) {
            best = menu;
            bestCount = n;
        }
    }
    return best;
}

} // namespace autodeco

// ============================================================================
//  Editor pause menu hook
// ============================================================================

class $modify(AutoDecoPauseLayer, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto* spr = ButtonSprite::create("Auto Deco", 100, true, "bigFont.fnt", "GJ_button_02.png", 30.f, 0.6f);
        auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AutoDecoPauseLayer::onAutoDeco));
        btn->setID("auto-deco-button");

        CCPoint wanted = ccp(winSize.width - 75.f, 38.f);   // bottom-right corner, in screen space
        if (auto* host = autodeco::pickHostMenu(this)) {
            btn->setPosition(host->convertToNodeSpace(wanted));
            host->addChild(btn);
        } else {
            auto* menu = CCMenu::create();
            menu->setPosition(ccp(0.f, 0.f));
            btn->setPosition(wanted);
            menu->addChild(btn);
            this->addChild(menu, 10);
        }
    }

    void onAutoDeco(CCObject*) {
        if (auto* popup = autodeco::AutoDecoPopup::create()) {
            popup->show();
        }
    }
};
