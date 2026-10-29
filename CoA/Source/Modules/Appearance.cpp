// AscAppearance.cpp (see Appearance.h): the manager, the SMSG handlers, the validator, C_Appearance and C_ItemSet.
//
// Module init 0x100C5AE0: the six libraries (C_Appearance 0x100CD9B0, C_AppearanceCollection 0x100CD850,
// C_AppearanceOutfit 0x100CDC20, C_ItemSet 0x100CDDF0; C_UICamera and C_Item live elsewhere), nine events,
// OnGlueScreen 0x100C2170 and the SMSG handlers below.
//
// Manager layout:
//   +0x000 vector<u32> pending    per-category appearance the player has picked (index = category id)
//   +0x00C AppearancesEntryContainer (collection browser)       +0x0C8 AppearanceOutfitEntryContainer
//   +0x144 u32 (reset at glue)    +0x148 vector<u32> current    (what the server says is applied)
//   +0x154 vector<{u32 id, u32, u64 time}> collected appearances
//   +0x160 unordered_map<string, vector<u32>> outfits           +0x180 string pending outfit name
//   +0x198 vector<u32> pending outfit appearances               +0x1A4 / +0x1A5 u8 visibility flags
//
// SMSG:
//   0x698 (FUN_100c4a50) str result: APPLY_APPEARANCES_* (29); OK -> current := pending;
//         APPLY_PENDING_APPEARANCE_RESULT(ok, result).
//   0x699 (FUN_100c4480) u32 n x {u32 appearance, u32 time}: the collected list.
//   0x69A (FUN_100c41e0) u32 n x u32: current; then pending := current.
//   0x69B (handler_0x069b) u32 appearance, u32 -- newly collected (time now); APPEARANCE_COLLECTED(id).
//   0x69C (handler_0x069c) u32 appearance, u32 -- uncollected; APPEARANCE_UNCOLLECTED(id).
//   0x69D (FUN_100c4530) u32 n x {str name, u32 m x u32}: the outfits.
//   0x69F (FUN_100c5320) str result: SAVE_APPEARANCE_OUTFIT_*; OK -> outfits[pending name] := pending outfit;
//         APPEARANCE_OUTFIT_SAVED_RESULT(ok, result).
//   0x6A1 (FUN_100c4f70) str result: DELETE_APPEARANCE_OUTFIT_*; OK -> erase outfits[pending name];
//         APPEARANCE_OUTFIT_DELETED_RESULT(ok, result).
//   0x6A2 (handler_0x06a2) u8, u8 -> +0x1A4, +0x1A5.
//   0x6EB (FUN_100c4da0) str -> COLLECT_ITEM_APPEARANCE_RESULT(str).
//
// DLL DBCs (rows as the original's parser lays them out):
//   AppearanceCategories (0x10BE005C, patched by SMSG 0x691): +0 id, +4 type (APPEARANCE_TYPE_* name, FUN_101b07c0),
//     +8 name, +0xC tier, +0x10 inventory type, +0x18 icon, +0x1C / +0x20 class mask.
//   Appearances (0x10BDFCB4, SMSG 0x692): +0 id, +4, +8 display type name, +0xC, +0x10 camera, +0x14..+0x1C
//     categories, +0x20 source (item / item set / creature), +0x24, +0x2C, +0x30..+0x40 realm gates.
//   AppearanceDetails (0x10BDFA2C, iterated in file order): +4 appearance, +8 details, +0xC store product,
//     +0x10 Ethereal Bazaar flag.
//   ItemAppearances: by item (FUN_1020fae0) -> +8 appearance.  ItemSetAppearances: by item set (FUN_10210030) -> +8.
#include "CoA/Source/Modules/AppearanceInternal.h"
#include "CoA/Source/Modules/CA.h"
#include "CoA/Source/Modules/Config.h"
#include "CoA/Source/Modules/PatchData.h"
#include "CoA/Source/Modules/RealmInfo.h"
#include "CoA/Source/Runtime/ClientHooks.h"
#include "CoA/Source/Runtime/ClientState.h"
#include "CoA/Source/Runtime/Events.h"
#include "CoA/Source/Runtime/Lifecycle.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Packet.h"
#include "CoA/Source/Runtime/Packets.h"
#include "CoA/Source/Runtime/Read.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <algorithm>
#include <ctime>

using namespace CoA::Appearance;
using namespace CoA::Appearance::detail;
using CoA::Script::PushBool;
using CoA::Script::PushStr;
using CoA::Script::ReadInt2;
using CoA::Script::ReadNumber;
using CoA::Script::ValidateInput;

const char* const CoA::Appearance::detail::kAppearanceTypes[12] = { "APPEARANCE_TYPE_NONE", "APPEARANCE_TYPE_ITEM",
    "APPEARANCE_TYPE_ILLUSION", "APPEARANCE_TYPE_INCARNATION", "APPEARANCE_TYPE_SPELL_VISUAL", "APPEARANCE_TYPE_COSMETIC_PET",
    "APPEARANCE_TYPE_COSMETIC", "APPEARANCE_TYPE_ITEM_SET", "APPEARANCE_TYPE_OUTFIT", "APPEARANCE_TYPE_MOUNT", "APPEARANCE_TYPE_COMPANION",
    "APPEARANCE_TYPE_TOY" };

const char* const CoA::Appearance::detail::kDisplayTypes[8] = { "APPEARANCE_DISPLAY_TYPE_NONE", "APPEARANCE_DISPLAY_TYPE_ITEM",
    "APPEARANCE_DISPLAY_TYPE_CREATURE", "APPEARANCE_DISPLAY_TYPE_SPELL", "APPEARANCE_DISPLAY_TYPE_ITEM_ENCHANT",
    "APPEARANCE_DISPLAY_TYPE_ITEM_SET", "APPEARANCE_DISPLAY_TYPE_STATIC_SPELL_VISUAL", "APPEARANCE_DISPLAY_TYPE_ANIMATED_SPELL_VISUAL" };

namespace {
    const char* const kApplyResults[29] = { "APPLY_APPEARANCES_OK", "APPLY_APPEARANCES_UNKNOWN", "APPLY_APPEARANCES_OUT_OF_RANGE",
        "APPLY_APPEARANCES_NOT_IN_WORLD", "APPLY_APPEARANCES_WRONG_SIZE", "APPLY_APPEARANCES_BAD_APPEARANCE", "APPLY_APPEARANCES_BAD_CATEGORY",
        "APPLY_APPEARANCES_CATEGORY_NOT_FOUND", "APPLY_APPEARANCES_NOT_AVAILABLE_FOR_CLASS", "APPLY_APPEARANCES_NOT_COLLECTED",
        "APPLY_APPEARANCES_CANNOT_APPLY_ITEM_SET", "APPLY_APPEARANCES_NOT_REALM_APPROPRIATE", "APPLY_APPEARANCES_BAD_ITEM_TEMPLATE",
        "APPLY_APPEARANCES_BAD_ITEM_CLASS", "APPLY_APPEARANCES_THROWN_APPEARANCE", "APPLY_APPEARANCES_COSMETIC_PET_IN_COMBAT",
        "APPLY_APPEARANCES_BAD_CREATURE_ENTRY", "APPLY_APPEARANCES_BAD_CREATURE_TEMPLATE", "APPLY_APPEARANCES_BAD_CREATURE_EXPANSION",
        "APPLY_APPEARANCES_CANNOT_TAME_EXOTIC", "APPLY_APPEARANCES_BAD_CREATURE_SCRIPT", "APPLY_APPEARANCES_PET_TYPE_NOT_FOUND",
        "APPLY_APPEARANCES_PET_SUMMON_SPELL_NOT_FOUND", "APPLY_APPEARANCES_PET_SUMMON_SPELL_NOT_LEARNED",
        "APPLY_APPEARANCES_PET_DISMISS_SPELL_NOT_FOUND", "APPLY_APPEARANCES_PET_DISMISS_SPELL_NOT_LEARNED",
        "APPLY_APPEARANCES_PET_DEAD_NON_SAFE_ZONE", "APPLY_APPEARANCES_UNSUPPORTED_CATEGORY",
        "APPLY_APPEARANCES_COSMETIC_PET_APPEARANCE_CONFIRMATION" };   // 0x10B2A020
    const char* const kSaveResults[2] = { "SAVE_APPEARANCE_OUTFIT_OK", "SAVE_APPEARANCE_OUTFIT_UNKNOWN" };         // 0x10B297C8
    const char* const kDeleteResults[2] = { "DELETE_APPEARANCE_OUTFIT_OK", "DELETE_APPEARANCE_OUTFIT_UNKNOWN" };   // 0x10B29F38

