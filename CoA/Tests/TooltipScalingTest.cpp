#include "CoA/Source/Modules/BridgeStore.h"
#include "CoA/Source/Modules/GameEvents.h"
#include "CoA/Source/Modules/ItemStats.h"
#include "CoA/Source/Modules/ObjectAddon.h"
#include "CoA/Source/Modules/RealmInfo.h"
#include "CoA/Source/Modules/TooltipScaling.h"
#include "CoA/Source/Runtime/ClientHooks.h"
#include "CoA/Source/Runtime/Dbc.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Packets.h"
#include "CoA/Tests/Support.h"
#include "CoA/Tests/TestMain.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
    using TS = CoA::TooltipScaling::Line;

    const uint64_t kPlayer = 0x0000000000000777ull;
    const uint64_t kItemA = 0x4000000000000011ull;
    const uint64_t kItemB = 0x4000000000000022ull;

    // ItemAddon rows: 9001 flags 0 (level 0), 9002 flags 0x20 (the context's level, else the player's), 9003 flags 0x200
    // (the player's level inside a Manastorm only, else 0); reborn armor (field 7) 77 / 0 / 0.
    std::vector<uint32_t> AddonRow(uint32_t rowId, uint32_t item, uint32_t flags, uint32_t reborn) {
        std::vector<uint32_t> row = { rowId, item };
        std::vector<uint32_t> n = CoATest::Loc(0);
        row.insert(row.end(), n.begin(), n.end());
        row.insert(row.end(), n.begin(), n.end());
        std::vector<uint32_t> fields(12, 0);
        fields[1] = flags;
        fields[7] = reborn;
        row.insert(row.end(), fields.begin(), fields.end());
        return row;
    }

    // QuestTemplateScaling rows: id, lo, hi, 5 thresholds, 5 bonuses.
    bool ReadTables(const char* path, std::vector<uint8_t>& out) {
        if (strstr(path, "ItemAddon.dbc")) {
            out = CoATest::Wdbc({ AddonRow(1, 9001, 0, 77), AddonRow(2, 9002, 0x20, 0), AddonRow(3, 9003, 0x200, 0) }, std::string());
            return true;
        }

        if (strstr(path, "QuestTemplateScaling.dbc")) {
            out = CoATest::Wdbc({ { 50, 10, 40, 30, 20, 0, 0, 0, 0, 5, 2, 0, 0, 0 } }, std::string());
            return true;
        }

        return false;
    }

    // A 0xA8-byte scaled record.
    struct Rec {
        uint32_t d[0x2A] = {};

        Rec(uint32_t item, uint32_t level) {
            d[0] = item;
            d[1] = level;
            d[3] = level;               // +0x0C its item level
            for (uint32_t i = 0; i < 10; ++i) {
                d[4 + i * 2] = 0xFFFFFFFF;
            }
        }

        Rec& Stat(uint32_t i, int32_t type, int32_t value) {
            d[4 + i * 2] = static_cast<uint32_t>(type);
            d[5 + i * 2] = static_cast<uint32_t>(value);
            return *this;
        }

        Rec& Damage(uint32_t pair, float min, float max) {
            memcpy(&d[0x18 + pair * 3], &min, 4);
            memcpy(&d[0x19 + pair * 3], &max, 4);
            return *this;
        }

        Rec& At(uint32_t off, uint32_t v) {
            d[off / 4] = v;
            return *this;
        }

        void Insert() const {
            CoA::ItemStats::InsertLoaded(reinterpret_cast<const uint8_t*>(d));
        }
    };

    bool s_havePlayer = true;
    CoA::TooltipScaling::Player s_player;
    std::map<uint32_t, CoA::TooltipScaling::CacheItem> s_cache;
    bool s_lootUnit = false;
    uint32_t s_lootLevel = 0;
    bool s_inspect = false;
    uint32_t s_tradeLevels[7] = {};
    std::map<uint64_t, uint32_t> s_items;
    std::map<std::string, std::string> s_strings;
    std::vector<std::pair<uint8_t, const char*>> s_classes;
    float s_qualityMult = 1.0f;

    void Start() {
        CoA::Dbc::SetFileReader(ReadTables);
        CoA::Dbc::Get("DBFilesClient\\QuestTemplateScaling.dbc").Unload();
        CoA::BridgeStore::Reset();
        CoA::BridgeStore::LoadTables();
        CoA::BridgeStore::BuildIndexes();
        CoA::GameEvents::Reset();
        CoA::RealmInfo::Reset();
        CoA::ObjectAddon::Reset();
        CoA::ItemStats::Reset();
        CoA::TooltipScaling::Reset();
        CoA::ClientHooks::Reset();
        CoA::Packets::Clear();
        CoA::LuaReg::Reset();
        s_havePlayer = true;
        s_player = CoA::TooltipScaling::Player();
        s_player.guid = kPlayer;
        s_player.level = 10;
        s_player.cls = 1;
        s_cache.clear();
        s_lootUnit = false;
        s_lootLevel = 0;
        s_inspect = false;
        memset(s_tradeLevels, 0, sizeof(s_tradeLevels));
        s_items.clear();
        s_qualityMult = 1.0f;
        s_strings = {
            { "ITEM_CLASSES_ALLOWED", "Classes: %s" }, { "DAMAGE_TEMPLATE_WITH_SCHOOL", "%d - %d %s Damage" },
            { "DAMAGE", "Damage" }, { "DAMAGE_TEMPLATE", "%d - %d Damage" }, { "ARMOR", "Armor" }, { "BLOCK", "Block" },
            { "ITEM_LEVEL", "Item Level %d" }, { "ITEM_MIN_LEVEL", "Requires Level %d" }, { "DAMAGE_SCHOOL3", "Fire" },
            { "RESISTANCE2_NAME", "Fire Resistance" }, { "PVE_POWER_LABEL", "PvE Power" }, { "PVP_POWER_LABEL", "PvP Power" },
        };
        s_classes = { { 1, "Warrior" }, { 2, "Paladin" }, { 10, "Hero" }, { 12, "Barbarian" }, { 13, "Witch" } };
        CoA::TooltipScaling::Deps& d = CoA::TooltipScaling::Dependencies();
        d.itemCache = [](uint32_t item, CoA::TooltipScaling::CacheItem& out) {
            auto it = s_cache.find(item);

            if (it == s_cache.end()) {
                return false;
            }

            out = it->second;
            return true;
        };
        d.player = [](CoA::TooltipScaling::Player& out) { out = s_player; return s_havePlayer; };
        d.lootUnitLevel = [](uint32_t& level) { level = s_lootLevel; return s_lootUnit; };
        d.inspectTargetExists = [] { return s_inspect; };
        d.tradeLevel = [](uint32_t slot) { return slot < 7 ? s_tradeLevels[slot] : 0u; };
        d.itemEntry = [](uint64_t guid, uint32_t& entry) {
            auto it = s_items.find(guid);

            if (it == s_items.end()) {
                return false;
            }

            entry = it->second;
            return true;
        };
        d.globalString = [](const char* key, int32_t) -> const char* {
            auto it = s_strings.find(key);
            return it == s_strings.end() ? nullptr : it->second.c_str();
        };
        d.classes = [] { return s_classes; };
        d.durabilityCost = [](uint32_t level, uint32_t column, uint32_t& cost) {
            if (level == 0 || level > 100) {
                return false;
            }

            cost = level * 100 + column;
            return true;
        };
        d.durabilityQuality = [](uint32_t, float& m) { m = s_qualityMult; return true; };
        CoA::TooltipScaling::Init();
    }

    void Stop() {
        CoA::TooltipScaling::Reset();
        CoA::ItemStats::Reset();
        CoA::ObjectAddon::Reset();
        CoA::ClientHooks::Reset();
        CoA::BridgeStore::Reset();
        CoA::Dbc::Get("DBFilesClient\\QuestTemplateScaling.dbc").Unload();
        CoA::Dbc::SetFileReader(nullptr);
        CoA::Packets::Clear();
        CoA::LuaReg::Reset();
    }

    void Scales() {
        CoA::ObjectAddon::Unit(kPlayer)[87].value = 1;
    }

    // SetItem's context around a call: before, the callback, after.
    template <class F>
    void WithItem(uint32_t item, const uint64_t* itemGuid, F f) {
        int32_t skipLock = 1;
        CoA::TooltipScaling::BeforeSetItem(item, nullptr, itemGuid, skipLock);
        f();
        CoA::TooltipScaling::AfterSetItem();
    }

    const uint8_t* Scaled(uint32_t item, const uint64_t* guid = nullptr) {
        const uint8_t* r = nullptr;
        WithItem(item, guid, [&r] { r = CoA::TooltipScaling::ScaledRecord(); });
        return r;
    }

    uint32_t LevelOf(const uint8_t* r) {
        uint32_t v = 0;

        if (r) {
            memcpy(&v, r + 4, 4);
        }

        return v;
    }
}

