#include "object_roles.hpp"

#include "pointer_map.hpp"
#include "../debug.hpp"
#include "../settings.hpp"

#include <bitset>
#include <utility>

using namespace geode::prelude;

namespace layoutfeed::roles {
    namespace {
        // Decoration object IDs, kept in sync with the object list used by the
        // GDH Layout Mode reference and stored as inclusive ranges.
        constexpr std::pair<int, int> kDecorationRanges[] = {
            {5, 5}, {15, 21}, {41, 41}, {48, 54}, {60, 60}, {73, 73}, {80, 80}, {85, 87}, {97, 97},
            {106, 107}, {110, 110}, {113, 115}, {120, 120}, {123, 134}, {136, 139}, {148, 159},
            {180, 182}, {190, 190}, {211, 211}, {222, 242}, {245, 246}, {259, 259}, {266, 266},
            {273, 273}, {277, 285}, {296, 297}, {324, 325}, {358, 358}, {373, 378}, {394, 396},
            {405, 414}, {419, 420}, {448, 457}, {460, 460}, {472, 474}, {476, 482}, {485, 491},
            {494, 650}, {653, 659}, {668, 672}, {681, 708}, {713, 716}, {719, 719}, {721, 724},
            {730, 736}, {738, 739}, {752, 759}, {762, 767}, {769, 775}, {807, 833}, {841, 848},
            {850, 850}, {853, 857}, {859, 859}, {861, 863}, {867, 874}, {877, 878}, {880, 885},
            {888, 891}, {893, 896}, {902, 911}, {916, 917}, {920, 921}, {923, 961}, {964, 977},
            {980, 988}, {990, 990}, {992, 992}, {997, 1005}, {1009, 1021}, {1024, 1048},
            {1050, 1071}, {1075, 1118}, {1120, 1120}, {1122, 1127}, {1132, 1153}, {1158, 1201},
            {1205, 1207}, {1223, 1225}, {1228, 1259}, {1261, 1261}, {1263, 1263}, {1265, 1267},
            {1269, 1274}, {1276, 1320}, {1322, 1322}, {1325, 1326}, {1348, 1395}, {1431, 1464},
            {1471, 1473}, {1496, 1496}, {1507, 1507}, {1510, 1519}, {1521, 1540}, {1552, 1560},
            {1586, 1586}, {1588, 1588}, {1590, 1593}, {1596, 1597}, {1599, 1610}, {1617, 1618},
            {1621, 1700}, {1737, 1742}, {1752, 1754}, {1756, 1810}, {1820, 1821}, {1823, 1828},
            {1830, 1858}, {1860, 1902}, {1908, 1909}, {1919, 1928}, {1936, 1939}, {2020, 2055},
            {2064, 2065}, {2070, 2700}, {2703, 2704}, {2708, 2770}, {2773, 2773}, {2776, 2807},
            {2838, 2865}, {2867, 2897}, {2927, 2998}, {3000, 3002}, {3032, 3032}, {3038, 3097},
            {3101, 3599}, {3621, 3639}, {3646, 3654}, {3656, 3659}, {3700, 3799}, {3801, 4385},
        };

        constexpr std::size_t kMaxObjectID = 8192;

        std::bitset<kMaxObjectID> const& decorationIDs() {
            static auto const ids = [] {
                std::bitset<kMaxObjectID> value;
                for (auto const& [first, last] : kDecorationRanges) {
                    for (int id = first; id <= last; ++id) value.set(static_cast<std::size_t>(id));
                }
                return value;
            }();
            return ids;
        }

        // A cache entry stores the sprite's part and whether its owner is
        // decoration. The settings-dependent Role is derived on lookup so a
        // settings change never requires reclassifying sprites.
        enum Part : std::uint8_t {
            PartKeep = 0,
            PartMain = 1,
            PartDetail = 2,
            PartGlow = 3,
        };
        constexpr std::uint8_t kDecorationBit = 0x80;
        // The part has no color channel (orbs, pads, portals, ...): its
        // vertex colors already are its default look, so they are kept.
        constexpr std::uint8_t kFixedColorBit = 0x40;
        // Decoration that GD animates or rotates (EnhancedGameObject): deco
        // saws, gears and similar. It looks like gameplay, so it can be kept.
        constexpr std::uint8_t kAnimatedDecorationBit = 0x20;
        constexpr std::uint8_t kOwnerBits = kDecorationBit | kFixedColorBit | kAnimatedDecorationBit;
        constexpr std::uint8_t kPartMask = 0x0F;