    Deps s_deps;
    Manager s_m;
    bool s_browserDirty = true;
}

// ---- the shared helpers ----------------------------------------------------------------------------------------
Manager& CoA::Appearance::detail::M() {
    return s_m;
}

bool& CoA::Appearance::detail::BrowserDirty() {
    return s_browserDirty;
}

CoA::Appearance::Deps& CoA::Appearance::Dependencies() {
    return s_deps;
}

CoA::Dbc::Table& CoA::Appearance::detail::Categories() {
    return CoA::Dbc::Get("DBFilesClient/AppearanceCategories.dbc");
}

CoA::Dbc::Table& CoA::Appearance::detail::Appearances() {
    return CoA::Dbc::Get("DBFilesClient/Appearances.dbc");
}

CoA::Dbc::Table& CoA::Appearance::detail::Details() {
    return CoA::Dbc::Get("DBFilesClient/AppearanceDetails.dbc");
}

CoA::Dbc::Table& CoA::Appearance::detail::ItemAppearances() {
    return CoA::Dbc::Get("DBFilesClient/ItemAppearances.dbc");
}

CoA::Dbc::Table& CoA::Appearance::detail::ItemSetAppearances() {
    return CoA::Dbc::Get("DBFilesClient/ItemSetAppearances.dbc");
}

bool CoA::Appearance::detail::FetchItem(uint32_t id, ItemRecord& out) {
    out = ItemRecord();
    return s_deps.item && s_deps.item(id, out);
}

bool CoA::Appearance::detail::FetchItemSet(uint32_t id, ItemSetRecord& out) {
    out = ItemSetRecord();
    return s_deps.itemSet && s_deps.itemSet(id, out);
}

bool CoA::Appearance::detail::HasPlayer() {
    return s_deps.hasPlayer && s_deps.hasPlayer();
}

uint32_t CoA::Appearance::detail::PlayerClass() {
    return s_deps.playerClass ? s_deps.playerClass() : 0;
}

// FUN_101b0ac0 -> FUN_101b07c0: a category row's APPEARANCE_TYPE_* name as its index (0 = none).
int CoA::Appearance::detail::CategoryType(const uint8_t* row) {
    const char* name = Categories().Str(row, 4);

    for (int i = 0; i < 12; ++i) {
        if (strcmp(name, kAppearanceTypes[i]) == 0) {
            return i;
        }
    }

    return TYPE_NONE;
}

// The inline pattern "category row exists && its type == t" (FUN_100c6030 and siblings).
bool CoA::Appearance::detail::CategoryIs(uint32_t category, int type) {
    const uint8_t* row = Categories().Row(category);
    return row && CategoryType(row) == type;
}

uint32_t CoA::Appearance::detail::At(const std::vector<uint32_t>& v, uint32_t i) {
    return i < v.size() ? v[i] : 0;
}

void CoA::Appearance::detail::PendingChanged() {
    CoA::Events::Signal("PENDING_APPEARANCE_CHANGED", nullptr);
}

namespace {
    // FUN_1020fc80 (ItemAppearances, file order) and FUN_102101d0 (ItemSetAppearances, id order) build +4 -> row with
    // the last row winning, and +8 -> rows; SMSG 0x693 / 0x6EC rebuild them.
    std::unordered_map<uint32_t, const uint8_t*> s_itemAppearanceByItem;
    std::unordered_map<uint32_t, const uint8_t*> s_itemSetAppearanceBySet;
    std::unordered_map<uint32_t, std::vector<const uint8_t*>> s_itemsForAppearance;
    std::unordered_map<uint32_t, std::vector<const uint8_t*>> s_setsByAppearance;
    bool s_itemIndexBuilt = false;
    bool s_setIndexBuilt = false;

    void BuildItemAppearanceIndex() {   // FUN_1020fc80
        s_itemIndexBuilt = true;
        s_itemAppearanceByItem.clear();
        s_itemsForAppearance.clear();
        CoA::Dbc::Table& t = ItemAppearances();

        for (uint32_t i = 0; i < t.Count(); ++i) {
            if (const uint8_t* r = t.RowAt(i)) {
                s_itemAppearanceByItem[U32(r, 4)] = r;
                s_itemsForAppearance[U32(r, 8)].push_back(r);
            }
        }
    }

    void BuildItemSetAppearanceIndex() {   // FUN_102101d0
        s_setIndexBuilt = true;
        s_itemSetAppearanceBySet.clear();
        s_setsByAppearance.clear();
        CoA::Dbc::Table& t = ItemSetAppearances();

        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id) {
            if (const uint8_t* r = t.Row(id)) {
                s_itemSetAppearanceBySet[U32(r, 4)] = r;
                s_setsByAppearance[U32(r, 8)].push_back(r);
            }
        }
    }

    // FUN_101b5660: item appearance -> the item-set appearances whose set contains that item (ItemSet.dbc in id order,
    // set items +8..+0x48).
    std::unordered_map<uint32_t, std::vector<uint32_t>> s_setsForItemAppearance;
    bool s_setsForItemAppearanceBuilt = false;

    // FUN_102100b0's map: appearance -> the item sets whose ItemSetAppearances row points at it (built once).
    std::unordered_map<uint32_t, std::vector<uint32_t>> s_setsWithAppearance;
}

const uint8_t* CoA::Appearance::detail::ItemAppearanceOf(uint32_t item) {
    if (!s_itemIndexBuilt) {
        BuildItemAppearanceIndex();
    }

    auto it = s_itemAppearanceByItem.find(item);
    return it == s_itemAppearanceByItem.end() ? nullptr : it->second;
}

const uint8_t* CoA::Appearance::detail::ItemSetAppearanceOf(uint32_t set) {
    if (!s_setIndexBuilt) {
        BuildItemSetAppearanceIndex();
    }

    auto it = s_itemSetAppearanceBySet.find(set);
    return it == s_itemSetAppearanceBySet.end() ? nullptr : it->second;
}

const std::vector<const uint8_t*>* CoA::Appearance::detail::ItemsForAppearance(uint32_t appearance) {
    if (!s_itemIndexBuilt) {
        BuildItemAppearanceIndex();
    }

    auto it = s_itemsForAppearance.find(appearance);
    return it == s_itemsForAppearance.end() ? nullptr : &it->second;
}

std::vector<uint32_t> CoA::Appearance::detail::SetsWithAppearance(uint32_t appearance) {
    if (s_setsWithAppearance.empty()) {
        CoA::Dbc::Table& t = ItemSetAppearances();

        for (uint32_t i = 0; i < t.Count(); ++i) {
            if (const uint8_t* r = t.RowAt(i)) {
                s_setsWithAppearance[U32(r, 8)].push_back(U32(r, 4));
            }
        }
    }

    auto it = s_setsWithAppearance.find(appearance);
    return it == s_setsWithAppearance.end() ? std::vector<uint32_t>() : it->second;
}