// FUN_1027eee0's brackets: the player's level + 2 / 3 / 4, 61..70 shifted, 70..80 + 35, else + 5; only while the
// player's addon field 87 is set; nothing for an item without an addon row.
COA_TEST(TooltipScaling_ScaledRecordBrackets) {
    Start();
    struct Case {
        uint32_t player;
        uint32_t scaled;
    } const kCases[] = { { 0, 2 }, { 20, 22 }, { 21, 24 }, { 30, 33 }, { 31, 35 }, { 50, 54 }, { 51, 56 }, { 60, 65 },
        { 61, 86 }, { 62, 88 }, { 63, 91 }, { 64, 94 }, { 65, 96 }, { 66, 98 }, { 67, 99 }, { 68, 101 }, { 69, 103 },
        { 70, 105 }, { 80, 115 }, { 81, 86 } };

    for (const Case& c : kCases) {
        Rec(9002, c.scaled).Insert();
    }

    CHECK(Scaled(9002) == nullptr);   // field 87 clear: stock
    Scales();

    for (const Case& c : kCases) {
        s_player.level = c.player;
        CHECK(LevelOf(Scaled(9002)) == c.scaled);
    }

    CHECK(Scaled(5555) == nullptr);   // no addon row
    s_havePlayer = false;
    CHECK(Scaled(9002) == nullptr);      // no player: no field 87 to scale by
    Stop();
}