        // How an entry is re-validated on every lookup. GD frees and creates
        // sprites while a level runs (detail and glow sprites of activated
        // objects, animated children); a new sprite can get the address of a
        // freed one and must never inherit its role. A stale "Hide" entry was
        // what made gameplay objects such as dash orbs vanish at random.
        enum class Origin : std::uint8_t {
            Object, // the GameObject itself; objects live as long as the level
            Glow,   // ref = owner, valid while owner->m_glowSprite == sprite
            Detail, // ref = owner, valid while owner->m_colorSprite == sprite
            Lazy,   // ref = parent at classification time
        };

        struct Entry {
            std::uint8_t bits = 0;
            Origin origin = Origin::Lazy;
            CCNode const* ref = nullptr;
        };

        bool sameEntry(Entry const& a, Entry const& b) {
            return a.bits == b.bits && a.origin == b.origin && a.ref == b.ref;
        }

        PointerMap<Entry> s_entries;
        std::uint32_t s_generation = 1;
        bool s_collecting = false;

        struct Totals {
            std::uint32_t objects = 0;
            std::uint32_t decoration = 0;
            std::uint32_t detailSprites = 0;
            std::uint32_t glowSprites = 0;
            std::uint32_t activationParts = 0;
            std::uint32_t lazy = 0;
            std::uint32_t lazyKeep = 0;
            std::uint32_t stale = 0;
            std::uint32_t changed = 0;
        } s_totals;

        bool isDecoration(GameObject* object) {
            auto const id = object->m_objectID;
            return object->m_objectType == GameObjectType::Decoration || object->m_isNoTouch ||
                (id > 0 && static_cast<std::size_t>(id) < kMaxObjectID &&
                 decorationIDs().test(static_cast<std::size_t>(id)));
        }

        bool hasFixedColor(GameObject* owner, Part part) {
            return (part == PartMain && !owner->m_baseColor) || (part == PartDetail && !owner->m_detailColor);
        }

        std::uint8_t entryFor(GameObject* owner, Part part) {
            auto entry = static_cast<std::uint8_t>(part);
            if (isDecoration(owner)) {
                entry |= kDecorationBit;
                // One RTTI check per registered part, never per frame.
                if (typeinfo_cast<EnhancedGameObject*>(owner)) entry |= kAnimatedDecorationBit;
            }
            if (hasFixedColor(owner, part)) entry |= kFixedColorBit;
            return entry;
        }

        // Role masks only have to be rebuilt when a sprite that may already
        // be in one changes its role; brand new entries cannot be in a mask.
        bool store(CCNode const* node, Entry entry) {
            if (!node) return false;
            auto const result = s_entries.assign(node, entry, sameEntry);
            if (result != PointerMap<Entry>::Assigned::Changed) return result == PointerMap<Entry>::Assigned::Inserted;
            ++s_totals.changed;
            ++s_generation;
            return true;
        }

        // Detail and glow sprites are often created or re-created when GD
        // activates an object, after PlayLayer::addObject has run.
        void storeSubParts(GameObject* object) {
            if (object->m_colorSprite &&
                store(object->m_colorSprite, {entryFor(object, PartDetail), Origin::Detail, object})) {
                ++s_totals.detailSprites;
            }
            if (object->m_glowSprite &&
                store(object->m_glowSprite, {entryFor(object, PartGlow), Origin::Glow, object})) {
                ++s_totals.glowSprites;
            }
        }

        bool valid(CCNode const* node, Entry const& entry) {
            switch (entry.origin) {
                case Origin::Object:
                    return true;
                case Origin::Glow:
                    return static_cast<GameObject const*>(entry.ref)->m_glowSprite == node;
                case Origin::Detail:
                    return static_cast<GameObject const*>(entry.ref)->m_colorSprite == node;
                case Origin::Lazy:
                    // cocos2d's getParent() is not const-qualified.
                    return const_cast<CCNode*>(node)->getParent() == entry.ref;
            }
            return false;
        }

        // Finds a still valid entry; stale ones are dropped.
        Entry const* lookup(CCNode const* node) {
            auto* entry = s_entries.find(node);
            if (!entry) return nullptr;
            if (valid(node, *entry)) return entry;
            s_entries.erase(node);
            ++s_totals.stale;
            return nullptr;
        }

        bool decorationHidden(std::uint8_t bits) {
            if (!(bits & kDecorationBit) || !settings::hideDecoration()) return false;
            return !((bits & kAnimatedDecorationBit) && settings::showAnimatedDecoration());
        }