// DAT_10bdfa34 / DAT_10bdfa48: the first AppearanceDetails row (file order) for an appearance.
const uint8_t* CoA::Appearance::detail::DetailsFor(uint32_t appearance) {
    CoA::Dbc::Table& t = Details();

    for (uint32_t i = 0; i < t.Count(); ++i) {
        if (const uint8_t* r = t.RowAt(i)) {
            if (U32(r, 4) == appearance) {
                return r;
            }
        }
    }

    return nullptr;
}

namespace {
    // DAT_10bdb264 (FUN_100059a0): the inventory types an ITEM category may take from an item set.
    const uint32_t kSetSlots[12] = { 1, 3, 4, 5, 6, 7, 8, 9, 10, 16, 19, 20 };

    bool SetSlot(uint32_t inv) {
        return std::find(std::begin(kSetSlots), std::end(kSetSlots), inv) != std::end(kSetSlots);
    }
}

// FUN_100c3ba0: an item set's appearances per category -- for each set item whose item record's inventory type (0x14
// robe counts as 5 chest) matches an ITEM category's +0x10, and that inventory type is one of the tracked slots, the
// item's ItemAppearances appearance goes in that category's index. The list is MaxId long (FUN_100ce6b0(max)), so the
// highest category id never gets a slot.
void CoA::Appearance::detail::ItemSetAppearanceList(const ItemSetRecord& set, std::vector<uint32_t>& out) {
    CoA::Dbc::Table& cats = Categories();
    out.assign(cats.Loaded() ? cats.MaxId() : 0, 0);

    for (uint32_t k = 0; k < 17; ++k) {
        const uint32_t item = set.items[k];
        ItemRecord itemRow;

        if (!FetchItem(item, itemRow)) {
            continue;
        }

        uint32_t inv = itemRow.inventoryType;

        if (inv == 0x14) {
            inv = 5;   // robe -> chest
        }

        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id) {
            const uint8_t* c = cats.Row(id);

            if (!c || U32(c, 0x10) != inv || CategoryType(c) != TYPE_ITEM) {
                continue;
            }

            if (SetSlot(inv) && U32(c, 0) < out.size()) {
                const uint8_t* ia = ItemAppearanceOf(item);
                out[U32(c, 0)] = ia ? U32(ia, 8) : 0;
            }

            break;
        }
    }
}

// FUN_100c57a0: an item-set appearance counts as collected when every part is; anything else when it is in the list.
bool CoA::Appearance::detail::IsCollected(uint32_t appearance) {
    if (const uint8_t* a = Appearances().Row(appearance)) {
        ItemSetRecord set;

        if (FetchItemSet(U32(a, 0x20), set)) {
            std::vector<uint32_t> parts;
            ItemSetAppearanceList(set, parts);

            for (uint32_t p : parts) {
                if (p && !IsCollected(p)) {
                    return false;
                }
            }

            return true;
        }
    }

    for (const Collected& c : s_m.collected) {
        if (c.id == appearance) {
            return true;
        }
    }

    return false;
}

uint64_t CoA::Appearance::detail::CollectedTime(uint32_t appearance) {
    for (const Collected& c : s_m.collected) {
        if (c.id == appearance) {
            return c.time;
        }
    }

    return 0;
}

// FUN_101b0b00: the category's 64-bit class mask (+0x1C / +0x20) has the player's class bit.
bool CoA::Appearance::detail::CategoryAllowsClass(const uint8_t* category) {
    if (!HasPlayer()) {
        return false;
    }

    const uint32_t bit = PlayerClass() - 1;
    uint32_t lo = 1u << (bit & 0x1F);
    uint32_t hi = 0;

    if (bit > 0x1F) {
        hi = lo;
    }

    lo ^= hi;

    if (bit > 0x3F) {
        hi = lo;
    }

    return (lo & U32(category, 0x1C)) != 0 || (hi & U32(category, 0x20)) != 0;
}

// FUN_101b49f0: an Appearances row's realm gates (+0x30 live, +0x34 seasonal, +0x38 league, +0x3C PTR, +0x40
// development) against the realm object +0x40.. flags.
bool CoA::Appearance::detail::RealmAppropriate(const uint8_t* a) {
    const CoA::RealmInfo::Info& r = CoA::RealmInfo::Get();
    return (U32(a, 0x30) && r.Live()) || (U32(a, 0x34) && r.Seasonal()) || (U32(a, 0x38) && r.League()) || (U32(a, 0x3C) && r.PTR()) ||
        (U32(a, 0x40) && r.Dev());
}

bool CoA::Appearance::detail::HasCategoryOfType(const uint8_t* a, int type) {   // the +0x14..+0x1C category loop
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t c = U32(a, 0x14 + k * 4);

        if (c && CategoryIs(c, type)) {
            return true;
        }
    }

    return false;
}

bool CoA::Appearance::detail::IsItemDisplay(const uint8_t* a) {
    return strcmp(Appearances().Str(a, 8), kDisplayTypes[1]) == 0;
}

// FUN_101b41e0: an item set appearance's set name (ItemSet.dbc +4, "Unknown Item Set"), else the item's name from
// FUN_1020e9c0, "Unknown Item" when it is empty.
std::string CoA::Appearance::detail::AppearanceName(const uint8_t* a) {
    const uint32_t source = U32(a, 4);

    if (HasCategoryOfType(a, TYPE_ITEM_SET)) {
        ItemSetRecord set;
        return FetchItemSet(source, set) ? set.name : "未知物品套装";   // 中文化（By leewheel 2026-10-29）
    }

    const ItemInfo info = s_deps.itemInfo ? s_deps.itemInfo(source) : ItemInfo();
    return info.name.empty() ? "未知物品" : info.name;
}

void CoA::Appearance::detail::PushIntList(lua_State* L, const std::vector<uint32_t>& v) {
    lua_createtable(L, 0, static_cast<int>(v.size()));
    lua_checkstack(L, 2);

    for (uint32_t i = 0; i < v.size(); ++i) {
        lua_pushnumber(L, static_cast<double>(i + 1));
        lua_pushinteger(L, static_cast<int>(v[i]));
        lua_settable(L, -3);
    }
}

// FUN_100c6750: reset the browser (+0xC, FUN_100c6670) and the outfit view (+0xC8, FUN_100c65d0); in world (0xBD0792),
// VIEWABLE_APPEARANCES_RESET.
void CoA::Appearance::RebuildOutfits() {
    s_browserDirty = true;

    if (CoA::ClientState::InWorld()) {
        CoA::Events::Signal("VIEWABLE_APPEARANCES_RESET", nullptr);
    }
}

namespace {
    // ---- SMSG ----------------------------------------------------------------------------------------------------
    int ResultIndex(const std::string& s, const char* const* names, int n) {
        for (int i = 0; i < n; ++i) {
            if (s == names[i]) {
                return i;
            }
        }

        return -1;
    }

    // A count past the bytes left reads nothing (the 7a 0x726 guard): the original would read past the buffer.
    bool CountFits(CDataStore* msg, uint32_t n, uint32_t bytesEach) {
        return n <= CoA::Read::Remaining(msg) / bytesEach;
    }

    int32_t OnApplyResult(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x698
        const std::string result = CoA::Read::Str(msg);
        const int i = ResultIndex(result, kApplyResults, 29);

        if (i == 0) {
            s_m.current = s_m.pending;
        }

        CoA::Events::Signal("APPLY_PENDING_APPEARANCE_RESULT", "%b%s", i == 0, result.c_str());
        return 1;
    }