// The context's level for flag 0x20: a loot roll, the looted unit, the inspected slot, else the player's; flag 0x200
// takes the player's only in a Manastorm. The item's own level (ObjectAddon) and a guild bank level go first; a trade
// slot next; an auction level before the addon flags.
COA_TEST(TooltipScaling_ScaledRecordContext) {
    Start();
    Scales();
    s_player.level = 25;   // -> 28
    for (uint32_t lvl : { 28u, 7u, 44u, 33u, 13u, 50u, 61u, 99u, 2u }) {
        Rec(9002, lvl).Insert();
        Rec(9002, lvl).Insert();
        Rec(9003, lvl).Insert();
    }

    CHECK(LevelOf(Scaled(9002)) == 28);
    s_inspect = true;
    CoATest::Bytes inspect;
    for (uint32_t i = 0; i < 19; ++i) {
        inspect.U32(i == 4 ? 30 : 0);
    }
    CHECK(CoATest::Deliver(CoA::TooltipScaling::OnInspectLevels, 0x716, inspect) == 1);
    CHECK(CoA::TooltipScaling::InspectLevel(4) == 30);
    CoA::ItemStats::InspectedSlot() = 4;
    CHECK(LevelOf(Scaled(9002)) == 33);           // 30 -> 33
    s_lootUnit = true;
    s_lootLevel = 40;
    CHECK(LevelOf(Scaled(9002)) == 44);           // the looted unit first
    CoA::ItemStats::LootRollLevels() = { 0, 5 };
    lua_State* L = luaL_newstate();
    lua_pushnil(L);
    lua_pushnumber(L, 1);
    CoA::TooltipScaling::BeginLootRoll(L);
    CHECK(LevelOf(Scaled(9002)) == 7);            // roll 1: level 5 -> 7
    CoA::TooltipScaling::EndLootRoll();
    CHECK(LevelOf(Scaled(9002)) == 44);
    lua_close(L);

    CHECK(LevelOf(Scaled(9003)) == 2);            // flag 0x200 outside a Manastorm: level 0 (not the player's) -> 2

    // The item's own level beats everything else.
    CoA::ObjectAddon::Item(kItemA)[0].value = 50;
    CHECK(LevelOf(Scaled(9002, &kItemA)) == 50);
    CHECK(LevelOf(Scaled(5555, &kItemA)) == 0);   // no record for 5555 at 50: the store asks, nothing yet
    CoA::TooltipScaling::SetGuildBankLevel(61);
    CHECK(LevelOf(Scaled(9002, &kItemA)) == 61);
    CHECK(LevelOf(Scaled(9002, &kItemA)) == 50);  // cleared after the item
    Stop();
}