        Role toRole(std::uint8_t entry) {
            auto const part = entry & kPartMask;
            if (part == PartKeep) return Role::Keep;
            if (decorationHidden(entry)) return Role::Hide;
            // Only sprites registered as an object's glow are hidden. Glow
            // batch layers also hold other art (parts of portals and orbs),
            // which skipping whole layers used to remove.
            if (part == PartGlow) return settings::hideGlow() ? Role::Hide : Role::Keep;
            if ((entry & kFixedColorBit) || !settings::recolorObjects()) return Role::Opaque;
            return part == PartDetail ? Role::Detail : Role::Main;
        }

        // One-time classification of a sprite that was not registered through
        // PlayLayer::addObject, such as an animated child of an object. Only
        // registered level objects can own a role, so the player (also a
        // GameObject subclass), its parts and all UI keep their colors.
        std::uint8_t classify(CCNode* node) {
            CCNode* previous = node;
            auto* current = node->getParent();
            for (int depth = 0; current && depth < 4; ++depth) {
                if (auto const* entry = lookup(current)) {
                    auto const part = entry->bits & kPartMask;
                    if (part == PartMain && entry->origin == Origin::Object) {
                        auto* owner = static_cast<GameObject*>(current);
                        auto const detail = owner->m_colorSprite && previous == owner->m_colorSprite;
                        return entryFor(owner, detail ? PartDetail : PartMain);
                    }
                    if (part != PartKeep) {
                        return static_cast<std::uint8_t>(part | (entry->bits & kOwnerBits));
                    }
                }
                previous = current;
                current = current->getParent();
            }
            return PartKeep;
        }
    }

    void reset(char const* reason) {
        LF_DEBUG(
            "roles: reset ({}), dropping {} cached sprites from the previous level",
            reason, s_entries.size()
        );
        s_entries.clear();
        s_entries.reserve(1 << 16);
        s_totals = {};
        s_collecting = true;
        ++s_generation;
    }

    void stopCollecting() {
        // Entries stay valid while the quit transition still draws the level;
        // they are dropped when the next level starts.
        s_collecting = false;
    }

    void registerObject(GameObject* object) {
        if (!s_collecting || !object) return;
        store(object, {entryFor(object, PartMain), Origin::Object, nullptr});
        storeSubParts(object);
        ++s_totals.objects;
        if (isDecoration(object)) ++s_totals.decoration;
    }

    void registerGlow(GameObject* object) {
        if (!s_collecting || !object || !object->m_glowSprite) return;
        if (store(object->m_glowSprite, {entryFor(object, PartGlow), Origin::Glow, object})) {
            ++s_totals.glowSprites;
        }
    }

    void refreshParts(GameObject* object) {
        if (!s_collecting || !object) return;
        // Only level objects: the player and editor objects are never
        // registered and must keep their own colors.
        auto const* entry = s_entries.find(object);
        if (!entry || entry->origin != Origin::Object) return;
        auto const before = s_totals.detailSprites + s_totals.glowSprites;
        storeSubParts(object);
        s_totals.activationParts += s_totals.detailSprites + s_totals.glowSprites - before;
    }

    Role resolve(CCNode* sprite) {
        if (!sprite) return Role::Keep;
        if (auto const* entry = lookup(sprite)) return toRole(entry->bits);

        auto const bits = classify(sprite);
        store(sprite, {bits, Origin::Lazy, sprite->getParent()});
        ++s_totals.lazy;
        if ((bits & kPartMask) == PartKeep) ++s_totals.lazyKeep;
        return toRole(bits);
    }

    bool isLevelObject(CCNode* node) {
        auto const* entry = lookup(node);
        return entry && entry->origin == Origin::Object;
    }

    bool hiddenInLayout(GameObject* object) {
        if (!settings::hideDecoration()) return false;
        // Registered objects carry the answer in their entry (no RTTI).
        if (auto const* entry = s_entries.find(object); entry && entry->origin == Origin::Object) {
            return decorationHidden(entry->bits);
        }
        return decorationHidden(entryFor(object, PartMain));
    }

    ccColor3B revealColor(GameObject* owner, bool detail) {
        auto const part = detail ? PartDetail : PartMain;
        if (hasFixedColor(owner, part) || !settings::recolorObjects()) return {255, 255, 255};
        return detail ? settings::detailColor() : settings::objectColor();
    }

    std::uint32_t generation() {
        return s_generation;
    }

    void logSummary() {
        log::info(
            "roles: {} objects registered ({} decoration), {} detail sprites, {} glow sprites "
            "({} found at activation), {} lazily classified ({} unrelated to objects), "
            "{} stale entries replaced, {} roles changed, {} cache entries",
            s_totals.objects, s_totals.decoration, s_totals.detailSprites, s_totals.glowSprites,
            s_totals.activationParts, s_totals.lazy, s_totals.lazyKeep, s_totals.stale,
            s_totals.changed, s_entries.size()
        );
    }
}