    int32_t OnCollectedList(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x699
        s_m.collected.clear();
        const uint32_t n = CoA::Read::U32(msg);

        for (uint32_t k = 0, fits = CountFits(msg, n, 8); fits && k < n; ++k) {
            const uint32_t id = CoA::Read::U32(msg);
            const uint32_t time = CoA::Read::U32(msg);
            s_m.collected.push_back({ id, 0, time });
        }

        return 1;
    }

    int32_t OnCurrent(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x69A
        s_m.current.clear();
        const uint32_t n = CoA::Read::U32(msg);

        for (uint32_t k = 0, fits = CountFits(msg, n, 4); fits && k < n; ++k) {
            s_m.current.push_back(CoA::Read::U32(msg));
        }

        s_m.pending = s_m.current;
        return 1;
    }

    int32_t OnCollected(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x69B
        const uint32_t id = CoA::Read::U32(msg);
        const uint32_t second = CoA::Read::U32(msg);

        if (!IsCollected(id)) {
            s_m.collected.push_back({ id, 0, static_cast<uint64_t>(_time64(nullptr)) });
            CoA::Events::Signal("APPEARANCE_COLLECTED", "%u%u", id, second);
        }

        return 1;
    }

    int32_t OnUncollected(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x69C
        const uint32_t id = CoA::Read::U32(msg);
        const uint32_t second = CoA::Read::U32(msg);

        if (IsCollected(id)) {
            s_m.collected.erase(std::remove_if(s_m.collected.begin(), s_m.collected.end(), [id](const Collected& c) { return c.id == id; }),
                s_m.collected.end());
            CoA::Events::Signal("APPEARANCE_UNCOLLECTED", "%u%u", id, second);
        }

        return 1;
    }

    int32_t OnOutfits(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x69D
        s_m.outfits.clear();   // FUN_100cd0f0
        const uint32_t n = CoA::Read::U32(msg);

        for (uint32_t k = 0, fits = CountFits(msg, n, 5); fits && k < n; ++k) {
            std::vector<uint32_t>& list = s_m.outfits[CoA::Read::Str(msg)];
            const uint32_t m = CoA::Read::U32(msg);

            for (uint32_t j = 0, fits = CountFits(msg, m, 4); fits && j < m; ++j) {
                list.push_back(CoA::Read::U32(msg));
            }
        }

        CoA::Appearance::RebuildOutfits();
        return 1;
    }

    int32_t OnOutfitSaved(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x69F
        const std::string result = CoA::Read::Str(msg);
        const int i = ResultIndex(result, kSaveResults, 2);

        if (i == 0) {
            s_m.outfits[s_m.pendingOutfitName] = s_m.pendingOutfit;
            s_m.pendingOutfitName.clear();
            s_m.pendingOutfit.clear();
            CoA::Appearance::RebuildOutfits();
        }

        CoA::Events::Signal("APPEARANCE_OUTFIT_SAVED_RESULT", "%b%s", i == 0, result.c_str());
        return 1;
    }

    int32_t OnOutfitDeleted(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x6A1
        const std::string result = CoA::Read::Str(msg);
        const int i = ResultIndex(result, kDeleteResults, 2);

        if (i == 0) {
            s_m.outfits.erase(s_m.pendingOutfitName);
            s_m.pendingOutfitName.clear();
            CoA::Appearance::RebuildOutfits();
        }

        CoA::Events::Signal("APPEARANCE_OUTFIT_DELETED_RESULT", "%b%s", i == 0, result.c_str());
        return 1;
    }

    int32_t OnVisibility(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x6A2
        s_m.seeOwn = CoA::Read::U8(msg) != 0;
        s_m.seeOthers = CoA::Read::U8(msg) != 0;
        return 1;
    }

    int32_t OnCollectItemResult(void*, uint32_t, uint32_t, CDataStore* msg) {   // 0x6EB
        const std::string result = CoA::Read::Str(msg);
        CoA::Events::Signal("COLLECT_ITEM_APPEARANCE_RESULT", "%s", result.c_str());
        return 1;
    }

    // ---- C_Appearance --------------------------------------------------------------------------------------------
    void PushIntOrNil(lua_State* L, uint32_t v) {
        if (v) {
            lua_pushinteger(L, static_cast<int>(v));
        } else {
            lua_pushnil(L);
        }
    }

    // handler_GetAppearanceForCategory: current[category], nil when 0 / out of range.
    int GetAppearanceForCategory(lua_State* L) {
        uint32_t category;

        if (!ReadNumber(L, category)) {
            return 0;
        }

        PushIntOrNil(L, At(s_m.current, category));
        return 1;
    }