COA_TEST(TooltipScaling_TradeAuctionQuest) {
    Start();
    Scales();
    Rec(9002, 13).Insert();
    Rec(5555, 13).Insert();
    s_tradeLevels[2] = 13;
    lua_State* L = luaL_newstate();
    lua_pushnil(L);
    lua_pushnumber(L, 3);
    CoA::TooltipScaling::BeginTrade(L);
    CHECK(LevelOf(Scaled(5555)) == 13);           // a trade needs no addon row
    CoA::TooltipScaling::EndTrade(false);         // the player's: the flag stays, the slot is 0
    CHECK(Scaled(5555) == nullptr);               // slot 0 level 0
    lua_settop(L, 0);
    lua_pushnil(L);
    lua_pushnumber(L, 3);
    CoA::TooltipScaling::BeginTrade(L);
    CoA::TooltipScaling::EndTrade(true);
    s_player.level = 50;
    Rec(9002, 54).Insert();
    CHECK(LevelOf(Scaled(9002)) == 54);
    lua_close(L);

    // Quest scaling: row 50 {lo 10, hi 40; level >= 30 +5, >= 20 +2}.
    Rec(9002, 40).Insert();
    Rec(9002, 27).Insert();
    s_player.level = 35;
    CoA::TooltipScaling::SetQuestContext(50);
    CHECK(LevelOf(Scaled(9002)) == 40);           // 35 + 5 capped at 40
    s_player.level = 25;
    CoA::TooltipScaling::SetQuestContext(50);
    CHECK(LevelOf(Scaled(9002)) == 27);           // 25 + 2
    CoA::TooltipScaling::SetQuestContext(51);     // no row: the bracket (25 -> 28) applies
    Rec(9002, 28).Insert();
    CHECK(LevelOf(Scaled(9002)) == 28);
    Stop();
}

// sub_1027c870: skipLock cleared with a guild bank level; the item-set jump forced for a down-scaled item and for an
// epic 70 at cap 80 off Dev; the comparison jump while a hyperlink compare runs; all restored after.
COA_TEST(TooltipScaling_SetItemContext) {
    Start();
    Scales();
    Rec(9002, 12).Insert();
    CoA::TooltipScaling::CacheItem c;
    c.itemLevel = 20;
    s_cache[9002] = c;
    int32_t skipLock = 1;
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, nullptr, skipLock);
    CHECK(skipLock == 1);
    CHECK(!CoA::ClientHooks::options.tooltipItemSet);   // 12 < 20
    CHECK(CoA::ClientHooks::options.itemComparison);
    CoA::TooltipScaling::AfterSetItem();
    CHECK(CoA::ClientHooks::options.tooltipItemSet && CoA::ClientHooks::options.itemComparison);

    CoA::TooltipScaling::SetGuildBankLevel(12);
    CHECK(CoA::TooltipScaling::BeginHyperlinkCompare(false));
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, &kItemA, skipLock);
    CHECK(skipLock == 0);
    CHECK(!CoA::ClientHooks::options.itemComparison);
    CHECK(CoA::TooltipScaling::TooltipGuid() == kItemA);
    CoA::TooltipScaling::AfterSetItem();
    CoA::TooltipScaling::EndHyperlinkCompare();
    CHECK(CoA::TooltipScaling::TooltipGuid() == 0);
    CHECK(!CoA::TooltipScaling::BeginHyperlinkCompare(true));

    // Unscaled (field 87 clear) with the cache: nothing forced -- stock.
    CoA::ObjectAddon::Unit(kPlayer)[87].value = 0;
    skipLock = 1;
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, nullptr, skipLock);
    CHECK(CoA::ClientHooks::options.tooltipItemSet && CoA::ClientHooks::options.itemComparison && skipLock == 1);
    CoA::TooltipScaling::AfterSetItem();
    Stop();
}

// FUN_1027b110 with nothing scaled: every line passes as given (the two-handed list is shortened regardless).
COA_TEST(TooltipScaling_AddLineStockPasses) {
    Start();
    for (const char* line : { "+10 Stamina", "Item Level 60", "Requires Level 60", "10 - 20 Damage", "Equip: Increases spell power by 5.",
             "Durability 50 / 50" }) {
        TS l;
        WithItem(9002, nullptr, [&] { l = CoA::TooltipScaling::AddLine(line, nullptr, 0xFFFFFFFF); });
        CHECK(l.kind == TS::Pass && l.hasLeft && l.left == line);
    }

    const TS two = CoA::TooltipScaling::AddLine("Requires Two-Handed Axes, Two-Handed Maces, Polearms, Two-Handed Swords, Staves",
        nullptr, 0);
    CHECK(two.kind == TS::Pass && two.left == "需要近战双手武器");
    const TS none = CoA::TooltipScaling::AddLine(nullptr, "right", 0);
    CHECK(none.kind == TS::Pass && !none.hasLeft);
    Stop();
}

