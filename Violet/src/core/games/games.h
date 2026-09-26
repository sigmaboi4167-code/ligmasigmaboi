#pragma once
#include <cstdint>

// violet.lol — supported-games registry.
// PlaceId is read from DataModel right after attach (see App::init).
// If your game shows as "Unknown", the loader prints its PlaceId —
// send it in and it gets a first-class profile on the next build.
namespace Game {

enum class Type : int {
    Unknown = 0,
    Fallen,        // Fallen Survival (ores / plants / Boris / cabinets)
    Rivals,        // Rivals by Nosniy Games
    PhantomForces, // Place 292439477 (PF cache path)
    Combat,        // CB path (Workspace.Characters folder)
    Ops            // Ops path (viewmodels)
};

struct Info {
    Type type;
    const char* name;
    const char* tag;
    const char* features[6];
    int featureCount;
};

// --- known PlaceIds -------------------------------------------------------
inline constexpr std::uint64_t kRivalsPlaces[] = {
    17625359962ull, // Rivals main
};
inline constexpr std::uint64_t kPfPlaces[] = {
    292439477ull, // Phantom Forces
};

inline bool place_in(std::uint64_t p, const std::uint64_t* list, int n) {
    for (int i = 0; i < n; ++i)
        if (list[i] == p) return true;
    return false;
}

// PlaceId-first detection. Fallen Survival has no pinned id yet on purpose:
// it is the home profile — anything that is not a known id boots the
// survival stack (world ESP, fallen ballistics, deep weapon sweep).
inline Type Detect(std::uint64_t placeId) {
    if (place_in(placeId, kRivalsPlaces, (int)(sizeof(kRivalsPlaces) / sizeof(kRivalsPlaces[0]))))
        return Type::Rivals;
    if (place_in(placeId, kPfPlaces, (int)(sizeof(kPfPlaces) / sizeof(kPfPlaces[0]))))
        return Type::PhantomForces;
    return Type::Unknown;
}

// Refine with live cache signals (works even before PlaceId replicates,
// and catches sub-places / private servers whose id is not pinned).
inline Type Refine(Type base, bool pfActive, bool cbActive, bool opsActive) {
    if (pfActive) return Type::PhantomForces;
    if (cbActive) return Type::Combat;
    if (opsActive) return Type::Ops;
    if (base == Type::Unknown) return Type::Fallen; // home turf default
    return base;
}

inline const Info& Get(Type t) {
    static const Info table[] = {
        { Type::Unknown, "Unknown", "generic survival profile",
          { "ESP", "Aimbot", "No Spread", "No Recoil", "World", "Misc" }, 6 },
        { Type::Fallen, "Fallen Survival", "home turf",
          { "ESP", "Fallen ballistics", "No Spread", "No Recoil", "World", "Misc" }, 6 },
        { Type::Rivals, "Rivals", "arena profile",
          { "ESP", "Aimbot", "No Spread", "No Recoil", "Triggerbot", "Misc" }, 6 },
        { Type::PhantomForces, "Phantom Forces", "pf cache path",
          { "ESP", "PF Silent", "No Spread", "No Recoil", "Triggerbot", "Misc" }, 6 },
        { Type::Combat, "Combat", "cb cache path",
          { "ESP", "Aimbot", "No Spread", "No Recoil", "Triggerbot", "Misc" }, 6 },
        { Type::Ops, "Ops", "viewmodel path",
          { "ESP", "Aimbot", "No Spread", "No Recoil", "Triggerbot", "Misc" }, 6 },
    };
    for (auto& i : table)
        if (i.type == t) return i;
    return table[0];
}

// Combat profile: big-tree games get the deep ReplicatedStorage sweep,
// cache-path games stay light (their trees are huge and hot).
inline bool DeepSweep(Type t) {
    return t == Type::Fallen || t == Type::Rivals || t == Type::Unknown;
}

} // namespace Game