    int GetItemAppearanceID(lua_State* L) {   // handler_GetItemAppearanceID
        uint32_t item;

        if (!ReadNumber(L, item)) {
            return 0;
        }

        if (const uint8_t* r = ItemAppearanceOf(item)) {
            lua_pushinteger(L, static_cast<int>(U32(r, 8)));
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    int GetItemSetAppearanceID(lua_State* L) {   // handler_GetItemSetAppearanceID
        uint32_t set;

        if (!ReadNumber(L, set)) {
            return 0;
        }

        if (const uint8_t* r = ItemSetAppearanceOf(set)) {
            lua_pushinteger(L, static_cast<int>(U32(r, 8)));
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    // handler_HasPendingAppearances: any index where pending and current differ.
    int HasPendingAppearances(lua_State* L) {
        const size_t n = std::max(s_m.pending.size(), s_m.current.size());
        bool any = false;

        for (uint32_t i = 0; i < n && !any; ++i) {
            any = At(s_m.pending, i) != At(s_m.current, i);
        }

        PushBool(L, any);
        return 1;
    }

    // handler_GetPendingAppearance: pending[category] when it differs from current, else nil.
    int GetPendingAppearance(lua_State* L) {
        uint32_t category;

        if (!ReadNumber(L, category)) {
            return 0;
        }

        const uint32_t p = At(s_m.pending, category);

        if (p == At(s_m.current, category)) {
            lua_pushnil(L);
        } else {
            lua_pushinteger(L, static_cast<int>(p));
        }

        return 1;
    }

    // FUN_100c21f0: put one category back to current (growing pending to current's size first).
    void ClearPending(uint32_t category) {
        if (At(s_m.pending, category) == At(s_m.current, category)) {
            return;
        }

        if (s_m.pending.size() < s_m.current.size()) {
            const size_t from = s_m.pending.size();
            s_m.pending.resize(s_m.current.size());

            for (size_t i = from; i < s_m.current.size(); ++i) {
                s_m.pending[i] = s_m.current[i];
            }

            PendingChanged();
        }

        if (category < s_m.pending.size()) {
            // The original indexes current unchecked (the server always sends 0x69A at login); read it bounded.
            s_m.pending[category] = At(s_m.current, category);
            PendingChanged();
        }
    }

    int ClearPendingAppearance(lua_State* L) {   // handler_ClearPendingAppearance
        uint32_t category;

        if (ReadNumber(L, category)) {
            ClearPending(category);
        }

        return 0;
    }

    int ClearPendingAppearances(lua_State*) {   // handler_ClearPendingAppearances
        if (s_m.pending != s_m.current) {
            s_m.pending = s_m.current;
            PendingChanged();
        }

        return 0;
    }

    int CanSeeAppearances(lua_State* L) {   // handler_CanSeeAppearances
        PushBool(L, s_m.seeOwn);
        PushBool(L, s_m.seeOthers);
        return 2;
    }

    // FUN_100cb2e0: (own, others) -> CMSG 0x6A3 {u8, u8}, flags updated at once, true.
    int SetCanSeeAppearances(lua_State* L) {
        if (!ValidateInput(L, { LUA_TBOOLEAN, LUA_TBOOLEAN })) {
            return 0;
        }

        const bool own = lua_toboolean(L, 1) != 0;
        const bool others = lua_toboolean(L, 2) != 0;
        CoA::Packet(0x6A3).U8(own ? 1 : 0).U8(others ? 1 : 0).Send();
        s_m.seeOwn = own;
        s_m.seeOthers = others;
        PushBool(L, true);
        return 1;
    }

    // handler_IsTransmogable: every category except MOUNT / COMPANION / TOY.
    int IsTransmogable(lua_State* L) {
        uint32_t category;

        if (!ReadNumber(L, category)) {
            return 0;
        }

        PushBool(L, !CategoryIs(category, TYPE_MOUNT) && !CategoryIs(category, TYPE_COMPANION) && !CategoryIs(category, TYPE_TOY));
        return 1;
    }

    int GetActiveDiscount(lua_State* L) {   // handler_GetActiveDiscount: (id) -> false, nil, nil
        uint32_t unused;

        if (!ReadNumber(L, unused)) {
            return 0;
        }

        PushBool(L, false);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }

    int IsEtherealBazaarAppearance(lua_State* L) {   // handler_IsEtherealBazaarAppearance
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        const uint8_t* d = DetailsFor(appearance);
        PushBool(L, d && U32(d, 0x10) != 0);
        return 1;
    }

    // FUN_100c7f60: the details text, nil when absent or empty.
    int GetAppearanceDetails(lua_State* L) {
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        const uint8_t* d = DetailsFor(appearance);
        const char* text = d ? Details().Str(d, 8) : "";

        if (d && *text) {
            PushStr(L, text);
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    std::string StorePage(uint32_t product, const char* tail) {
        return std::string("store/") + CoA::RealmInfo::Get().dataPath + "/" + std::to_string(product) + "/" + tail;
    }

    // FUN_100c4120 / FUN_100c3ff0: "store/{realm +0x4C}/{product}/game" and ".../checkout"; empty when the appearance has
    // no details row or no product.
    std::string StoreUrl(uint32_t appearance, const char* tail) {
        const uint8_t* d = DetailsFor(appearance);
        const uint32_t product = d ? U32(d, 0xC) : 0;
        return product ? StorePage(product, tail) : std::string();
    }

    void PushStrOrNil(lua_State* L, const std::string& s) {
        if (s.empty()) {
            lua_pushnil(L);
        } else {
            PushStr(L, s.c_str());
        }
    }

    // FUN_100c8950: (appearance) -> game URL (or nil), and the checkout URL when there is one.
    int GetAppearanceWebURL(lua_State* L) {
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        PushStrOrNil(L, StoreUrl(appearance, "game"));
        PushStrOrNil(L, StoreUrl(appearance, "checkout"));
        return 2;
    }

    // FUN_100ca120: the store pages of product 0x93, both always pushed.
    int GetEtherealBazaarWebURL(lua_State* L) {
        PushStr(L, StorePage(0x93, "game").c_str());
        PushStr(L, StorePage(0x93, "checkout").c_str());
        return 2;
    }

    // ---- the validator (FUN_100c1b40) ----------------------------------------------------------------------------
    bool StockClass() {   // FUN_100c6380
        const uint32_t c = PlayerClass();
        return (c >= 1 && c <= 9) || c == 0xB;
    }

    bool CoAClass() {   // FUN_100c5f50
        const uint32_t c = PlayerClass();
        return c >= 0xC && c <= 0x20;
    }

    // The summon (first) / dismiss (second) spell a cosmetic pet of tier 1..5 needs.
    uint32_t PetSpell(uint32_t tier, bool dismiss) {
        if (!HasPlayer()) {
            return 0;
        }

        const bool stock = StockClass();

        switch (tier) {
            case 1: return stock ? (dismiss ? 0x10D331 : 0x10CC53) : CoAClass() ? (dismiss ? 0x8C35F : 0x8C35E) : (dismiss ? 0xA51 : 0x373);
            case 2: return stock ? (dismiss ? 0x10CC5C : 0x10CC54) : (dismiss ? 0x37C : 0x374);
            case 3: return dismiss ? 0x37D : 0x375;
            case 4: return stock ? (dismiss ? 0x15FF73 : 0x15FF42) : (dismiss ? 0x16603 : 0x165D2);
            case 5: return stock ? (dismiss ? 0x15FF74 : 0x15FF5F) : (dismiss ? 0x16604 : 0x165EF);
            default: return 0;
        }
    }

    bool SpellExists(uint32_t spell) {
        return s_deps.spellExists && s_deps.spellExists(spell);
    }

    bool KnowsSpell(uint32_t spell) {
        return s_deps.knowsSpell && s_deps.knowsSpell(spell);
    }

    // FUN_100c1b40(list, category, confirmed): 0 or an APPLY_APPEARANCES_* index for list[category].
    uint32_t Validate(const std::vector<uint32_t>& list, uint32_t category, bool confirmed) {
        if (category >= list.size()) {
            return 2;   // OUT_OF_RANGE
        }

        if (!HasPlayer()) {
            return 3;   // NOT_IN_WORLD
        }

        const uint32_t appearance = list[category];

        if (appearance == At(s_m.current, category) || appearance == 0) {
            return 0;
        }

        const uint8_t* a = Appearances().Row(appearance);

        if (!a) {
            return 5;   // BAD_APPEARANCE
        }

        if (!RealmAppropriate(a)) {
            return 0xB;   // NOT_REALM_APPROPRIATE
        }

        bool inCategory = false;

        for (uint32_t k = 0; k < 3; ++k) {
            inCategory = inCategory || U32(a, 0x14 + k * 4) == category;
        }

        if (!inCategory) {
            return 6;   // BAD_CATEGORY
        }

        const uint8_t* cat = Categories().Row(category);

        if (!cat) {
            return 7;   // CATEGORY_NOT_FOUND
        }

        if (!CategoryAllowsClass(cat)) {
            return 8;   // NOT_AVAILABLE_FOR_CLASS
        }

        if (!IsCollected(U32(a, 0))) {
            return 9;   // NOT_COLLECTED
        }

        const int type = CategoryType(cat);

        if (type == TYPE_ITEM) {
            ItemRecord item;

            if (!FetchItem(U32(a, 0x20), item)) {
                return 0xC;   // BAD_ITEM_TEMPLATE
            }

            if (item.classId != 4 && item.classId != 2) {
                return 0xD;   // BAD_ITEM_CLASS
            }

            const bool* thrown = CoA::Config::Bool("CONFIG_THROWN_WEAPON_APPEARANCES_ENABLED");

            if (!(thrown && *thrown) && item.classId == 2 && item.subclassId == 0x10) {
                return 0xE;   // THROWN_APPEARANCE
            }
        } else if (type != TYPE_ILLUSION && type != TYPE_INCARNATION && type != TYPE_SPELL_VISUAL) {
            if (type == TYPE_COSMETIC_PET) {
                if (!s_deps.creatureKnown || !s_deps.creatureKnown(U32(a, 0x20))) {
                    return 0x11;   // BAD_CREATURE_TEMPLATE
                }

                const uint32_t tier = U32(cat, 0xC);

                if (tier > 9) {
                    return 0x15;   // PET_TYPE_NOT_FOUND
                }

                const uint32_t summon = PetSpell(tier, false);

                if (!SpellExists(summon)) {
                    return 0x16;   // PET_SUMMON_SPELL_NOT_FOUND
                }

                if (!KnowsSpell(summon)) {
                    return 0x17;   // PET_SUMMON_SPELL_NOT_LEARNED
                }

                const uint32_t dismiss = PetSpell(tier, true);

                if (!SpellExists(dismiss)) {
                    return 0x18;   // PET_DISMISS_SPELL_NOT_FOUND
                }

                if (!KnowsSpell(dismiss)) {
                    return 0x19;   // PET_DISMISS_SPELL_NOT_LEARNED
                }
            } else if (type != TYPE_COSMETIC) {
                return type == TYPE_ITEM_SET ? 10 : 0x1B;   // CANNOT_APPLY_ITEM_SET / UNSUPPORTED_CATEGORY
            }
        }

        if (!confirmed && type == TYPE_COSMETIC_PET) {
            return 0x1C;   // COSMETIC_PET_APPEARANCE_CONFIRMATION
        }

        return 0;
    }

    std::string ResultName(uint32_t code) {   // FUN_100be920
        return code < 29 ? kApplyResults[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code);
    }

    // FUN_100cb740: grow pending to category + 1 (zeros), set it, PENDING_APPEARANCE_CHANGED on change.
    void SetPending(uint32_t category, uint32_t appearance) {
        if (category >= s_m.pending.size()) {
            s_m.pending.resize(category + 1, 0);
        }

        if (s_m.pending[category] != appearance) {
            s_m.pending[category] = appearance;
            PendingChanged();
        }
    }

    // FUN_100cb8b0: an item set's parts into the pending ITEM categories whose inventory type is a set slot;
    // PENDING_APPEARANCE_CHANGED when anything changed.
    void SetPendingItemSet(const ItemSetRecord& set) {
        const std::vector<uint32_t> before = s_m.pending;
        std::vector<uint32_t> parts;
        ItemSetAppearanceList(set, parts);
        CoA::Dbc::Table& cats = Categories();

        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id) {
            const uint8_t* c = cats.Row(id);

            if (!c || CategoryType(c) != TYPE_ITEM || !SetSlot(U32(c, 0x10))) {
                continue;
            }

            const uint32_t idx = U32(c, 0);

            if (idx < parts.size() && idx < s_m.pending.size() && parts[idx] != s_m.pending[idx]) {
                s_m.pending[idx] = parts[idx];
            }
        }

        if (s_m.pending != before) {
            PendingChanged();
        }
    }

    // FUN_100cb4a0: (category, appearance) -- an ITEM_SET category takes the whole set (false when the appearance or its
    // set (+4) is unknown); anything else sets the one category. Pushes success.
    int SetPendingAppearance(lua_State* L) {
        int32_t cat32;
        int32_t app32;

        if (!ReadInt2(L, cat32, app32)) {
            return 0;
        }

        const uint32_t category = static_cast<uint32_t>(cat32);
        const uint32_t appearance = static_cast<uint32_t>(app32);
        bool ok = true;

        if (CategoryIs(category, TYPE_ITEM_SET)) {
            const uint8_t* a = Appearances().Row(appearance);
            ItemSetRecord set;

            if (a && FetchItemSet(U32(a, 4), set)) {
                SetPendingItemSet(set);
            } else {
                ok = false;
            }
        } else {
            SetPending(category, appearance);
        }

        PushBool(L, ok);
        return 1;
    }

    // FUN_100c7350: (category, appearance) -> ok, error name, and for NOT_COLLECTED the store game / checkout URLs
    // (else nil).
    int CanSetAppearance(lua_State* L) {
        int32_t cat32;
        int32_t app32;

        if (!ReadInt2(L, cat32, app32)) {
            return 0;
        }

        const uint32_t category = static_cast<uint32_t>(cat32);
        const uint32_t appearance = static_cast<uint32_t>(app32);
        std::vector<uint32_t> list = s_m.pending;
        std::vector<uint32_t> check;
        const uint8_t* a = Appearances().Row(appearance);
        ItemSetRecord set;
        const bool isSet = CategoryIs(category, TYPE_ITEM_SET);
        const bool haveSet = isSet && a && FetchItemSet(U32(a, 4), set);

        if (!isSet) {
            if (category < list.size()) {
                list[category] = appearance;
            }

            check.push_back(category);
        } else if (haveSet) {
            std::vector<uint32_t> parts;
            ItemSetAppearanceList(set, parts);

            for (uint32_t i = 0; i < parts.size(); ++i) {
                if (i < list.size()) {
                    list[i] = parts[i];
                }

                check.push_back(i);
            }
        }

        uint32_t err = 0;

        for (uint32_t c : check) {
            if ((err = Validate(list, c, true)) != 0) {
                break;
            }
        }

        PushBool(L, err == 0);

        if (err == 0) {
            lua_pushnil(L);
        } else {
            PushStr(L, ResultName(err).c_str());
        }

        PushStrOrNil(L, err == 9 ? StoreUrl(appearance, "game") : std::string());
        PushStrOrNil(L, err == 9 ? StoreUrl(appearance, "checkout") : std::string());
        return 4;
    }

    // FUN_100c6de0: (confirmed) -- every changed category must validate, else APPLY_PENDING_APPEARANCE_RESULT(false,
    // error) and false; then CMSG 0x697 {u32 n, u32 x n} and true.
    int ApplyPendingAppearances(lua_State* L) {
        if (!ValidateInput(L, { LUA_TBOOLEAN })) {
            return 0;
        }

        const bool confirmed = lua_toboolean(L, 1) != 0;

        for (uint32_t i = 0; i < s_m.pending.size(); ++i) {
            if (At(s_m.pending, i) != At(s_m.current, i)) {
                if (const uint32_t err = Validate(s_m.pending, i, confirmed)) {
                    CoA::Events::Signal("APPLY_PENDING_APPEARANCE_RESULT", "%b%s", false, ResultName(err).c_str());
                    PushBool(L, false);
                    return 1;
                }
            }
        }

        CoA::Packet p(0x697);
        p.U32(static_cast<uint32_t>(s_m.pending.size()));

        for (uint32_t v : s_m.pending) {
            p.U32(v);
        }

        p.Send();
        PushBool(L, true);
        return 1;
    }

    // handler_CanApplyPendingAppearances: the first failing category's error, else true, nil.
    int CanApplyPendingAppearances(lua_State* L) {
        CoA::Dbc::Table& cats = Categories();

        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id) {
            if (cats.Row(id)) {
                if (const uint32_t err = Validate(s_m.pending, id, true)) {
                    PushBool(L, false);
                    PushStr(L, err <= 0x1C ? kApplyResults[err] : "");
                    return 2;
                }
            }
        }

        PushBool(L, true);
        lua_pushnil(L);
        return 2;
    }

    // FUN_100c77c0: every category whose pending appearance fails validation goes back to current.
    int ClearInvalidPendingAppearances(lua_State*) {
        CoA::Dbc::Table& cats = Categories();

        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id) {
            if (cats.Row(id) && Validate(s_m.pending, id, true) != 0) {
                ClearPending(id);
            }
        }

        return 0;
    }

    // ---- display info / item sets / alternates -------------------------------------------------------------------
    // FUN_101b46e0: a UICameras row -- +0x10 directly, else for an ITEM display the item's camera (FUN_1021a020, shared
    // with C_UICamera.GetItemCameraID). The camera's id, false when there is none.
    bool DisplayCameraId(const uint8_t* a, int32_t& id) {
        if (const uint32_t cam = U32(a, 0x10)) {
            const uint8_t* row = CoA::Dbc::Get("DBFilesClient/UICameras.dbc").Row(cam);

            if (row) {
                id = I32(row, 0);
            }

            return row != nullptr;
        }

        return IsItemDisplay(a) && s_deps.itemCamera && s_deps.itemCamera(U32(a, 0xC), id);
    }

    // FUN_100c8090: display type, +0xC, camera id, name, source, and for a SPELL_VISUAL category the spell (+0x20); five
    // nils for an unknown appearance.
    int GetAppearanceDisplayInfo(lua_State* L) {
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        const uint8_t* a = Appearances().Row(appearance);

        if (!a) {
            return CoA::Script::PushNils(L, 5);
        }

        PushStr(L, Appearances().Str(a, 8));
        lua_pushinteger(L, I32(a, 0xC));
        int32_t camera = 0;

        if (DisplayCameraId(a, camera)) {
            lua_pushinteger(L, camera);
        } else {
            lua_pushnil(L);
        }

        PushStr(L, AppearanceName(a).c_str());
        lua_pushinteger(L, I32(a, 4));

        if (HasCategoryOfType(a, TYPE_SPELL_VISUAL)) {
            lua_pushinteger(L, I32(a, 0x20));
        } else {
            lua_pushnil(L);
        }

        return 6;
    }

    const std::vector<uint32_t>* SetsForItemAppearance(uint32_t appearance) {
        if (!s_setsForItemAppearanceBuilt) {
            s_setsForItemAppearanceBuilt = true;
            s_setsForItemAppearance.clear();
            uint32_t lo = 0;
            uint32_t hi = 0;

            if (s_deps.itemSetRange) {
                s_deps.itemSetRange(lo, hi);
            }

            for (uint32_t id = lo; id && id <= hi; ++id) {
                ItemSetRecord set;
                const uint8_t* sa = FetchItemSet(id, set) ? ItemSetAppearanceOf(id) : nullptr;

                if (!sa) {
                    continue;
                }

                for (uint32_t k = 0; k < 17; ++k) {
                    const uint32_t item = set.items[k];
                    const uint8_t* ia = item ? ItemAppearanceOf(item) : nullptr;

                    if (ia) {
                        s_setsForItemAppearance[U32(ia, 8)].push_back(U32(sa, 8));
                    }
                }
            }
        }

        auto it = s_setsForItemAppearance.find(appearance);
        return it == s_setsForItemAppearance.end() ? nullptr : &it->second;
    }

    // FUN_100c82e0: the first item set (with an ItemSet.dbc row) that uses this appearance: its name (nil if empty), set
    // id and set appearance; else three nils.
    int GetAppearanceItemSet(lua_State* L) {
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        if (Appearances().Row(appearance)) {
            if (const std::vector<uint32_t>* sets = SetsForItemAppearance(appearance)) {
                for (uint32_t sid : *sets) {
                    const uint8_t* sa = Appearances().Row(sid);
                    ItemSetRecord set;

                    if (!sa || !FetchItemSet(U32(sa, 4), set)) {
                        continue;
                    }

                    PushStrOrNil(L, set.name);
                    lua_pushinteger(L, static_cast<int>(set.id));
                    lua_pushinteger(L, static_cast<int>(sid));
                    return 3;
                }
            }
        }

        return CoA::Script::PushNils(L, 3);
    }

    // FUN_100c7c70: (appearance, item) -> the other items with that appearance.
    int GetAlternativeIDs(lua_State* L) {
        int32_t app32;
        int32_t item32;

        if (!ReadInt2(L, app32, item32)) {
            return 0;
        }

        std::vector<uint32_t> out;

        if (const std::vector<const uint8_t*>* rows = ItemsForAppearance(static_cast<uint32_t>(app32))) {
            for (const uint8_t* r : *rows) {
                if (I32(r, 4) != item32) {
                    out.push_back(U32(r, 4));
                }
            }
        }

        PushIntList(L, out);
        return 1;
    }

    // FUN_100c9de0: an appearance whose every category is an INCARNATION with tier 2 and with +0x24 set -> {+0x24}; else
    // nil.
    int GetCreatureDisplayItems(lua_State* L) {
        uint32_t appearance;

        if (!ReadNumber(L, appearance)) {
            return 0;
        }

        const uint8_t* a = Appearances().Row(appearance);
        bool ok = a != nullptr;

        for (uint32_t k = 0; ok && k < 3; ++k) {
            const uint32_t c = U32(a, 0x14 + k * 4);

            if (!c) {
                continue;
            }

            const uint8_t* cat = Categories().Row(c);
            ok = cat && CategoryType(cat) == TYPE_INCARNATION && U32(cat, 0xC) == 2;
        }

        if (ok && U32(a, 0x24)) {
            lua_createtable(L, 0, 1);
            lua_checkstack(L, 2);
            lua_pushnumber(L, 1.0);
            lua_pushinteger(L, I32(a, 0x24));
            lua_settable(L, -3);
            return 1;
        }

        lua_pushnil(L);
        return 1;
    }

    // ---- C_ItemSet -----------------------------------------------------------------------------------------------
    // FUN_100c8af0: (item set) -> its per-category appearances without index 0, or nil.
    int GetAppearances(lua_State* L) {
        uint32_t id;

        if (!ReadNumber(L, id)) {
            return 0;
        }

        ItemSetRecord set;

        if (!FetchItemSet(id, set)) {
            lua_pushnil(L);
            return 1;
        }

        std::vector<uint32_t> list;
        ItemSetAppearanceList(set, list);

        if (!list.empty()) {
            list.erase(list.begin());
        }

        PushIntList(L, list);
        return 1;
    }

    // FUN_100ca370: (item set) -> collected, total over its non-zero appearances; nil, nil if unknown.
    int GetItemSetNumCollected(lua_State* L) {
        uint32_t id;

        if (!ReadNumber(L, id)) {
            return 0;
        }

        ItemSetRecord set;

        if (!FetchItemSet(id, set)) {
            return CoA::Script::PushNils(L, 2);
        }

        std::vector<uint32_t> list;
        ItemSetAppearanceList(set, list);
        int total = 0;
        int collected = 0;

        for (uint32_t a : list) {
            if (a) {
                ++total;

                if (IsCollected(a)) {
                    ++collected;
                }
            }
        }

        lua_pushinteger(L, collected);
        lua_pushinteger(L, total);
        return 2;
    }

    void OnGlueScreen() {   // 0x100C2170
        s_m.pending.clear();
        s_browserDirty = true;   // +0xC container reset (FUN_100c6670)
        s_m.unk144 = 0;
        s_m.current.clear();
        s_m.pendingOutfitName.clear();
        s_m.pendingOutfit.clear();
    }

    // 0x100C6410 (list 0x10BE2B6C, before 0x724820): with other players' appearances hidden (+0x1A5), a spell flagged
    // SpellCustomAttr +0x14 & 0x2000000 is not applied to anyone but the player (FUN_10324a10).
    bool BeforeAuraApply(void* unit, uint32_t, const void* rec) {
        if ((s_deps.isActivePlayer && s_deps.isActivePlayer(unit)) || s_m.seeOthers) {   // 0x4CEE50
            return true;
        }

        uint32_t spell;
        memcpy(&spell, rec, 4);
        const uint8_t* attr = CoA::CA::SpellCustomAttrRow(spell);
        return !(attr && (U32(attr, 0x14) & 0x2000000));
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("C_Appearance", "GetAppearanceForCategory", GetAppearanceForCategory),
        COA_NATIVE("C_Appearance", "GetItemAppearanceID", GetItemAppearanceID),
        COA_NATIVE("C_Appearance", "GetItemSetAppearanceID", GetItemSetAppearanceID),
        COA_NATIVE("C_Appearance", "HasPendingAppearances", HasPendingAppearances),
        COA_NATIVE("C_Appearance", "GetPendingAppearance", GetPendingAppearance),
        COA_NATIVE("C_Appearance", "ClearPendingAppearance", ClearPendingAppearance),
        COA_NATIVE("C_Appearance", "ClearPendingAppearances", ClearPendingAppearances),
        COA_NATIVE("C_Appearance", "CanSeeAppearances", CanSeeAppearances),
        COA_NATIVE("C_Appearance", "SetCanSeeAppearances", SetCanSeeAppearances),
        COA_NATIVE("C_Appearance", "IsTransmogable", IsTransmogable),
        COA_NATIVE("C_Appearance", "GetActiveDiscount", GetActiveDiscount),
        COA_NATIVE("C_Appearance", "IsEtherealBazaarAppearance", IsEtherealBazaarAppearance),
        COA_NATIVE("C_Appearance", "GetAppearanceDetails", GetAppearanceDetails),
        COA_NATIVE("C_Appearance", "GetAppearanceWebURL", GetAppearanceWebURL),
        COA_NATIVE("C_Appearance", "GetEtherealBazaarWebURL", GetEtherealBazaarWebURL),
        COA_NATIVE("C_Appearance", "SetPendingAppearance", SetPendingAppearance),
        COA_NATIVE("C_Appearance", "CanSetAppearance", CanSetAppearance),
        COA_NATIVE("C_Appearance", "ApplyPendingAppearances", ApplyPendingAppearances),
        COA_NATIVE("C_Appearance", "CanApplyPendingAppearances", CanApplyPendingAppearances),
        COA_NATIVE("C_Appearance", "ClearInvalidPendingAppearances", ClearInvalidPendingAppearances),
        COA_NATIVE("C_Appearance", "GetAppearanceDisplayInfo", GetAppearanceDisplayInfo),
        COA_NATIVE("C_Appearance", "GetAppearanceItemSet", GetAppearanceItemSet),
        COA_NATIVE("C_Appearance", "GetAlternativeIDs", GetAlternativeIDs),
        COA_NATIVE("C_Appearance", "GetCreatureDisplayItems", GetCreatureDisplayItems),
        COA_NATIVE("C_ItemSet", "GetAppearances", GetAppearances),
        COA_NATIVE("C_ItemSet", "GetItemSetNumCollected", GetItemSetNumCollected),
    };
}

// FUN_10322c70's branch for the active player: the first AppearanceCategories.dbc row (by id) of APPEARANCE_TYPE 4
// whose +0xC or +0x10 is `spell` gives the category (0 when none); the appearance applied there (FUN_100c39f0 over
// +0x148) names an Appearances.dbc row whose +0x20 is the visual. False (the caller falls back to its map) when there is
// no applied appearance or no such row.
bool CoA::Appearance::PlayerSpellVisual(uint32_t spell, uint32_t& visual) {
    uint32_t category = 0;
    CoA::Dbc::Table& cats = Categories();

    for (uint32_t id = cats.MinId(); cats.Loaded() && id <= cats.MaxId() && !category; ++id) {
        const uint8_t* row = cats.Row(id);

        if (row && CategoryType(row) == 4 && (U32(row, 0xC) == spell || U32(row, 0x10) == spell)) {
            category = U32(row, 0);
        }
    }

    const uint32_t appearance = At(s_m.current, category);
    const uint8_t* row = appearance ? Appearances().Row(appearance) : nullptr;

    if (!row) {
        return false;
    }

    visual = U32(row, 0x20);
    return true;
}

// The refreshes the SMSG_PATCH_* handlers run: 0x693 (FUN_1020fc80 then FUN_101b5660) and 0x6EC (FUN_102101d0 then
// FUN_101b5660).
void CoA::Appearance::ItemAppearancesPatched() {
    BuildItemAppearanceIndex();
    s_setsForItemAppearanceBuilt = false;
}

// FUN_1020fae0 -> FUN_100c57a0 -> FUN_100c6450: the appearance's categories +0x14..+0x1C, the first with an
// AppearanceCategories row ends the search; UNLOCKED_APPEARANCE_ITEM_USED("%u%s%u%s": category, its APPEARANCE_TYPE_*
// name, appearance, item name).
void CoA::Appearance::ItemUsed(uint32_t entry, const char* itemName) {
    const uint8_t* itemAppearance = ItemAppearanceOf(entry);

    if (!itemAppearance) {
        return;
    }

    const uint8_t* appearance = Appearances().Row(U32(itemAppearance, 8));

    if (!appearance || !IsCollected(U32(appearance, 0))) {
        return;
    }

    for (uint32_t off = 0x14; off < 0x20; off += 4) {
        const uint32_t category = U32(appearance, off);
        const uint8_t* row = Categories().Row(category);

        if (!row) {
            continue;
        }

        const int type = CategoryType(row);

        if (type >= 2 && type <= 6) {
            CoA::Events::Signal("UNLOCKED_APPEARANCE_ITEM_USED", "%u%s%u%s", category, Categories().Str(row, 4),
                U32(appearance, 0), itemName);
        }

        return;
    }
}

void CoA::Appearance::ItemSetAppearancesPatched() {
    BuildItemSetAppearanceIndex();
    s_setsForItemAppearanceBuilt = false;
}

void CoA::Appearance::Init() {   // 0x100C5AE0
    CoA::ClientHooks::addAuraEffect.Add(BeforeAuraApply, 0x100C5B88);
    COA_PACKET(0x698, OnApplyResult);
    COA_PACKET(0x699, OnCollectedList);
    COA_PACKET(0x69A, OnCurrent);
    COA_PACKET(0x69B, OnCollected);
    COA_PACKET(0x69C, OnUncollected);
    COA_PACKET(0x69D, OnOutfits);
    COA_PACKET(0x69F, OnOutfitSaved);
    COA_PACKET(0x6A1, OnOutfitDeleted);
    COA_PACKET(0x6A2, OnVisibility);
    COA_PACKET(0x6EB, OnCollectItemResult);
    CoA::Lifecycle::OnGlueScreen(OnGlueScreen);
    COA_EVENT("APPLY_PENDING_APPEARANCE_RESULT");
    COA_EVENT("APPEARANCE_COLLECTED");
    COA_EVENT("APPEARANCE_UNCOLLECTED");
    COA_EVENT("APPEARANCE_OUTFIT_SAVED_RESULT");
    COA_EVENT("APPEARANCE_OUTFIT_DELETED_RESULT");
    COA_EVENT("UNLOCKED_APPEARANCE_ITEM_USED");
    COA_EVENT("PENDING_APPEARANCE_CHANGED");
    COA_EVENT("VIEWABLE_APPEARANCES_RESET");
    COA_EVENT("COLLECT_ITEM_APPEARANCE_RESULT");
    CoA::PatchData::Deps& patch = CoA::PatchData::Dependencies();
    patch.itemAppearancesPatched = CoA::Appearance::ItemAppearancesPatched;
    patch.itemSetAppearancesPatched = CoA::Appearance::ItemSetAppearancesPatched;
    patch.rebuildOutfits = CoA::Appearance::RebuildOutfits;
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
    InitBrowser();
}

void CoA::Appearance::Reset() {
    s_m = Manager();
    s_browserDirty = true;
    s_deps = Deps();
    s_itemAppearanceByItem.clear();
    s_itemSetAppearanceBySet.clear();
    s_itemsForAppearance.clear();
    s_setsByAppearance.clear();
    s_itemIndexBuilt = false;
    s_setIndexBuilt = false;
    s_setsForItemAppearance.clear();
    s_setsForItemAppearanceBuilt = false;
    s_setsWithAppearance.clear();
    ResetBrowser();
}