COA_TEST(TooltipScaling_AddLineRewrites) {
    Start();
    Scales();
    s_player.level = 10;   // -> 12
    Rec r(9002, 12);
    r.Stat(0, 7, 25).Stat(1, 0x2D, 40).Damage(0, 11.9f, 22.2f).Damage(1, 3.0f, 6.0f).At(0x78, 300).At(0x7C, 350).At(0x84, 15)
        .At(0x98, 9).At(0xA0, 11).At(0xA4, 1234).At(0xC, 33);
    r.Insert();
    CoA::TooltipScaling::CacheItem c;
    c.delay = 2000;
    s_cache[9002] = c;
    auto Line = [](const char* text, uint32_t colour = 0xFFFFFFFF) {
        return CoA::TooltipScaling::AddLine(text, nullptr, colour);
    };

    int32_t skipLock = 0;
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, nullptr, skipLock);
    CHECK(Line("+10 Stamina").left == "+25 Stamina");
    CHECK(Line("+10 Stamina", 0xFF00FF00).kind == TS::Pass);              // a green line is not rewritten
    CHECK(Line("5 Fire").left == "+15 Fire Resistance");                  // the school becomes its resistance
    CHECK(Line("100 Armor").left == "350 Armor");                         // stock class: the reborn armor
    CHECK(Line("100 Armor", 0xFF00FF00).kind == TS::Pass);                // green, and the first armor is done
    CHECK(Line("3 Block").left == "9 Block");
    CHECK(Line("10 - 20 Damage").left == "11 - 22 Damage");
    CHECK(Line("1 - 2 Fire Damage").left == "3 - 6 Fire Damage");
    CHECK(Line("1 - 2 Arcane Damage").left == "3 - 6 Arcane Damage");     // "1 - 2 Damage" reads school "Damage": skipped
    CHECK(Line("(5.0 damage per second)").left == "(10.5 damage per second)");   // (11+22+3+6)/2 / 2s
    CHECK(Line("Item Level 60").left == "Item Level 33");
    const TS req = Line("Requires Level 60");
    CHECK(req.left == "Requires Level 11" && req.colour == 2);            // 11 > 10: red
    Rec(9002, 13).At(0xA0, 11).Insert();
    s_player.level = 11;                                                  // -> the level-13 record
    CHECK(Line("Requires Level 60").colour == 1);
    s_player.level = 10;
    CHECK(Line("Equip: Increases spell power by 7.").left == "Equip: Increases spell power by 40.");
    CHECK(Line("Equip: Improves spell power by 7.").left == "Equip: Improves spell power by 40.");
    CHECK(Line("Equip: Restores 3 Stamina.").left == "Equip: Restores 25 Stamina");
    CHECK(Line("Equip: Increases PvE Power by 7.").kind == TS::Drop);     // under the cap, not Dev
    CHECK(Line("Equip: Increases nothing by 7.").kind == TS::Pass);
    CoA::TooltipScaling::AfterSetItem();

    s_player.cls = 10;  // a Hero class: plain armor
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, nullptr, skipLock);
    CHECK(Line("100 Armor").left == "300 Armor");
    CoA::TooltipScaling::AfterSetItem();
    Stop();
}

// FUN_1027f210: the Classes line keeps the classes of the player's own model; dropped when all of them are listed.
COA_TEST(TooltipScaling_ClassesLine) {
    Start();
    s_player.cls = 1;
    CHECK(CoA::TooltipScaling::AddLine("Classes: Warrior, Paladin", nullptr, 0).kind == TS::Drop);
    CHECK(CoA::TooltipScaling::AddLine("Classes: Paladin, Barbarian, Warrior", nullptr, 0).kind == TS::Drop);
    const TS one = CoA::TooltipScaling::AddLine("Classes: Paladin, Witch", nullptr, 0);
    CHECK(one.kind == TS::Rewrite && one.left == "Classes: Paladin");
    s_player.cls = 12;
    const TS coa = CoA::TooltipScaling::AddLine("Classes: Warrior, Witch", nullptr, 0);
    CHECK(coa.kind == TS::Rewrite && coa.left == "Classes: Witch");
    s_player.cls = 10;
    CHECK(CoA::TooltipScaling::AddLine("Classes: Hero", nullptr, 0).kind == TS::Drop);
    s_havePlayer = false;
    CHECK(CoA::TooltipScaling::AddLine("Classes: Warrior", nullptr, 0).kind == TS::Drop);
    Stop();
}

