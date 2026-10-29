// AscBuildCreator.cpp's Lua half: the entry serialiser (FUN_1009c1f0), the table arguments, and the C_BuildCreator /
// C_BuildEditor natives (binding tables 0x1010DF40 / 0x1010D980).
#include "CoA/Source/Modules/BuildCreatorInternal.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Packet.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <algorithm>
#include <unordered_set>

using namespace CoA::BuildCreator::detail;
using CoA::CA::Row;
using CoA::Script::CheckNumber;
using CoA::Script::CheckString;
using CoA::Script::PushBool;
using CoA::Script::PushInt;
using CoA::Script::PushStr;
using CoA::Script::ReadInt2;
using CoA::Script::ReadNumber;
using CoA::Script::ReadString;
using CoA::Script::ToInt;
using CoA::Script::ValidateInput;

namespace {
#include "CoA/Source/Modules/CAEnums.generated.inc"

    // ---- Lua serialisers -------------------------------------------------------------------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v) {
        lua_pushstring(L, k);
        lua_pushstring(L, v.c_str());
        lua_settable(L, -3);
    }

    void SetInt(lua_State* L, const char* k, int32_t v) {
        lua_pushstring(L, k);
        lua_pushinteger(L, v);
        lua_settable(L, -3);
    }

    void SetBool(lua_State* L, const char* k, bool v) {
        lua_pushstring(L, k);
        lua_pushboolean(L, v ? 1 : 0);
        lua_settable(L, -3);
    }

    void PushSpell(lua_State* L, const Spell& s) {   // FUN_1009d6f0
        lua_createtable(L, 0, 0);
        lua_checkstack(L, 2);
        SetInt(L, "Spell", static_cast<int32_t>(s.spell));
        SetInt(L, "Level", static_cast<int32_t>(s.level));
        SetBool(L, "IsCoreAbility", s.core != 0);
        SetBool(L, "IsOptimalAbility", s.optimal != 0);
        SetBool(L, "IsEmpoweringAbility", s.empowering != 0);
        SetBool(L, "IsSynergisticAbility", s.synergistic != 0);
        SetInt(L, "Flags", static_cast<int32_t>(s.flags));
        SetStr(L, "Comment", s.comment);
    }

    template <class T, class Fn>
    void PushList(lua_State* L, const std::vector<T>& v, Fn element) {
        lua_createtable(L, 0, static_cast<int>(v.size()));
        lua_checkstack(L, 2);

        for (size_t i = 0; i < v.size(); ++i) {
            lua_pushnumber(L, static_cast<double>(i + 1));
            element(v[i]);
            lua_settable(L, -3);
        }
    }

    // FUN_1009c1f0 (the entry passed by value).
    void PushEntry(lua_State* L, const Entry& e) {
        lua_createtable(L, 0, 0);
        lua_checkstack(L, 2);
        SetStr(L, "ID", e.id);
        SetStr(L, "LevelingID", e.levelingId);
        SetStr(L, "EndGameIDPVE", e.pveId);
        SetStr(L, "EndGameIDPVP", e.pvpId);

        if (e.author.empty()) {   // the player's name (CGObject vtable +0xD8, nullptr as ""), "Unknown" without one
            SetStr(L, "AuthorName", HasPlayer() ? PlayerName() : "未知");   // 中文化（By leewheel 2026-10-29）
        } else {
            SetStr(L, "AuthorName", e.author);
        }

        SetStr(L, "Name", e.name);
        SetStr(L, "Subtext", e.subtext);
        SetStr(L, "Description", e.description);
        SetStr(L, "Icon", e.icon);
        SetInt(L, "CreatedTime", static_cast<int32_t>(e.created));
        SetInt(L, "UpdatedTime", static_cast<int32_t>(e.updated));
        SetStr(L, "Category", EnumName(kBcCategories, e.category));
        std::string role = Unexpected(e.roles);

        for (int i = 0; i < 6; ++i) {
            if (kRoleValues[i] == e.roles) {
                role = kPlayerRoles[i];
                break;
            }
        }

        SetStr(L, "Roles", role);
        SetStr(L, "PrimaryStat", EnumName(kStats, e.primaryStat));   // FUN_10099f80

        lua_pushstring(L, "Spells");
        PushList(L, e.spells, [&](const Spell& s) { PushSpell(L, s); });
        lua_settable(L, -3);

        lua_pushstring(L, "RandomEnchants");   // FUN_1009a680
        PushList(L, e.enchants, [&](const Enchant& c) {
            lua_createtable(L, 0, 0);
            lua_checkstack(L, 2);
            SetInt(L, "Enchant", static_cast<int32_t>(c.id));
            SetInt(L, "Stacks", static_cast<int32_t>(c.stacks));
            SetInt(L, "Level", static_cast<int32_t>(c.level));
            SetInt(L, "Flags", static_cast<int32_t>(c.flags));
            SetStr(L, "Comment", c.comment);
        });
        lua_settable(L, -3);

        lua_pushstring(L, "WeaponTypes");   // FUN_1009a8f0 / FUN_1009d890
        PushList(L, e.weapons, [&](const Typed& t) {
            lua_createtable(L, 0, 0);
            lua_checkstack(L, 2);
            SetStr(L, "Type", EnumName(kWeaponTypes, t.type));
            SetStr(L, "Comment", t.comment);
        });
        lua_settable(L, -3);

        lua_pushstring(L, "ArmorTypes");   // FUN_1009a500 / FUN_1009c030
        PushList(L, e.armor, [&](const Typed& t) {
            lua_createtable(L, 0, 0);
            lua_checkstack(L, 2);
            SetStr(L, "Type", EnumName(kArmorTypes, t.type));
            SetStr(L, "Comment", t.comment);
        });
        lua_settable(L, -3);

        SetBool(L, "NeedsRepairs", e.hidden != 0);
        SetInt(L, "Flags", static_cast<int32_t>(e.flags));
        SetInt(L, "Upvotes", static_cast<int32_t>(e.upvotes));
        SetStr(L, "DifficultyRating", e.difficulty <= 4 ? std::string(kDifficulty[e.difficulty]) : Unexpected(e.difficulty));

        // The first random enchant whose MysticEnchant record is legendary (quality 5): its spell.
        lua_pushstring(L, "LegendaryEnchant");
        bool found = false;

        for (const Enchant& c : e.enchants) {
            if (const uint8_t* rec = EnchantRow(c.id)) {
                if (EnchantQuality(rec) == 5) {
                    lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(CoA::Dbc::Table::U32(rec, 4))));
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            lua_pushnil(L);
        }

        lua_settable(L, -3);

        // Essence and costs come from a build of the PENDING entry (FUN_100fde60 is container +0x10), whichever entry
        // is being serialised.
        CoA::CA::Build b(SeedOf(M().pending), MaxLevel(), false);
        SetInt(L, "AbilityEssence", static_cast<int32_t>(b.GlobalAE(0)));
        SetInt(L, "TalentEssence", static_cast<int32_t>(b.GlobalTE(0)));
        SetInt(L, "CommonCost", static_cast<int32_t>(CoA::CA::QualitySumOf(b, 1)));
        SetInt(L, "UncommonCost", static_cast<int32_t>(CoA::CA::QualitySumOf(b, 2)));
        SetInt(L, "RareCost", static_cast<int32_t>(CoA::CA::QualitySumOf(b, 3)));
        SetInt(L, "EpicCost", static_cast<int32_t>(CoA::CA::QualitySumOf(b, 4)));
        SetInt(L, "LegendaryCost", static_cast<int32_t>(CoA::CA::QualitySumOf(b, 5)));

        // Rows by the first flag each spell carries (core > optimal > empowering > synergistic). The original indexes
        // CharacterAdvancement.dbc by the spell value itself here (DAT_10bdf65c).
        std::unordered_set<Row> sets[4];

        for (const Spell& s : e.spells) {
            if (Row r = CoA::CA::FindRow(s.spell)) {
                if (s.core) {
                    sets[0].insert(r);
                } else if (s.optimal) {
                    sets[1].insert(r);
                } else if (s.empowering) {
                    sets[2].insert(r);
                } else if (s.synergistic) {
                    sets[3].insert(r);
                }
            }
        }

        const CoA::CA::Build* active = CoA::CA::ActiveBuild();
        int32_t abilities[4] = {};
        int32_t knownAbilities[4] = {};
        int32_t talents[4] = {};
        int32_t knownTalents[4] = {};

        for (int i = 0; i < 4; ++i) {
            for (Row r : sets[i]) {
                const int t = CoA::CA::RowType(r);
                const bool known = active->Has(CoA::CA::RowU32(r, 0));

                if (t == 1 || t == 4) {   // FUN_101c6770
                    ++abilities[i];
                    knownAbilities[i] += known;
                }

                if (t == 2) {   // FUN_101c6e80
                    ++talents[i];
                    knownTalents[i] += known;
                }
            }
        }

        const char* kinds[4] = { "Core", "Optimal", "Empowering", "Synergistic" };

        for (int i = 0; i < 4; ++i) {
            const std::string k = kinds[i];
            SetInt(L, ("Num" + k + "Spells").c_str(), abilities[i] + talents[i]);
            SetInt(L, ("NumKnown" + k + "Spells").c_str(), knownAbilities[i] + knownTalents[i]);
            SetInt(L, ("Num" + k + "Abilities").c_str(), abilities[i]);
            SetInt(L, ("NumKnown" + k + "Abilities").c_str(), knownAbilities[i]);
            SetInt(L, ("Num" + k + "Talents").c_str(), talents[i]);
            SetInt(L, ("NumKnown" + k + "Talents").c_str(), knownTalents[i]);
        }
    }

    // FUN_100f1660 & siblings: { [1] = NAME, ... }.
    template <size_t N>
    void PushErrorList(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors) {
        lua_createtable(L, 0, static_cast<int>(errors.size()));
        lua_checkstack(L, 2);

        for (size_t i = 0; i < errors.size(); ++i) {
            lua_pushnumber(L, static_cast<double>(i + 1));
            PushStr(L, EnumName(table, errors[i]).c_str());
            lua_settable(L, -3);
        }
    }

    // The Can* shape: (true, nil) or (false, {names}).
    template <size_t N>
    int PushCan(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors) {
        PushBool(L, errors.empty());

        if (errors.empty()) {
            lua_pushnil(L);
        } else {
            PushErrorList(L, table, errors);
        }

        return 2;
    }

    // The mutator shape (FUN_100f2c20 & siblings): (false, NAME) on the first error, else (true, nil).
    template <size_t N>
    int PushDone(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors) {
        if (!errors.empty()) {
            PushBool(L, false);
            PushStr(L, EnumName(table, errors[0]).c_str());
            return 2;
        }

        PushBool(L, true);
        lua_pushnil(L);
        return 2;
    }

    int PushOk(lua_State* L) {
        PushBool(L, true);
        lua_pushnil(L);
        return 2;
    }

    // ---- Lua table arguments (FrameScript::lua_getmapvalue, FUN_100f05c0 / FUN_100f1310) ---------------------
    void MissingMember(const char* type, const char* key) {
        CoA::Log::Printf("lua_tovalue<%s>: Missing member '%s' in table at index %d", type, key, 1);
    }

    void FieldU32(lua_State* L, const char* type, const char* key, uint32_t& out) {
        lua_getfield(L, 1, key);

        if (lua_type(L, -1) == LUA_TNUMBER) {
            out = static_cast<uint32_t>(ToInt(lua_tonumber(L, -1)));
        } else {
            MissingMember(type, key);
        }

        lua_settop(L, -2);
    }

    void FieldBool(lua_State* L, const char* type, const char* key, uint8_t& out) {
        lua_getfield(L, 1, key);

        if (lua_type(L, -1) == LUA_TBOOLEAN) {
            out = lua_toboolean(L, -1) ? 1 : 0;
        } else {
            MissingMember(type, key);
        }

        lua_settop(L, -2);
    }

    bool FieldStr(lua_State* L, const char* type, const char* key, std::string& out) {
        lua_getfield(L, 1, key);
        const bool found = lua_type(L, -1) == LUA_TSTRING;

        if (found) {
            out = lua_tolstring(L, -1, nullptr);
        } else {
            MissingMember(type, key);
        }

        lua_settop(L, -2);
        return found;
    }

    bool ReadSpellArg(lua_State* L, Spell& s) {   // FUN_100e9540 -> FUN_100f4b20 / FUN_100f6310
        if (!ValidateInput(L, { LUA_TTABLE })) {
            return false;
        }

        const char* t = "BuildCreatorSpell";
        FieldU32(L, t, "Spell", s.spell);
        FieldU32(L, t, "Level", s.level);
        FieldBool(L, t, "IsCoreAbility", s.core);
        FieldBool(L, t, "IsOptimalAbility", s.optimal);
        FieldBool(L, t, "IsEmpoweringAbility", s.empowering);
        FieldBool(L, t, "IsSynergisticAbility", s.synergistic);
        FieldU32(L, t, "Flags", s.flags);
        FieldStr(L, t, "Comment", s.comment);
        return true;
    }

    bool ReadEnchantArg(lua_State* L, Enchant& c) {   // FUN_100e9460 -> FUN_100f4800 / FUN_100f5c50
        if (!ValidateInput(L, { LUA_TTABLE })) {
            return false;
        }

        const char* t = "BuildCreatorRandomEnchant";
        FieldU32(L, t, "Enchant", c.id);
        FieldU32(L, t, "Stacks", c.stacks);
        FieldU32(L, t, "Level", c.level);
        FieldU32(L, t, "Flags", c.flags);
        FieldStr(L, t, "Comment", c.comment);
        return true;
    }

    // FUN_100e9600 / FUN_100e9390 -> FUN_100f4e50 / FUN_100f4420: {Type = "ITEM_SUBCLASS_*", Comment}.
    template <size_t N>
    bool ReadTypedArg(lua_State* L, const char* const (&table)[N], Typed& out) {
        if (!ValidateInput(L, { LUA_TTABLE })) {
            return false;
        }

        const char* t = "BuildCreatorItemType";
        std::string name;

        if (FieldStr(L, t, "Type", name)) {
            out.type = EnumOrLog(table, name, out.type);
        }

        FieldStr(L, t, "Comment", out.comment);
        return true;
    }

    const char* const kBuildFilters[48] = {"FILTER_NONE", "FILTER_FEATURED", "FILTER_DIFFICULTY_RATING_EASY",
        "FILTER_DIFFICULTY_RATING_NORMAL", "FILTER_DIFFICULTY_RATING_HARD", "FILTER_DIFFICULTY_RATING_VERY_HARD",
        "FILTER_DIFFICULTY_RATING_IMPOSSIBLE", "FILTER_OWNED_BUILDS", "FILTER_PRIMARY_STAT_STRENGTH",
        "FILTER_PRIMARY_STAT_AGILITY", "FILTER_PRIMARY_STAT_INTELLECT", "FILTER_PRIMARY_STAT_SPIRIT",
        "FILTER_ROLE_TANK", "FILTER_ROLE_HEALER", "FILTER_ROLE_DAMAGE", "FILTER_LAST_30_DAYS",
        "FILTER_CLASS_WARRIOR", "FILTER_CLASS_PALADIN", "FILTER_CLASS_HUNTER", "FILTER_CLASS_ROGUE",
        "FILTER_CLASS_PRIEST", "FILTER_CLASS_DEATH_KNIGHT", "FILTER_CLASS_SHAMAN", "FILTER_CLASS_MAGE",
        "FILTER_CLASS_WARLOCK", "FILTER_CLASS_HERO", "FILTER_CLASS_DRUID", "FILTER_CLASS_BARBARIAN",
        "FILTER_CLASS_WITCH_DOCTOR", "FILTER_CLASS_DEMON_HUNTER", "FILTER_CLASS_WITCH_HUNTER",
        "FILTER_CLASS_STORMBRINGER", "FILTER_CLASS_KNIGHT_OF_XOROTH", "FILTER_CLASS_GUARDIAN", "FILTER_CLASS_MONK",
        "FILTER_CLASS_SON_OF_ARUGAL", "FILTER_CLASS_RANGER", "FILTER_CLASS_CHRONOMANCER",
        "FILTER_CLASS_NECROMANCER", "FILTER_CLASS_PYROMANCER", "FILTER_CLASS_CULTIST", "FILTER_CLASS_STARCALLER",
        "FILTER_CLASS_SUN_CLERIC", "FILTER_CLASS_TINKER", "FILTER_CLASS_VENOMANCER", "FILTER_CLASS_REAPER",
        "FILTER_CLASS_PRIMALIST", "FILTER_CLASS_RUNEMASTER"};  // 0x10B1F780
    const char* const kBuildSorts[5] = {"SORT_NONE", "SORT_RATING_ASCENDING", "SORT_RATING_DESCENDING",
        "SORT_DATE_ASCENDING", "SORT_DATE_DESCENDING"};  // 0x10B1E854

    // FUN_100f36c0 / FUN_100f3d70 (FrameScript::lua_totable into an enum -> bool map): string keys through
    // FUN_100f5470 / FUN_100f56a0 (an unknown one logs and becomes 0), nil / boolean / number values, the last write
    // winning. A bad key or value logs and yields an empty map. The true keys in the map's (the original's
    // unordered_map) order.
    template <size_t N>
    std::vector<uint32_t> TrueKeys(lua_State* L, int idx, const char* const (&names)[N]) {
        U32Map<bool> map;
        lua_pushnil(L);

        while (lua_next(L, idx)) {
            if (!lua_isstring(L, -2)) {
                CoA::Log::Printf("FrameScript::lua_totable Invalid lua key type");
                lua_settop(L, -3);
                return {};
            }

            const int vt = lua_type(L, -1);

            if (vt != LUA_TNIL && vt != LUA_TBOOLEAN && vt != LUA_TNUMBER) {
                CoA::Log::Printf("FrameScript::lua_totable Invalid lua value type");
                lua_settop(L, -3);
                return {};
            }

            const bool value = lua_toboolean(L, -1) != 0;
            const std::string key = lua_tolstring(L, -2, nullptr);
            uint32_t id = 0;

            if (!EnumIndex(names, key, id)) {
                CoA::Log::Printf("lua_tovalue<enum>: Unknown string '%s'", key.c_str());
            }

            map[id] = value;
            lua_settop(L, -2);
        }

        std::vector<uint32_t> keys;

        for (const auto& kv : map) {
            if (kv.second) {
                keys.push_back(kv.first);
            }
        }

        return keys;
    }

    // ---- getters ---------------------------------------------------------------------------------------------
    int GetNumBuilds(lua_State* L) {   // +0x164 filtered
        PushInt(L, static_cast<int32_t>(Builds().Size()));
        return 1;
    }

    int GetBuildAtIndex(lua_State* L) {
        uint32_t i = 0;

        if (!ReadNumber(L, i) || i == 0) {
            return 0;
        }

        if (const Entry* e = FilteredAt(i - 1)) {
            PushEntry(L, Entry(*e));   // FUN_100f2bb0
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    int GetBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        if (const Entry* e = FindBuild(id)) {
            PushEntry(L, Entry(*e));
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    int GetPendingBuild(lua_State* L) {
        PushEntry(L, Entry(M().pending));
        return 1;
    }

    int GetActiveBuild(lua_State* L) {   // the id recorded for spec (1-based), "" past the end
        uint32_t i = 0;

        if (!ReadNumber(L, i) || i == 0) {
            return 0;
        }

        PushStr(L, i - 1 < M().specs.size() ? M().specs[i - 1].id.c_str() : "");
        return 1;
    }

    int GetActiveSpecForBuild(lua_State* L) {   // the spec index holding this id, -1 if none
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        int32_t index = -1;

        for (size_t i = 0; i < M().specs.size(); ++i) {
            if (M().specs[i].id == id) {
                index = static_cast<int32_t>(i);
                break;
            }
        }

        PushInt(L, index);
        return 1;
    }

    int GetNumBookmarkedBuilds(lua_State* L) {
        PushInt(L, static_cast<int32_t>(M().queried.size()));
        return 1;
    }

    int GetBookmarkedBuildAtIndex(lua_State* L) {   // the id, "" past the end
        uint32_t i = 0;

        if (!ReadNumber(L, i) || i == 0) {
            return 0;
        }

        PushStr(L, i - 1 < M().queried.size() ? M().queried[i - 1].id.c_str() : "");
        return 1;
    }

    int IsOwnedBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        PushBool(L, Contains(M().owned, id));
        return 1;
    }

    int IsUpvotedBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        PushBool(L, Contains(M().upvoted, id));
        return 1;
    }

    int DoesBuildHaveSpellID(lua_State* L) {
        uint32_t spell = 0;

        if (!ReadNumber(L, spell)) {
            return 0;
        }

        PushBool(L, PendingSpell(spell) != nullptr);
        return 1;
    }

    int DoesBuildHaveEnchant(lua_State* L) {
        uint32_t id = 0;

        if (!ReadNumber(L, id)) {
            return 0;
        }

        bool found = false;

        for (const Enchant& c : M().pending.enchants) {
            found = found || c.id == id;
        }

        PushBool(L, found);
        return 1;
    }

    int GetSpell(lua_State* L) {   // (buildId, spell)
        if (!ValidateInput(L, { LUA_TSTRING, LUA_TNUMBER })) {
            return 0;
        }

        const std::string id = CheckString(L, 1);
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));   // FUN_10ae6370

        if (const Entry* e = FindBuild(id)) {
            for (const Spell& s : e->spells) {
                if (s.spell == spell) {
                    PushSpell(L, s);   // FUN_1009af40
                    return 1;
                }
            }
        }

        lua_pushnil(L);
        return 1;
    }

    int GetSpellByID(lua_State* L) {
        uint32_t spell = 0;

        if (!ReadNumber(L, spell)) {
            return 0;
        }

        if (const Spell* s = PendingSpell(spell)) {
            PushSpell(L, *s);
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    // The level argument, or the server's max level (FUN_1008ba00 fails -> FUN_102fc4d0).
    uint32_t LevelArg(lua_State* L) {
        uint32_t lvl = 0;

        if (!ReadNumber(L, lvl)) {
            lvl = MaxLevel();
        }

        return lvl;
    }

    int GetEssenceForLevel(lua_State* L) {
        CoA::CA::Build b(SeedOf(M().pending), LevelArg(L), false);
        PushInt(L, static_cast<int32_t>(b.GlobalAE(0)));
        PushInt(L, static_cast<int32_t>(b.RemainingAE()));
        PushInt(L, static_cast<int32_t>(b.GlobalTE(0)));
        PushInt(L, static_cast<int32_t>(b.RemainingTE()));
        return 4;
    }

    int GetQualityInfo(lua_State* L) {   // (spell[, level]) -> quality name, cost
        int32_t spell = 0;
        int32_t lvl = 0;

        if (!ReadInt2(L, spell, lvl)) {
            uint32_t s = 0;

            if (!ReadNumber(L, s)) {
                return 0;
            }

            spell = static_cast<int32_t>(s);
            lvl = static_cast<int32_t>(MaxLevel());
        }

        const Row row = CoA::CA::RowBySpell(static_cast<uint32_t>(spell));

        if (!row) {
            lua_pushnil(L);
            return 1;
        }

        CoA::CA::Build b(SeedOf(M().pending), static_cast<uint32_t>(lvl), true);
        const uint32_t q = static_cast<uint32_t>(CoA::CA::QualityIndexOf(b, row));
        PushStr(L, (q < 9 ? std::string(kQualities[q]) : Unexpected(q)).c_str());
        PushInt(L, static_cast<int32_t>(CoA::CA::QualityCostOf(b, row)));
        return 2;
    }

    int GetQualityInfoForLevel(lua_State* L) {
        CoA::CA::Build b(SeedOf(M().pending), LevelArg(L), false);

        for (int q = 1; q <= 5; ++q) {
            PushInt(L, static_cast<int32_t>(CoA::CA::QualitySumOf(b, q)));
        }

        return 5;
    }

    // ---- editing the pending build -----------------------------------------------------------------------------
    int EditBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        if (const Entry* e = FindBuild(id)) {
            M().pending = Entry(*e);
        } else {
            ResetPending();
        }

        return 0;
    }

    int DiscardPendingBuild(lua_State*) {
        ResetPending();
        return 0;
    }

    int SetName(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.name = s;
        }

        return 0;
    }

    int SetIcon(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.icon = s;
        }

        return 0;
    }

    int SetDescription(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.description = s;
        }

        return 0;
    }

    int SetCategory(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.category = EnumOrLog(kBcCategories, s, kNoValue);
        }

        return 0;
    }

    int SetRoles(lua_State* L) {
        std::string s;

        if (!ReadString(L, s)) {
            return 0;
        }

        uint32_t index = 0;
        uint32_t value = 0;

        if (EnumIndex(kPlayerRoles, s, index)) {
            value = kRoleValues[index];
        } else {
            CoA::Log::Printf("Unexpected Value: %s", s.c_str());
        }

        M().pending.roles = value;
        return 0;
    }

    int SetPrimaryStat(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.primaryStat = EnumOrLog(kStats, s, 0);
        }

        return 0;
    }

    int SetDifficultyRating(lua_State* L) {
        std::string s;

        if (ReadString(L, s)) {
            M().pending.difficulty = EnumOrLog(kDifficulty, s, 0);
        }

        return 0;
    }

    // (spellOrEnchant, comment): an enchant with that id, else every spell of that chain; or (typeName, comment): the
    // weapon type, else the armour type of that name.
    int SetComment(lua_State* L) {
        if (ValidateInput(L, { LUA_TNUMBER, LUA_TSTRING })) {
            const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            const std::string comment = CheckString(L, 2);
            bool enchant = false;

            for (Enchant& c : M().pending.enchants) {
                if (c.id == id) {
                    c.comment = comment;
                    enchant = true;
                    break;
                }
            }

            if (!enchant) {
                const uint32_t first = FirstRank(id);

                for (Spell& s : M().pending.spells) {
                    if (FirstRank(s.spell) == first) {
                        PendingSpell(s.spell)->comment = comment;
                    }
                }
            }

            return 0;
        }

        if (!ValidateInput(L, { LUA_TSTRING, LUA_TSTRING })) {
            return 0;
        }

        const std::string name = CheckString(L, 1);
        const std::string comment = CheckString(L, 2);
        uint32_t type = 0;

        if (EnumIndex(kWeaponTypes, name, type)) {   // FUN_100eff20
            for (Typed& t : M().pending.weapons) {
                if (t.type == type) {
                    t.comment = comment;
                    break;
                }
            }
        } else if (EnumIndex(kArmorTypes, name, type)) {   // FUN_100efd20
            for (Typed& t : M().pending.armor) {
                if (t.type == type) {
                    t.comment = comment;
                    break;
                }
            }
        }

        return 0;
    }

    // Spells.
    int AddSpell(lua_State* L) {
        Spell s;

        if (!ReadSpellArg(L, s)) {
            return 0;
        }

        const std::vector<uint32_t> errors = CanAddSpell(s);

        if (errors.empty()) {
            if (Spell* old = PendingSpell(s.spell)) {   // FUN_100fa550
                *old = s;
            } else {
                M().pending.spells.push_back(s);
            }

            SortEntry(M().pending);
        }

        return PushDone(L, kAddSpellResults, errors);
    }

    int CanAddSpellL(lua_State* L) {
        Spell s;

        if (!ReadSpellArg(L, s)) {
            return 0;
        }

        return PushCan(L, kAddSpellResults, CanAddSpell(s));
    }

    int RemoveSpell(lua_State* L) {
        uint32_t spell = 0;

        if (!ReadNumber(L, spell)) {
            return 0;
        }

        const std::vector<uint32_t> errors = CanRemoveSpell(spell);

        if (errors.empty()) {
            std::vector<Spell>& v = M().pending.spells;
            auto it = std::find_if(v.begin(), v.end(), [&](const Spell& s) { return s.spell == spell; });

            if (it != v.end()) {
                v.erase(it);
                SortEntry(M().pending);
            }
        }

        return PushDone(L, kRemoveSpellResults, errors);
    }

    int CanRemoveSpellL(lua_State* L) {   // (true, nil) or (false, NAME) -- a single name here
        uint32_t spell = 0;

        if (!ReadNumber(L, spell)) {
            return 0;
        }

        return PushDone(L, kRemoveSpellResults, CanRemoveSpell(spell));
    }

    int SetSpellLevel(lua_State* L) {
        int32_t spell = 0;
        int32_t level = 0;

        if (!ReadInt2(L, spell, level)) {
            return 0;
        }

        const std::vector<uint32_t> errors = CanSetSpellLevel(static_cast<uint32_t>(spell), static_cast<uint32_t>(level));

        if (errors.empty()) {
            if (Spell* s = PendingSpell(static_cast<uint32_t>(spell))) {
                s->level = static_cast<uint32_t>(level);
            }
        }

        return PushDone(L, kSetSpellLevelResults, errors);
    }

    int CanSetSpellLevelL(lua_State* L) {
        int32_t spell = 0;
        int32_t level = 0;

        if (!ReadInt2(L, spell, level)) {
            return 0;
        }

        return PushCan(L, kSetSpellLevelResults, CanSetSpellLevel(static_cast<uint32_t>(spell), static_cast<uint32_t>(level)));
    }

    int SetSpellFlags(lua_State* L) {   // every spell of the chain
        int32_t spell = 0;
        int32_t flags = 0;

        if (!ReadInt2(L, spell, flags)) {
            return 0;
        }

        const uint32_t first = FirstRank(static_cast<uint32_t>(spell));

        for (Spell& s : M().pending.spells) {
            if (FirstRank(s.spell) == first) {
                PendingSpell(s.spell)->flags = static_cast<uint32_t>(flags);
            }
        }

        return 0;
    }

    // FUN_10102b80: CanSetSpellFlags, CanSetEnchantLevel, CanSetEnchantFlags, CanSetRandomEnchantStacks.
    int CanSetInt2(lua_State* L) {
        int32_t a = 0;
        int32_t b = 0;

        if (!ReadInt2(L, a, b)) {
            return 0;
        }

        return PushOk(L);
    }

    bool ReadSpellBool(lua_State* L, uint32_t& spell, bool& on) {   // FUN_100e92d0
        if (!ValidateInput(L, { LUA_TNUMBER, LUA_TBOOLEAN })) {
            return false;
        }

        spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        on = lua_toboolean(L, 2) != 0;
        return true;
    }

    // SetIs*Ability: the flag on every spell of the chain; setting it clears the other three.
    int SetAbilityKind(lua_State* L, uint8_t Spell::*flag) {
        uint32_t spell = 0;
        bool on = false;

        if (!ReadSpellBool(L, spell, on)) {
            return 0;
        }

        uint8_t Spell::*const all[4] = { &Spell::core, &Spell::optimal, &Spell::empowering, &Spell::synergistic };
        const uint32_t first = FirstRank(spell);

        for (Spell& s : M().pending.spells) {
            if (FirstRank(s.spell) == first) {
                Spell* t = PendingSpell(s.spell);
                t->*flag = on ? 1 : 0;

                if (on) {
                    for (uint8_t Spell::*other : all) {
                        if (other != flag) {
                            t->*other = 0;
                        }
                    }
                }
            }
        }

        return 0;
    }

    template <size_t N>
    int CanSetAbilityKind(lua_State* L, uint8_t Spell::*flag, const char* const (&table)[N]) {
        uint32_t spell = 0;
        bool on = false;

        if (!ReadSpellBool(L, spell, on)) {
            return 0;
        }

        std::vector<uint32_t> errors;

        if (const Spell* s = PendingSpell(spell)) {
            if ((s->*flag != 0) == on) {
                errors.push_back(1);
            }
        } else {
            errors.push_back(2);
        }

        return PushCan(L, table, errors);
    }

    int SetIsCoreAbility(lua_State* L) {
        return SetAbilityKind(L, &Spell::core);
    }

    int SetIsOptimalAbility(lua_State* L) {
        return SetAbilityKind(L, &Spell::optimal);
    }

    int SetIsEmpoweringAbility(lua_State* L) {
        return SetAbilityKind(L, &Spell::empowering);
    }

    int SetIsSynergisticAbility(lua_State* L) {
        return SetAbilityKind(L, &Spell::synergistic);
    }

    int CanSetIsCoreAbility(lua_State* L) {
        return CanSetAbilityKind(L, &Spell::core, kSetCoreResults);
    }

    int CanSetIsOptimalAbility(lua_State* L) {
        return CanSetAbilityKind(L, &Spell::optimal, kSetOptimalResults);
    }

    int CanSetIsEmpoweringAbility(lua_State* L) {
        return CanSetAbilityKind(L, &Spell::empowering, kSetEmpoweringResults);
    }

    int CanSetIsSynergisticAbility(lua_State* L) {
        return CanSetAbilityKind(L, &Spell::synergistic, kSetSynergisticResults);
    }

    // (id, comment) with the comment at most 0x80 (FUN_100e9180).
    template <size_t N>
    int CanSetNumberComment(lua_State* L, const char* const (&table)[N]) {
        if (!ValidateInput(L, { LUA_TNUMBER, LUA_TSTRING })) {
            return 0;
        }

        const std::string comment = CheckString(L, 2);
        std::vector<uint32_t> errors;

        if (comment.size() > 0x80) {
            errors.push_back(1);
        }

        return PushCan(L, table, errors);
    }

    int CanSetSpellComment(lua_State* L) {
        return CanSetNumberComment(L, kSetSpellCommentResults);
    }

    int CanSetRandomEnchantComment(lua_State* L) {
        return CanSetNumberComment(L, kSetEnchantCommentResults);
    }

    // Random enchants.
    int AddRandomEnchant(lua_State* L) {
        Enchant c;

        if (!ReadEnchantArg(L, c)) {
            return 0;
        }

        std::vector<Enchant>& v = M().pending.enchants;   // FUN_100fa430
        auto it = std::find_if(v.begin(), v.end(), [&](const Enchant& e) { return e.id == c.id; });

        if (it == v.end()) {
            v.push_back(c);
        } else {
            *it = c;
        }

        SortEntry(M().pending);
        return PushOk(L);
    }

    int CanAddRandomEnchant(lua_State* L) {
        Enchant c;

        if (!ReadEnchantArg(L, c)) {
            return 0;
        }

        return PushOk(L);
    }

    int RemoveRandomEnchant(lua_State* L) {
        uint32_t id = 0;

        if (!ReadNumber(L, id)) {
            return 0;
        }

        std::vector<Enchant>& v = M().pending.enchants;
        auto it = std::find_if(v.begin(), v.end(), [&](const Enchant& e) { return e.id == id; });

        if (it != v.end()) {
            v.erase(it);
            SortEntry(M().pending);
        }

        return PushOk(L);
    }

    int CanRemoveRandomEnchant(lua_State* L) {
        uint32_t id = 0;

        if (!ReadNumber(L, id)) {
            return 0;
        }

        return PushOk(L);
    }

    int SetEnchantField(lua_State* L, uint32_t Enchant::*field) {
        int32_t id = 0;
        int32_t value = 0;

        if (!ReadInt2(L, id, value)) {
            return 0;
        }

        for (Enchant& c : M().pending.enchants) {
            if (c.id == static_cast<uint32_t>(id)) {
                c.*field = static_cast<uint32_t>(value);
                break;
            }
        }

        return PushOk(L);
    }

    int SetEnchantLevel(lua_State* L) {
        return SetEnchantField(L, &Enchant::level);
    }

    int SetEnchantFlags(lua_State* L) {
        return SetEnchantField(L, &Enchant::flags);
    }

    // SetEnchantStacks (0x10106db0): sets +0x04 on the first match and, unlike the two above, returns nothing.
    int SetEnchantStacks(lua_State* L) {
        int32_t id = 0;
        int32_t value = 0;

        if (!ReadInt2(L, id, value)) {
            return 0;
        }

        for (Enchant& c : M().pending.enchants) {
            if (c.id == static_cast<uint32_t>(id)) {
                c.stacks = static_cast<uint32_t>(value);
                break;
            }
        }

        return 0;
    }

    // Weapon and armour types.
    template <size_t N>
    int AddTyped(lua_State* L, const char* const (&table)[N], uint32_t defaultType, std::vector<Typed>& v) {
        Typed t;
        t.type = defaultType;

        if (!ReadTypedArg(L, table, t)) {
            return 0;
        }

        auto it = std::find_if(v.begin(), v.end(), [&](const Typed& o) { return o.type == t.type; });   // FUN_100fa650

        if (it == v.end()) {
            v.push_back(t);
        } else {
            *it = t;
        }

        SortEntry(M().pending);
        return PushOk(L);
    }

    template <size_t N>
    int CanAddTyped(lua_State* L, const char* const (&table)[N], uint32_t defaultType) {
        Typed t;
        t.type = defaultType;

        if (!ReadTypedArg(L, table, t)) {
            return 0;
        }

        return PushOk(L);
    }

    template <size_t N>
    int RemoveTyped(lua_State* L, const char* const (&table)[N], std::vector<Typed>& v) {
        std::string name;

        if (!ReadString(L, name)) {
            return 0;
        }

        const uint32_t type = EnumOrLog(table, name, kNoValue);   // FUN_100effa0 / FUN_100efda0
        auto it = std::find_if(v.begin(), v.end(), [&](const Typed& o) { return o.type == type; });

        if (it != v.end()) {
            v.erase(it);
            SortEntry(M().pending);
        }

        return PushOk(L);
    }

    template <size_t N>
    int CanRemoveTyped(lua_State* L, const char* const (&table)[N]) {
        std::string name;

        if (!ReadString(L, name)) {
            return 0;
        }

        EnumOrLog(table, name, kNoValue);
        return PushOk(L);
    }

    template <size_t N, size_t K>
    int CanSetTypedComment(lua_State* L, const char* const (&types)[N], const char* const (&results)[K]) {
        if (!ValidateInput(L, { LUA_TSTRING, LUA_TSTRING })) {   // FUN_100d0770
            return 0;
        }

        EnumOrLog(types, std::string(CheckString(L, 1)), kNoValue);
        const std::string comment = CheckString(L, 2);
        std::vector<uint32_t> errors;

        if (comment.size() > 0x80) {
            errors.push_back(1);
        }

        return PushCan(L, results, errors);
    }

    int AddWeaponType(lua_State* L) {
        return AddTyped(L, kWeaponTypes, 0x15, M().pending.weapons);
    }

    int AddArmorType(lua_State* L) {
        return AddTyped(L, kArmorTypes, 0xB, M().pending.armor);
    }

    int CanAddWeaponType(lua_State* L) {
        return CanAddTyped(L, kWeaponTypes, 0x15);
    }

    int CanAddArmorType(lua_State* L) {
        return CanAddTyped(L, kArmorTypes, 0xB);
    }

    int RemoveWeaponType(lua_State* L) {
        return RemoveTyped(L, kWeaponTypes, M().pending.weapons);
    }

    int RemoveArmorType(lua_State* L) {
        return RemoveTyped(L, kArmorTypes, M().pending.armor);
    }

    int CanRemoveWeaponType(lua_State* L) {
        return CanRemoveTyped(L, kWeaponTypes);
    }

    int CanRemoveArmorType(lua_State* L) {
        return CanRemoveTyped(L, kArmorTypes);
    }

    int CanSetWeaponTypeComment(lua_State* L) {
        return CanSetTypedComment(L, kWeaponTypes, kSetWeaponCommentResults);
    }

    int CanSetArmorTypeComment(lua_State* L) {
        return CanSetTypedComment(L, kArmorTypes, kSetArmorCommentResults);
    }

    // ---- server round-trips ------------------------------------------------------------------------------------
    int CanPublishBuild(lua_State* L) {
        const uint32_t code = ValidatePublish(M().pending.id.empty());
        std::vector<uint32_t> errors;

        if (code) {
            errors.push_back(code);
        }

        return PushCan(L, kPublishResults, errors);
    }

    // CMSG 0x626 create (no id yet) or 0x628 save, carrying the pending entry. Not validated here.
    int PublishBuild(lua_State* L) {
        CoA::Packet pk(M().pending.id.empty() ? 0x626 : 0x628);
        WriteEntry(pk, M().pending);
        pk.Send();
        PushBool(L, true);
        return 1;
    }

    int ActivateBuild(lua_State* L) {   // CMSG 0x5C2: id, u8, u8
        if (!ValidateInput(L, { LUA_TSTRING, LUA_TBOOLEAN, LUA_TBOOLEAN })) {
            return 0;
        }

        const std::string id = CheckString(L, 1);
        const uint8_t a = lua_toboolean(L, 2) != 0;
        const uint8_t b = lua_toboolean(L, 3) != 0;
        CoA::Packet(0x5C2).Str(id.c_str()).U8(a).U8(b).Send();
        PushBool(L, true);
        return 1;
    }

    int DeactivateBuild(lua_State* L) {   // CMSG 0x5C4: id
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        CoA::Packet(0x5C4).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // CanActivateBuild / CanDeactivateBuild (FUN_10101f30): the id is known.
    int CanActivateBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        PushBool(L, FindBuild(id) != nullptr);
        lua_pushnil(L);
        return 2;
    }

    int RateBuild(lua_State* L) {   // CMSG 0x62A: id, u8 up; remembered for SMSG 0x62B
        if (!ValidateInput(L, { LUA_TSTRING, LUA_TBOOLEAN })) {
            return 0;
        }

        const std::string id = CheckString(L, 1);
        const bool up = lua_toboolean(L, 2) != 0;
        M().ratedId = id;
        M().rateUp = up;
        CoA::Packet(0x62A).Str(id.c_str()).U8(up ? 1 : 0).Send();
        PushBool(L, true);
        return 1;
    }

    int DeleteBuild(lua_State* L) {   // CMSG 0x62C: id; remembered for SMSG 0x62D
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        M().deletedId = id;
        CoA::Packet(0x62C).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryAllBuilds(lua_State* L) {   // CMSG 0x62E: the category name, remembered as the enum
        std::string name;

        if (!ReadString(L, name)) {
            return 0;
        }

        M().category = EnumOrLog(kBcCategories, name, kNoValue);
        CoA::Packet(0x62E).Str(name.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryBuild(lua_State* L) {   // CMSG 0x630: id
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        CoA::Packet(0x630).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_100fa7f0: a queried build moves to the front; one only on the category page is copied there.
    int BookmarkBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        std::vector<Entry>& q = M().queried;
        auto it = std::find_if(q.begin(), q.end(), [&](const Entry& e) { return e.id == id; });

        if (it != q.end()) {
            Entry copy = *it;
            q.erase(it);
            q.insert(q.begin(), copy);
        } else {
            std::vector<Entry>& page = M().page;
            auto pt = std::find_if(page.begin(), page.end(), [&](const Entry& e) { return e.id == id; });

            if (pt != page.end()) {
                q.insert(q.begin(), *pt);
            }
        }

        return 0;
    }

    // C_BuildCreator.ExportBuild (FUN_10103e40): the link of a known build.
    int ExportBuild(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return 0;
        }

        if (const Entry* e = FindBuild(id)) {
            PushStr(L, ExportLink(*e).c_str());
        } else {
            lua_pushnil(L);
        }

        return 1;
    }

    // C_BuildEditor.ExportPendingBuild (0x10103f50, no arguments): the pending entry's link.
    int ExportPendingBuild(lua_State* L) {
        PushStr(L, ExportLink(M().pending).c_str());
        return 1;
    }

    int ImportBuild(lua_State* L) {   // the link into the pending build
        std::string url;

        if (ReadString(L, url)) {
            ImportLink(M().pending, url);
        }

        return 0;
    }

    // The active CA build (with the equipped mystic enchants) round-tripped through a link into the pending build.
    int ImportCurrentBuild(lua_State*) {
        const Entry current = EntryFromBuild(*CoA::CA::ActiveBuild(), true);
        ImportLink(M().pending, ExportLink(current));
        return 0;
    }

    int UpdateFilter(lua_State* L) {   // (text, {[arg] = bool}, {[sort] = bool})
        if (!ValidateInput(L, { LUA_TSTRING, LUA_TTABLE, LUA_TTABLE })) {   // FUN_100e9a50
            return 0;
        }

        const std::string text = CheckString(L, 1);
        const std::vector<uint32_t> args = TrueKeys(L, 2, kBuildFilters);
        const std::vector<uint32_t> sorts = TrueKeys(L, 3, kBuildSorts);
        Builds().ApplyFilter(text, args, sorts);
        return 0;
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("C_BuildCreator", "UpdateFilter", UpdateFilter),
        COA_NATIVE("C_BuildCreator", "ExportBuild", ExportBuild),
        COA_NATIVE("C_BuildEditor", "ExportPendingBuild", ExportPendingBuild),
        COA_NATIVE("C_BuildEditor", "ImportBuild", ImportBuild),
        COA_NATIVE("C_BuildEditor", "ImportCurrentBuild", ImportCurrentBuild),
        COA_NATIVE("C_BuildCreator", "GetNumBuilds", GetNumBuilds),
        COA_NATIVE("C_BuildCreator", "GetBuildAtIndex", GetBuildAtIndex),
        COA_NATIVE("C_BuildCreator", "GetBuild", GetBuild),
        COA_NATIVE("C_BuildEditor", "GetPendingBuild", GetPendingBuild),
        COA_NATIVE("C_BuildCreator", "GetActiveBuild", GetActiveBuild),
        COA_NATIVE("C_BuildCreator", "GetActiveSpecForBuild", GetActiveSpecForBuild),
        COA_NATIVE("C_BuildCreator", "GetNumBookmarkedBuilds", GetNumBookmarkedBuilds),
        COA_NATIVE("C_BuildCreator", "GetBookmarkedBuildAtIndex", GetBookmarkedBuildAtIndex),
        COA_NATIVE("C_BuildCreator", "IsOwnedBuild", IsOwnedBuild),
        COA_NATIVE("C_BuildCreator", "IsUpvotedBuild", IsUpvotedBuild),
        COA_NATIVE("C_BuildEditor", "DoesBuildHaveSpellID", DoesBuildHaveSpellID),
        COA_NATIVE("C_BuildEditor", "DoesBuildHaveEnchant", DoesBuildHaveEnchant),
        COA_NATIVE("C_BuildCreator", "GetSpell", GetSpell),
        COA_NATIVE("C_BuildEditor", "GetSpellByID", GetSpellByID),
        COA_NATIVE("C_BuildEditor", "GetEssenceForLevel", GetEssenceForLevel),
        COA_NATIVE("C_BuildEditor", "GetQualityInfo", GetQualityInfo),
        COA_NATIVE("C_BuildEditor", "GetQualityInfoForLevel", GetQualityInfoForLevel),
        COA_NATIVE("C_BuildEditor", "EditBuild", EditBuild),
        COA_NATIVE("C_BuildEditor", "DiscardPendingBuild", DiscardPendingBuild),
        COA_NATIVE("C_BuildEditor", "SetName", SetName),
        COA_NATIVE("C_BuildEditor", "SetIcon", SetIcon),
        COA_NATIVE("C_BuildEditor", "SetDescription", SetDescription),
        COA_NATIVE("C_BuildEditor", "SetCategory", SetCategory),
        COA_NATIVE("C_BuildEditor", "SetRoles", SetRoles),
        COA_NATIVE("C_BuildEditor", "SetPrimaryStat", SetPrimaryStat),
        COA_NATIVE("C_BuildEditor", "SetDifficultyRating", SetDifficultyRating),
        COA_NATIVE("C_BuildEditor", "SetComment", SetComment),
        COA_NATIVE("C_BuildEditor", "AddSpell", AddSpell),
        COA_NATIVE("C_BuildEditor", "CanAddSpell", CanAddSpellL),
        COA_NATIVE("C_BuildEditor", "RemoveSpell", RemoveSpell),
        COA_NATIVE("C_BuildEditor", "CanRemoveSpell", CanRemoveSpellL),
        COA_NATIVE("C_BuildEditor", "SetSpellLevel", SetSpellLevel),
        COA_NATIVE("C_BuildEditor", "CanSetSpellLevel", CanSetSpellLevelL),
        COA_NATIVE("C_BuildEditor", "SetSpellFlags", SetSpellFlags),
        COA_NATIVE("C_BuildEditor", "CanSetSpellFlags", CanSetInt2),
        COA_NATIVE("C_BuildEditor", "SetIsCoreAbility", SetIsCoreAbility),
        COA_NATIVE("C_BuildEditor", "SetIsOptimalAbility", SetIsOptimalAbility),
        COA_NATIVE("C_BuildEditor", "SetIsEmpoweringAbility", SetIsEmpoweringAbility),
        COA_NATIVE("C_BuildEditor", "SetIsSynergisticAbility", SetIsSynergisticAbility),
        COA_NATIVE("C_BuildEditor", "CanSetIsCoreAbility", CanSetIsCoreAbility),
        COA_NATIVE("C_BuildEditor", "CanSetIsOptimalAbility", CanSetIsOptimalAbility),
        COA_NATIVE("C_BuildEditor", "CanSetIsEmpoweringAbility", CanSetIsEmpoweringAbility),
        COA_NATIVE("C_BuildEditor", "CanSetIsSynergisticAbility", CanSetIsSynergisticAbility),
        COA_NATIVE("C_BuildEditor", "CanSetSpellComment", CanSetSpellComment),
        COA_NATIVE("C_BuildEditor", "AddRandomEnchant", AddRandomEnchant),
        COA_NATIVE("C_BuildEditor", "CanAddRandomEnchant", CanAddRandomEnchant),
        COA_NATIVE("C_BuildEditor", "RemoveRandomEnchant", RemoveRandomEnchant),
        COA_NATIVE("C_BuildEditor", "CanRemoveRandomEnchant", CanRemoveRandomEnchant),
        COA_NATIVE("C_BuildEditor", "SetEnchantLevel", SetEnchantLevel),
        COA_NATIVE("C_BuildEditor", "CanSetEnchantLevel", CanSetInt2),
        COA_NATIVE("C_BuildEditor", "SetEnchantFlags", SetEnchantFlags),
        COA_NATIVE("C_BuildEditor", "SetEnchantStacks", SetEnchantStacks),
        COA_NATIVE("C_BuildEditor", "CanSetEnchantFlags", CanSetInt2),
        COA_NATIVE("C_BuildEditor", "CanSetRandomEnchantStacks", CanSetInt2),
        COA_NATIVE("C_BuildEditor", "CanSetRandomEnchantComment", CanSetRandomEnchantComment),
        COA_NATIVE("C_BuildEditor", "AddWeaponType", AddWeaponType),
        COA_NATIVE("C_BuildEditor", "CanAddWeaponType", CanAddWeaponType),
        COA_NATIVE("C_BuildEditor", "RemoveWeaponType", RemoveWeaponType),
        COA_NATIVE("C_BuildEditor", "CanRemoveWeaponType", CanRemoveWeaponType),
        COA_NATIVE("C_BuildEditor", "CanSetWeaponTypeComment", CanSetWeaponTypeComment),
        COA_NATIVE("C_BuildEditor", "AddArmorType", AddArmorType),
        COA_NATIVE("C_BuildEditor", "CanAddArmorType", CanAddArmorType),
        COA_NATIVE("C_BuildEditor", "RemoveArmorType", RemoveArmorType),
        COA_NATIVE("C_BuildEditor", "CanRemoveArmorType", CanRemoveArmorType),
        COA_NATIVE("C_BuildEditor", "CanSetArmorTypeComment", CanSetArmorTypeComment),
        COA_NATIVE("C_BuildEditor", "CanPublishBuild", CanPublishBuild),
        COA_NATIVE("C_BuildEditor", "PublishBuild", PublishBuild),
        COA_NATIVE("C_BuildCreator", "ActivateBuild", ActivateBuild),
        COA_NATIVE("C_BuildCreator", "CanActivateBuild", CanActivateBuild),
        COA_NATIVE("C_BuildCreator", "DeactivateBuild", DeactivateBuild),
        COA_NATIVE("C_BuildCreator", "CanDeactivateBuild", CanActivateBuild),
        COA_NATIVE("C_BuildCreator", "RateBuild", RateBuild),
        COA_NATIVE("C_BuildCreator", "DeleteBuild", DeleteBuild),
        COA_NATIVE("C_BuildCreator", "QueryAllBuilds", QueryAllBuilds),
        COA_NATIVE("C_BuildCreator", "QueryBuild", QueryBuild),
        COA_NATIVE("C_BuildCreator", "BookmarkBuild", BookmarkBuild),
    };
}

void CoA::BuildCreator::detail::InitNatives() {
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
}