COA_TEST(TooltipScaling_MoneyFeralRepairCanEquip) {
    Start();
    Scales();
    Rec r(9002, 12);
    r.Damage(0, 100.0f, 140.0f).At(0xA4, 999);
    r.Insert();
    CHECK(CoA::TooltipScaling::Money(50) == 50);   // no item: stock
    WithItem(9002, nullptr, [] { CHECK(CoA::TooltipScaling::Money(50) == 999); });

    CoA::TooltipScaling::CacheItem w;
    w.cls = 2;
    w.inventoryType = 0x11;
    w.delay = 2000;
    float damage = 10.0f;
    int32_t ap = -1;
    WithItem(9002, nullptr, [&] { CHECK(CoA::TooltipScaling::FeralAttackPower(w, damage, ap)); });
    CHECK(damage == 120.0f);                      // the scaled average
    CHECK(ap == static_cast<int32_t>((60.0f - 54.81f) * 14.0f + 200.0f));
    w.inventoryType = 0x10;
    damage = 10.0f;
    CHECK(!CoA::TooltipScaling::FeralAttackPower(w, damage, ap) && damage == 10.0f);

    CoA::TooltipScaling::CacheItem armor;
    armor.cls = 4;
    armor.subClass = 2;
    armor.itemLevel = 20;
    s_cache[9002] = armor;
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 10, 10) == 0);
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 11, 10) == 0);
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 5, 0) == 0);
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 7, 10) == (20 * 100 + 0x17) * 3);   // the cache's level
    CoA::ObjectAddon::Item(kItemA)[0].value = 30;
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 7, 10) == (30 * 100 + 0x17) * 3);   // the item's own
    s_qualityMult = 0.0001f;
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 7, 10) == 1);
    CoA::TooltipScaling::CacheItem other;
    other.cls = 15;
    other.subClass = 3;
    s_cache[9002] = other;
    s_qualityMult = 1.0f;
    CHECK(CoA::TooltipScaling::RepairCost(9002, kItemA, 9, 10) == 30 * 100);              // column 0
    CHECK(CoA::TooltipScaling::RepairCost(5555, kItemA, 9, 10) == 0);

    s_player.level = 25;
    CoA::ObjectAddon::Item(kItemB)[0].value = 30;
    CHECK(CoA::TooltipScaling::CanEquip(1, 40, true, kItemB) == 1);
    CHECK(CoA::TooltipScaling::CanEquip(0, 40, true, kItemB) == 1);    // 30 - 5 <= 25
    CoA::ObjectAddon::Item(kItemB)[0].value = 31;
    CHECK(CoA::TooltipScaling::CanEquip(0, 40, true, kItemB) == 0);
    CHECK(CoA::TooltipScaling::CanEquip(0, 40, false, kItemB) == 0);
    CoA::ObjectAddon::Item(kItemB)[0].value = 3;                        // 3 - 5 wraps
    CHECK(CoA::TooltipScaling::CanEquip(0, 40, true, kItemB) == 0);
    Stop();
}

// FUN_1027cb00: with both compared item objects, the deltas from their records (scaled when the item has a level).
COA_TEST(TooltipScaling_CompareDeltas) {
    Start();
    int32_t summary[0x4A] = {};
    CoA::TooltipScaling::CompareDeltas(summary);
    CHECK(summary[1] == 0);                       // no items: untouched
    Rec(9002, 20).Stat(0, 7, 30).Stat(1, 0x26, 50).At(0x78, 200).At(0x7C, 250).At(0x84, 9).Insert();
    CoA::ObjectAddon::Item(kItemA)[0].value = 20;
    CoA::TooltipScaling::CacheItem b;
    b.statType[0] = 7;
    b.statValue[0] = 10;
    b.resistances[0] = 120;
    b.resistances[2] = 4;
    s_cache[9004] = b;
    s_items[kItemA] = 9002;
    s_items[kItemB] = 9004;
    int32_t skipLock = 0;
    CoA::TooltipScaling::BeforeSetItem(9002, nullptr, &kItemA, skipLock);   // the shown item: compare A
    CoA::TooltipScaling::AfterSetItem();
    CHECK(CoA::TooltipScaling::BeginHyperlinkCompare(false));
    CoA::TooltipScaling::BeforeSetItem(9004, nullptr, &kItemB, skipLock);   // the compared one: B
    CoA::TooltipScaling::CompareDeltas(summary);
    CoA::TooltipScaling::AfterSetItem();
    CoA::TooltipScaling::EndHyperlinkCompare();
    CHECK(summary[0x13] == 20);                   // stamina 30 - 10
    CHECK(summary[0x08] == 50);                   // attack power
    CHECK(summary[1] == 250 - 120);               // stock class: reborn armor (B's from the cache's armor)
    CHECK(summary[3] == 9 - 4);                   // fire
    Stop();
}

COA_TEST(TooltipScaling_ContextHooksAndOnTextChanged) {
    Start();
    int a = 0;
    int b = 0;
    int ref = 0;
    CHECK(!CoA::TooltipScaling::OnTextChanged(&a, ref));
    CoA::TooltipScaling::SetOnTextChanged(&a, 5);
    CHECK(CoA::TooltipScaling::OnTextChanged(&a, ref) && ref == 5);
    CoA::TooltipScaling::SetOnTextChanged(&a, 7);
    CHECK(CoA::TooltipScaling::OnTextChanged(&a, ref) && ref == 7);
    CHECK(!CoA::TooltipScaling::OnTextChanged(&b, ref));

    CoA::TooltipScaling::SetInboxItem(0x1234);
    CHECK(CoA::TooltipScaling::TooltipGuid() == 0x4000000000001234ull);
    CoA::TooltipScaling::SetInboxItem(0);
    CHECK(CoA::TooltipScaling::TooltipGuid() == 0);

    // The auction context through ItemStats' auction Deps: index == count + 1 still reads.
    static std::vector<uint32_t> s_seen;
    CoA::ItemStats::Deps& d = CoA::ItemStats::Dependencies();
    d.auctionList = [](int list, uint32_t& count) { count = list == 2 ? 2 : 0; return list == 2; };
    d.auctionEntry = [](int, uint32_t index, uint32_t& item, uint32_t& level) {
        s_seen.push_back(index);
        item = 9002;
        level = 40 + index;
    };
    Scales();
    Rec(9002, 42).Insert();
    lua_State* L = luaL_newstate();
    lua_pushnil(L);
    lua_pushstring(L, "list");
    lua_pushnumber(L, 3);
    CoA::TooltipScaling::BeforeAuctionItem(L);
    CHECK((s_seen == std::vector<uint32_t>{ 2 }));
    CHECK(LevelOf(Scaled(9002)) == 42);
    lua_settop(L, 0);
    lua_pushnil(L);
    lua_pushstring(L, "list");
    lua_pushnumber(L, 4);                          // 3 >= count + 1: nothing
    CoA::TooltipScaling::BeforeAuctionItem(L);
    lua_settop(L, 0);
    lua_pushnil(L);
    lua_pushstring(L, "owner");                    // an empty list
    lua_pushnumber(L, 1);
    CoA::TooltipScaling::BeforeAuctionItem(L);
    CHECK(s_seen.size() == 1);
    lua_close(L);

    // SMSG 0x716 is registered; a short packet reads zeros.
    CoATest::Bytes shortPacket;
    shortPacket.U32(9).U32(8);
    CHECK(CoATest::Deliver(CoA::TooltipScaling::OnInspectLevels, 0x716, shortPacket) == 1);
    CHECK(CoA::TooltipScaling::InspectLevel(0) == 9 && CoA::TooltipScaling::InspectLevel(1) == 8 &&
        CoA::TooltipScaling::InspectLevel(2) == 0 && CoA::TooltipScaling::InspectLevel(19) == 0);
    int32_t result = 0;
    CDataStore msg(shortPacket.data.data(), static_cast<uint32_t>(shortPacket.data.size()));
    CHECK(CoA::Packets::Dispatch(0x716, 0, &msg, result));
    Stop();
}
