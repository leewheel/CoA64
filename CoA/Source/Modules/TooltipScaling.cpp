// AscTooltipScaling.cpp (see TooltipScaling.h): the scaled-item tooltip context and the hooks' logic.
#include "CoA/Source/Modules/TooltipScaling.h"
#include "CoA/Source/Modules/BridgeStore.h"
#include "CoA/Source/Modules/GameEvents.h"
#include "CoA/Source/Modules/ItemStats.h"
#include "CoA/Source/Modules/Manastorm.h"
#include "CoA/Source/Modules/Modules.h"
#include "CoA/Source/Modules/ObjectAddon.h"
#include "CoA/Source/Modules/RealmInfo.h"
#include "CoA/Source/Runtime/ClientHooks.h"
#include "CoA/Source/Runtime/Dbc.h"
#include "CoA/Source/Runtime/Packets.h"
#include "CoA/Source/Runtime/Read.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
    using CoA::Script::CheckNumber;
    using CoA::Script::ToInt;
    using Player = CoA::TooltipScaling::Player;

    CoA::TooltipScaling::Deps s_deps;

    // The tooltip context (0x10BE2EB0 ..).
    uint8_t s_scaling = 1;                  // DAT_10BCB6A0 (nothing clears it)
    uint8_t s_rewriteFirst = 1;             // DAT_10BCB6A1
    uint32_t s_item = 0;                    // 0x10BE2EB0
    uint64_t s_linkGuid = 0;                // 0x10BE2EC0 (nothing reads it)
    int32_t s_damage[4] = {};               // 0x10BE2EC8 / ECC / ED0 / ED4
    bool s_haveCache = false;               // 0x10BE2EDC != 0
    CoA::TooltipScaling::CacheItem s_cache; // *0x10BE2EDC
    uint8_t s_trade = 0;                    // 0x10BE2EE0
    uint8_t s_tradeSkill = 0;               // 0x10BE2EE1 (nothing reads it)
    uint8_t s_lootRoll = 0;                 // 0x10BE2EE2
    uint8_t s_compare = 0;                  // 0x10BE2EE3
    uint32_t s_lootRollId = 0;              // 0x10BE2EE4
    uint32_t s_tradeSlot = 0;               // 0x10BE2EE8
    uint32_t s_questLevel = 0;              // 0x10BE2EEC
    uint64_t s_compareA = 0;                // 0x10BE2EF0
    uint64_t s_compareB = 0;                // 0x10BE2EF8
    uint32_t s_guildBankLevel = 0;          // 0x10BE2F00
    uint32_t s_auctionLevel = 0;            // 0x10BE2F04
    // 0x10BE2F10: 19 from SMSG 0x716; slot 19 (SetInspectedSlot allows it) reads past them in the original -- a zero here.
    uint32_t s_inspectLevels[20] = {};
    std::unordered_map<const void*, int> s_onTextChanged;   // 0x10BE2F80: GameTooltip object -> registry ref

    uint32_t ItemLevelOf(uint64_t guid) {   // FUN_102cdbb0 field 0
        return CoA::ObjectAddon::Item(guid)[0].value;
    }

    bool HavePlayer(Player& p) {
        return s_deps.player && s_deps.player(p);
    }

    bool PlayerScales() {   // FUN_1027adc0: the player's unit addon field 87
        Player p;
        const uint64_t guid = HavePlayer(p) ? p.guid : 0;
        return CoA::ObjectAddon::Unit(guid)[87].value != 0;
    }

    // (double)(uint32) the MSVC way (0x10B1B220).
    double UintToDouble(uint32_t v) {
        return static_cast<double>(static_cast<int32_t>(v)) + (static_cast<int32_t>(v) < 0 ? 4294967296.0 : 0.0);
    }

    float F(const uint8_t* rec, uint32_t off) {
        float v;
        memcpy(&v, rec + off, 4);
        return v;
    }

    int32_t I(const uint8_t* rec, uint32_t off) {
        int32_t v;
        memcpy(&v, rec + off, 4);
        return v;
    }

    float AsFloat(uint32_t bits) {
        float v;
        memcpy(&v, &bits, 4);
        return v;
    }

    // Ruling: GetGlobalString's nullptr would make the original's std::string throw; here it reads as "".
    std::string GlobalString(const char* key) {
        const char* s = s_deps.globalString ? s_deps.globalString(key, 1) : nullptr;
        return s ? s : "";
    }

    int Format(char* out, size_t size, const char* fmt, ...) {   // FUN_100b82f0
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(out, size, fmt, args);
        va_end(args);
        return n < 0 ? -1 : n;
    }

    // FUN_10213cd0 on a QuestTemplateScaling row: the level a quest reward scales to for `level`.
    uint32_t QuestScaledLevel(const uint8_t* row, uint32_t level) {
        const uint32_t lo = CoA::Dbc::Table::U32(row, 4);
        const uint32_t hi = CoA::Dbc::Table::U32(row, 8);

        if (level < lo) {
            return lo;
        }

        for (uint32_t i = 0; i < 5; ++i) {
            if (CoA::Dbc::Table::U32(row, 0xC + i * 4) <= level) {
                level += CoA::Dbc::Table::U32(row, 0x24 + i * 4);
                break;
            }
        }

        if (hi < level) {
            level = hi;
        }

        if (level < lo) {
            level = lo;
        }

        return level;
    }

    uint32_t PlayerClass() {
        Player p;
        return HavePlayer(p) ? p.cls : 0;
    }

    bool StockClass() {   // FUN_100c6380
        const uint32_t c = PlayerClass();
        return (c >= 1 && c <= 9) || c == 0xB;
    }

    bool CoAClass() {   // FUN_100c5f50
        const uint32_t c = PlayerClass();
        return c >= 0xC && c <= 0x20;
    }

    float AverageDamage(const uint8_t* rec) {   // FUN_102102c0
        const float max = F(rec, 0x64);
        return max != 0.0f ? (max + F(rec, 0x60)) * 0.5f : 0.0f;
    }

    // ---- comparison (FUN_1027cb00) ----
    // FUN_10210430: a record image built from the item cache (and the addon's reborn armor) for an item without a
    // scaled level.
    void CacheRecord(uint32_t out[0x2A], uint32_t item) {
        memset(out, 0, 0x2A * 4);
        CoA::TooltipScaling::CacheItem c;

        if (!s_deps.itemCache || !s_deps.itemCache(item, c)) {
            return;
        }

        out[2] = item;
        out[3] = c.itemLevel;

        for (uint32_t i = 0; i < 10; ++i) {
            out[4 + i * 2] = c.statType[i];
            out[5 + i * 2] = c.statValue[i];
        }

        out[0x18] = c.damageMin[0];
        out[0x19] = c.damageMax[0];
        out[0x1B] = c.damageMin[1];
        out[0x1C] = c.damageMax[1];
        out[0x1E] = c.resistances[0];
        const CoA::ItemAddon::Record* addon = CoA::ItemAddon::Find(item);
        out[0x1F] = addon ? addon->fields[7] : c.resistances[0];

        for (uint32_t i = 0; i < 6; ++i) {
            out[0x20 + i] = c.resistances[1 + i];
        }

        out[0x26] = c.block;
        out[0x27] = c.randomProperty;
        out[0x28] = c.requiredLevel;
    }

    void CompareRecord(uint32_t out[0x2A], uint32_t entry, uint64_t guid) {
        const uint32_t level = ItemLevelOf(guid);

        if (level && CoA::ItemStats::Get(entry, level)) {
            memcpy(out, CoA::ItemStats::Get(entry, ItemLevelOf(guid)), 0x2A * 4);
            return;
        }

        CacheRecord(out, entry);
    }

    float Delay(const uint32_t rec[0x2A]) {
        CoA::TooltipScaling::CacheItem c;
        return s_deps.itemCache && s_deps.itemCache(rec[2], c) ? static_cast<float>(UintToDouble(c.delay)) : 2.0f;
    }

    float Dps(const uint32_t rec[0x2A], float delay) {
        const float max = AsFloat(rec[0x19]);
        return max == 0.0f ? 0.0f : (max + AsFloat(rec[0x18])) * 0.5f / (delay * 0.001f);
    }

    // FUN_10210780: stat `type` in a minus its value in b (0 where absent).
    int32_t StatDelta(const uint32_t a[0x2A], const uint32_t b[0x2A], int32_t type) {
        int32_t va = 0;

        for (uint32_t i = 0; i < 10; ++i) {
            if (static_cast<int32_t>(a[4 + i * 2]) == type) {
                va = static_cast<int32_t>(a[5 + i * 2]);
                break;
            }
        }

        for (uint32_t i = 0; i < 10; ++i) {
            if (static_cast<int32_t>(b[4 + i * 2]) == type) {
                return va - static_cast<int32_t>(b[5 + i * 2]);
            }
        }

        return va;
    }

    // ---- AddLine (FUN_1027b110) helpers ----
    int32_t StatTypeByName(const std::string& name) {   // FUN_10280760
        static const struct {
            const char* name;
            int32_t type;
        } kStats[] = {
            { "Mana", 0 }, { "Health", 1 }, { "Strength", 4 }, { "Agility", 3 }, { "Intellect", 5 }, { "Spirit", 6 },
            { "Stamina", 7 }, { "defense rating", 0xC }, { "your dodge rating", 0xD }, { "your parry rating", 0xE },
            { "your shield block rating", 0xF }, { "melee hit rating", 0x10 }, { "ranged hit rating", 0x11 },
            { "spell hit rating", 0x12 }, { "hit rating", 0x1F }, { "critical strike rating", 0x20 },
            { "haste rating", 0x24 }, { "your expertise rating", 0x25 }, { "attack power", 0x26 },
            { "mana per 5 sec", 0x2B }, { "your armor penetration rating", 0x2C }, { "spell power", 0x2D },
            { "health per 5 sec", 0x2E }, { "your spell penetration", 0x2F }, { "the block value of your shield", 0x30 },
        };

        for (const auto& s : kStats) {
            if (name == s.name) {
                return s.type;
            }
        }

        return -1;
    }

    // FUN_1027e550: the scaled value of a stat or resistance line; a school name becomes its resistance name.
    bool ScaledStat(char* name, size_t size, int32_t* value) {
        if (!s_scaling) {
            return false;
        }

        const uint8_t* rec = CoA::TooltipScaling::ScaledRecord();

        if (!rec) {
            return false;
        }

        for (uint32_t off = 0x10; off != 0x60; off += 8) {
            const int32_t type = I(rec, off);
            const int32_t v = I(rec, off + 4);

            if (type != -1 && v != 0 && type == StatTypeByName(std::string(name))) {
                *value = v;
                return true;
            }
        }

        static const struct {
            const char* school;
            const char* resistance;
            uint32_t off;
        } kSchools[] = {
            { "DAMAGE_SCHOOL3", "RESISTANCE2_NAME", 0x84 }, { "DAMAGE_SCHOOL7", "RESISTANCE6_NAME", 0x94 },
            { "DAMAGE_SCHOOL4", "RESISTANCE3_NAME", 0x88 }, { "DAMAGE_SCHOOL5", "RESISTANCE4_NAME", 0x8C },
            { "DAMAGE_SCHOOL6", "RESISTANCE5_NAME", 0x90 },
        };
        std::string schools[5];
        std::string resistances[5];

        for (uint32_t i = 0; i < 5; ++i) {
            schools[i] = GlobalString(kSchools[i].school);
            resistances[i] = GlobalString(kSchools[i].resistance);
        }

        for (uint32_t i = 0; i < 5; ++i) {
            if (strcmp(name, schools[i].c_str()) == 0) {
                *value = I(rec, kSchools[i].off);
                snprintf(name, size, "%s", resistances[i].c_str());
                return true;
            }
        }

        return false;
    }

    // FUN_1027e280: armor (the reborn armor for stock and CoA classes) and block.
    bool ScaledArmorOrBlock(const char* name, int32_t* value) {
        if (!s_scaling) {
            return false;
        }

        const std::string armor = GlobalString("ARMOR");
        const std::string block = GlobalString("BLOCK");
        const uint8_t* rec = CoA::TooltipScaling::ScaledRecord();

        if (!rec) {
            if (armor == name) {
                const CoA::ItemAddon::Record* addon = CoA::ItemAddon::Find(s_item);
                Player p;

                if (addon && HavePlayer(p) && (StockClass() || CoAClass())) {
                    *value = static_cast<int32_t>(addon->fields[7]);
                    return true;
                }
            }

            return false;
        }

        if (armor == name) {
            *value = I(rec, 0x78);
            Player p;

            if (HavePlayer(p) && (StockClass() || CoAClass())) {
                *value = I(rec, 0x7C);
            }

            return true;
        }

        if (block == name) {
            *value = I(rec, 0x98);
            return true;
        }

        return false;
    }

    // FUN_1027ee10: damage pair i of the scaled record, truncated.
    bool ScaledDamage(int32_t* min, int32_t* max, uint32_t i) {
        if (!s_scaling) {
            return false;
        }

        const uint8_t* rec = CoA::TooltipScaling::ScaledRecord();

        if (!rec) {
            return false;
        }

        *min = static_cast<int32_t>(F(rec, 0x60 + i * 0xC));
        *max = static_cast<int32_t>(F(rec, 0x64 + i * 0xC));
        return true;
    }

    // FUN_1008e380 on the player: under the level cap only a Dev realm at 60 / 70 / 80 passes.
    bool PowerLabelShown(uint32_t level) {
        if (level < CoA::GameEvents::ServerMaxLevel()) {
            if (!CoA::RealmInfo::Get().Dev() || (level != 0x3C && level != 0x46 && level != 0x50)) {
                return false;
            }
        }

        return true;
    }

    // FUN_1027f210: the "Classes:" line reduced to the classes of the player's own class model (CoA 12..32, stock 1..9 /
    // 11, else Hero 10), in ChrClasses order. Empty when every one of them is already listed, or without a player.
    std::string ClassesLine(const char* line) {
        Player p;
        const bool player = HavePlayer(p);
        const bool coa = player && CoAClass();
        const bool stock = player && StockClass();
        const bool hero = player && !coa && !stock;
        std::vector<const char*> names;
        const std::vector<std::pair<uint8_t, const char*>> classes =
            s_deps.classes ? s_deps.classes() : std::vector<std::pair<uint8_t, const char*>>();

        for (const auto& rec : classes) {
            const uint8_t cls = rec.first;
            bool include = false;

            if (coa) {
                include = cls >= 0xC && cls <= 0x20;
            } else if (stock) {
                include = (cls >= 1 && cls <= 9) || cls == 0xB;
            } else if (hero) {
                include = cls == 0xA;
            } else {
                return std::string();
            }

            if (include) {
                names.push_back(rec.second ? rec.second : "");
            }
        }

        for (const char* n : names) {
            if (strstr(line, n)) {
                continue;
            }

            std::string prefix = GlobalString("ITEM_CLASSES_ALLOWED");
            prefix.resize(prefix.size() >= 3 ? prefix.size() - 3 : 0);   // "Classes: %s" -> "Classes:"
            std::string result = prefix + " ";
            std::string listed(line);

            if (listed.compare(0, prefix.size(), prefix) == 0) {
                listed = listed.size() > 9 ? listed.substr(9) : std::string();
            }

            std::unordered_set<std::string> present;   // FUN_102871a0: split on ',' and trimmed
            std::istringstream in(listed);
            std::string token;

            while (std::getline(in, token, ',')) {
                size_t b = 0;
                size_t e = token.size();

                while (b < e && isspace(static_cast<unsigned char>(token[b]))) {
                    ++b;
                }

                while (e > b && isspace(static_cast<unsigned char>(token[e - 1]))) {
                    --e;
                }

                present.insert(token.substr(b, e - b));
            }

            bool first = true;

            for (const char* m : names) {
                if (present.count(m)) {
                    if (!first) {
                        result.append(", ", 2);
                    }

                    first = false;
                    result.append(m);
                }
            }

            return result;
        }

        return std::string();
    }

    CoA::TooltipScaling::Line Rewrite(const char* text, int colour = 0) {
        CoA::TooltipScaling::Line l;
        l.kind = CoA::TooltipScaling::Line::Rewrite;
        l.hasLeft = true;
        l.left = text;
        l.colour = colour;
        return l;
    }

    CoA::TooltipScaling::Line Drop() {
        CoA::TooltipScaling::Line l;
        l.kind = CoA::TooltipScaling::Line::Drop;
        return l;
    }
}

CoA::TooltipScaling::Deps& CoA::TooltipScaling::Dependencies() {
    return s_deps;
}

// FUN_1027eee0: the scaled record for the current tooltip item.
const uint8_t* CoA::TooltipScaling::ScaledRecord() {
    if (!s_scaling) {
        return nullptr;
    }

    const uint64_t tooltipGuid = CoA::ItemStats::TooltipGuid();

    if (tooltipGuid != 0) {
        uint32_t level = s_guildBankLevel;

        if (!level) {
            level = ItemLevelOf(tooltipGuid);
        }

        if (level) {
            return CoA::ItemStats::Get(s_item, level);
        }
    }

    if (s_trade) {
        return CoA::ItemStats::Get(s_item, s_deps.tradeLevel ? s_deps.tradeLevel(s_tradeSlot) : 0);
    }

    const CoA::ItemAddon::Record* addon = CoA::ItemAddon::Find(s_item);

    if (!addon) {
        return nullptr;
    }

    if (s_auctionLevel) {
        return CoA::ItemStats::Get(s_item, s_auctionLevel);
    }

    uint32_t level = 0;
    const uint32_t flags = addon->fields[1];   // +0x14
    bool fromPlayer = false;
    const std::vector<uint32_t>& rolls = CoA::ItemStats::LootRollLevels();

    if (!(flags >> 5 & 1)) {
        if ((flags >> 9 & 1) && CoA::Manastorm::InManastorm()) {
            fromPlayer = true;
        }
    } else if (s_lootRoll && s_lootRollId < rolls.size()) {
        level = rolls[s_lootRollId];
    } else if (s_deps.lootUnitLevel && s_deps.lootUnitLevel(level)) {
        // the looted unit's level
    } else if (s_deps.inspectTargetExists && s_deps.inspectTargetExists()) {
        const uint32_t slot = CoA::ItemStats::InspectedSlot();
        level = slot < 20 ? s_inspectLevels[slot] : 0;
    } else {
        fromPlayer = true;
    }

    if (fromPlayer) {
        Player p;

        if (HavePlayer(p)) {
            level = p.level;
        }
    }

    if (s_questLevel != 0 && PlayerScales()) {
        return CoA::ItemStats::Get(s_item, s_questLevel);
    }

    if (!PlayerScales()) {
        return nullptr;
    }

    if (level <= 0x14) {
        return CoA::ItemStats::Get(s_item, level + 2);
    }

    if (level < 0x1F) {
        return CoA::ItemStats::Get(s_item, level + 3);
    }

    if (level < 0x33) {
        return CoA::ItemStats::Get(s_item, level + 4);
    }

    switch (level) {
        case 0x3D:
            return CoA::ItemStats::Get(s_item, level + 0x19);
        case 0x3E:
            return CoA::ItemStats::Get(s_item, level + 0x1A);
        case 0x3F:
            return CoA::ItemStats::Get(s_item, level + 0x1C);
        case 0x40:
            return CoA::ItemStats::Get(s_item, level + 0x1E);
        case 0x41:
            return CoA::ItemStats::Get(s_item, level + 0x1F);
        case 0x42:
        case 0x43:
            return CoA::ItemStats::Get(s_item, level + 0x20);
        case 0x44:
            return CoA::ItemStats::Get(s_item, level + 0x21);
        case 0x45:
            return CoA::ItemStats::Get(s_item, level + 0x22);
        default:
            if (level >= 0x46 && level <= 0x50) {
                return CoA::ItemStats::Get(s_item, level + 0x23);
            }

            return CoA::ItemStats::Get(s_item, level + 5);
    }
}

// sub_1027c870 before the original. The tooltip +0x127 dword it also records is never read.
void CoA::TooltipScaling::BeforeSetItem(uint32_t item, const uint64_t* requester, const uint64_t* itemGuid, int32_t& skipLock) {
    if (s_guildBankLevel != 0) {
        skipLock = 0;
    }

    s_item = item;

    if (requester) {
        s_linkGuid = *requester;
    }

    if (!s_compare) {
        s_compareA = 0;
        s_compareB = 0;
    }

    if (itemGuid && *itemGuid != 0) {
        CoA::ItemStats::TooltipGuid() = *itemGuid;

        if (!s_compare) {
            s_compareA = *itemGuid;
        } else if (s_compareB == 0) {
            s_compareB = *itemGuid;
        }
    }

    s_haveCache = s_deps.itemCache && s_deps.itemCache(item, s_cache);
    const uint8_t* scaled = CoA::TooltipScaling::ScaledRecord();

    if (scaled && s_haveCache && I(scaled, 0xC) < static_cast<int32_t>(s_cache.itemLevel)) {
        CoA::ClientHooks::options.tooltipItemSet = false;   // FUN_1027da50(0)
    }

    if (s_compare) {
        CoA::ClientHooks::options.itemComparison = false;   // FUN_1027cf90(0)
    }

    if (!CoA::RealmInfo::Get().Dev() && s_haveCache && s_cache.quality == 4) {
        const uint32_t cap = CoA::GameEvents::ServerMaxLevel();
        const uint32_t required = s_cache.requiredLevel;

        if ((cap == 0x50 && required == 0x46) || (cap == 0x46 && required == 0x3C)) {
            CoA::ClientHooks::options.tooltipItemSet = false;
        }
    }
}

// After the original: the context cleared, the patches restored (0x627980's included).
void CoA::TooltipScaling::AfterSetItem() {
    s_item = 0;
    s_linkGuid = 0;
    CoA::ItemStats::TooltipGuid() = 0;
    memset(s_damage, 0, sizeof(s_damage));
    s_haveCache = false;
    s_cache = CacheItem();
    CoA::ClientHooks::options.tooltipItemSet = true;
    CoA::ClientHooks::options.itemComparison = true;
    s_scaling = 1;
    s_rewriteFirst = 1;
    s_questLevel = 0;
    s_guildBankLevel = 0;
    s_auctionLevel = 0;
    s_lootRollId = 0;
}

// FUN_1027b110. Each line is tested against the patterns in order; the first that applies rewrites it from the scaled
// record. (The original's required-form line under spell tooltips, FUN_10a10af0 at return address 0x625231, reads a
// spell id only a SetSpell detour the reconstruction has not transcribed writes; not ported.)
CoA::TooltipScaling::Line CoA::TooltipScaling::AddLine(const char* left, const char*, uint32_t leftColour) {
    Line pass;
    pass.hasLeft = left != nullptr;
    pass.left = left ? left : "";

    if (left && *left && strcmp(left, "Requires Two-Handed Axes, Two-Handed Maces, Polearms, Two-Handed Swords, Staves") == 0) {
        pass.left = "需要近战双手武器";   // 中文化（By leewheel 2026-10-29）
    }

    const char* l = pass.hasLeft ? pass.left.c_str() : nullptr;
    char scan[0x400];
    const std::string classes = GlobalString("ITEM_CLASSES_ALLOWED");

    if (!classes.empty() && l && *l && sscanf(l, classes.c_str(), scan) != 0) {
        const std::string line = ClassesLine(l);
        return line.empty() ? Drop() : Rewrite(line.c_str());
    }

    char out[0x400];
    char school[0x400] = {};
    int32_t min = 0;
    int32_t max = 0;

    {
        const std::string withSchool = GlobalString("DAMAGE_TEMPLATE_WITH_SCHOOL");
        const std::string damage = GlobalString("DAMAGE");

        if (l && *l && sscanf(l, withSchool.c_str(), &min, &max, school) == 3 && strcmp(school, damage.c_str()) != 0 &&
            ScaledDamage(&min, &max, 1)) {
            if (min == 0 || max == 0) {
                ScaledDamage(&min, &max, 0);
            }

            if (min != 0 && max != 0) {
                s_damage[1] = min;
                s_damage[3] = max;
                Format(out, sizeof(out), GlobalString("DAMAGE_TEMPLATE_WITH_SCHOOL").c_str(), min, max, school);
                return Rewrite(out);
            }
        }
    }

    {
        const std::string plain = GlobalString("DAMAGE_TEMPLATE");

        if (l && *l && sscanf(l, plain.c_str(), &min, &max) == 2 && s_scaling) {
            if (const uint8_t* rec = CoA::TooltipScaling::ScaledRecord()) {
                min = static_cast<int32_t>(F(rec, 0x60));
                max = static_cast<int32_t>(F(rec, 0x64));

                if (min != 0 && max != 0) {
                    s_damage[0] = min;
                    s_damage[2] = max;
                    Format(out, sizeof(out), plain.c_str(), min, max);
                    return Rewrite(out);
                }
            }
        }
    }

    // Ruling: the original's "%d %s" fills its 0x400 buffer unbounded; the width keeps it inside.
    char name[0x400] = {};
    int32_t value = 0;

    if (l && sscanf(l, "%d %1023s", &value, name) == 2) {
        const std::string armor = GlobalString("ARMOR");
        const bool green = leftColour == 0xFF1EFF00 || leftColour == 0xFF00FF00;
        const bool rewrite = !green || (armor == name && s_rewriteFirst);

        if (s_haveCache && s_cache.quality != 7 && rewrite) {
            if (ScaledStat(name, sizeof(name), &value)) {
                if (value != 0) {
                    Format(out, sizeof(out), value > 0 ? "+%d %s" : "%d %s", value, name);

                    if (armor == name) {
                        s_rewriteFirst = 0;
                    }

                    return Rewrite(out);
                }
            } else if (ScaledArmorOrBlock(name, &value) && value != 0) {
                Format(out, sizeof(out), "%d %s", value, name);
                s_rewriteFirst = 0;
                return Rewrite(out);
            }
        }
    }

    {
        const std::string itemLevel = GlobalString("ITEM_LEVEL");

        if (l && sscanf(l, itemLevel.c_str(), &value) == 1) {
            if (const uint8_t* rec = CoA::TooltipScaling::ScaledRecord()) {
                if (const int32_t level = I(rec, 0xC)) {
                    Format(out, sizeof(out), itemLevel.c_str(), level);
                    return Rewrite(out);
                }
            }
        }
    }

    const std::string minLevel = GlobalString("ITEM_MIN_LEVEL");

    if (l) {
        const uint8_t* scaled = sscanf(l, minLevel.c_str(), &value) == 1 ? CoA::TooltipScaling::ScaledRecord() : nullptr;

        if (scaled) {
            int32_t required = I(scaled, 0xC) - 5;

            if (const uint8_t* rec = CoA::TooltipScaling::ScaledRecord()) {
                required = I(rec, 0xA0);
            }

            if (required != 0) {
                Format(out, sizeof(out), minLevel.c_str(), required);
                Player p;

                if (!HavePlayer(p)) {
                    return Rewrite(out);
                }

                return Rewrite(out, static_cast<uint32_t>(required) > p.level ? 2 : 1);
            }
        }

        float dps = 0.0f;

        if (sscanf(l, "(%f damage per second)", &dps) == 1) {
            const int32_t min1 = s_damage[0];
            const int32_t max1 = s_damage[2];
            const int32_t min2 = s_damage[1];
            const int32_t max2 = s_damage[3];

            // Ruling: the original divides by the cache record's delay even when only the school pair is set and there
            // is no cache record (a fault); here the line passes then.
            if (s_haveCache && ((min1 != 0 && max1 != 0) || (min2 != 0 && max2 != 0))) {
                const float average = static_cast<float>(UintToDouble(static_cast<uint32_t>(max1 + min1 + max2 + min2))) * 0.5f;
                const float delay = static_cast<float>(UintToDouble(s_cache.delay)) / 1000.0f;
                Format(out, sizeof(out), "(%.1f damage per second)", static_cast<double>(average / delay));
                return Rewrite(out);
            }
        }

        if (strncmp(l, "Equip:", 6) == 0) {
            std::cmatch m;
            const std::regex increases("Equip: Increases (.*) by (\\d+).", std::regex::ECMAScript);

            if (std::regex_match(l, l + strlen(l), m, increases) && m[1].str().size() <= 0x40) {
                snprintf(name, sizeof(name), "%s", m[1].str().c_str());
                value = atoi(m[2].str().c_str());
                const std::string pve = GlobalString("PVE_POWER_LABEL");
                const std::string pvp = GlobalString("PVP_POWER_LABEL");
                const bool power = std::string(name) == pve || std::string(name) == pvp;
                Player p;

                if (power && HavePlayer(p) && !PowerLabelShown(p.level)) {
                    return Drop();
                }

                if (ScaledStat(name, sizeof(name), &value) && value != 0) {
                    Format(out, sizeof(out), "Equip: Increases %s by %d.", name, value);
                    return Rewrite(out);
                }
            }

            const std::regex improves("Equip: Improves (.*) by (\\d+).", std::regex::ECMAScript);

            if (std::regex_match(l, l + strlen(l), m, improves)) {
                snprintf(name, sizeof(name), "%s", m[1].str().c_str());
                value = atoi(m[2].str().c_str());

                if (ScaledStat(name, sizeof(name), &value) && value != 0) {
                    Format(out, sizeof(out), "Equip: Improves %s by %d.", name, value);
                    return Rewrite(out);
                }
            }

            const std::regex restores("Equip: Restores (\\d+) (.*).", std::regex::ECMAScript);

            if (std::regex_match(l, l + strlen(l), m, restores)) {
                value = atoi(m[1].str().c_str());
                snprintf(name, sizeof(name), "%s", m[2].str().c_str());

                if (ScaledStat(name, sizeof(name), &value) && value != 0) {
                    Format(out, sizeof(out), "Equip: Restores %d %s", value, name);
                    return Rewrite(out);
                }
            }
        }
    }

    return pass;
}

void CoA::TooltipScaling::CompareDeltas(int32_t* summary) {
    uint32_t entryA = 0;
    uint32_t entryB = 0;

    if (!s_deps.itemEntry || !s_deps.itemEntry(s_compareA, entryA) || !s_deps.itemEntry(s_compareB, entryB)) {
        return;
    }

    uint32_t a[0x2A];
    uint32_t b[0x2A];
    CompareRecord(a, entryA, s_compareA);
    CompareRecord(b, entryB, s_compareB);
    const float dps = Dps(a, Delay(a)) - Dps(b, Delay(b));   // FUN_10210300
    memcpy(&summary[0], &dps, 4);
    auto S = [](const uint32_t* r, uint32_t i) { return static_cast<int32_t>(r[i]); };
    summary[1] = S(a, 0x1E) - S(b, 0x1E);   // armor
    Player p;

    if (HavePlayer(p) && (StockClass() || CoAClass())) {
        summary[1] = S(a, 0x1F) - S(b, 0x1F);   // reborn armor
    }

    for (uint32_t i = 0; i < 6; ++i) {
        summary[2 + i] = S(a, 0x20 + i) - S(b, 0x20 + i);   // resistances
    }

    static const struct {
        uint32_t index;
        int32_t stat;
    } kStats[] = {
        { 0x08, 0x26 }, { 0x0F, 3 }, { 0x10, 4 }, { 0x11, 5 }, { 0x12, 6 }, { 0x13, 7 }, { 0x18, 0xC }, { 0x19, 0xD },
        { 0x1B, 0xF }, { 0x20, 0x14 }, { 0x2B, 0x1F }, { 0x2C, 0x20 }, { 0x3A, 0x2E },
    };

    for (const auto& s : kStats) {
        summary[s.index] = StatDelta(a, b, s.stat);
    }

    summary[0x3D] = StatDelta(a, b, 0x30) + (S(a, 0x26) - S(b, 0x26));   // block value + block
    static const struct {
        uint32_t index;
        int32_t stat;
    } kStats2[] = {
        { 0x2F, 0x23 }, { 0x30, 0x24 }, { 0x38, 0x2C }, { 0x39, 0x2D }, { 0x3E, 0x2B },
    };

    for (const auto& s : kStats2) {
        summary[s.index] = StatDelta(a, b, s.stat);
    }
}

uint32_t CoA::TooltipScaling::Money(uint32_t money) {
    if (s_scaling) {
        if (const uint8_t* r = CoA::TooltipScaling::ScaledRecord()) {
            return static_cast<uint32_t>(I(r, 0xA4));
        }
    }

    return money;
}

// A weapon (class 2) of inventory type 13 / 17 / 21 / 22 gets the module's own feral attack power from its dps.
bool CoA::TooltipScaling::FeralAttackPower(const CacheItem& item, float& damage, int32_t& result) {
    if (const uint8_t* r = CoA::TooltipScaling::ScaledRecord()) {
        damage = AverageDamage(r);
    }

    const uint32_t t = item.inventoryType;

    if (item.cls != 2 || (t != 0xD && t != 0x11 && t != 0x15 && t != 0x16)) {
        return false;
    }

    const float delay = static_cast<float>(UintToDouble(item.delay)) / 1000.0f;
    const float dps = damage / delay;
    const float low = (dps - 30.0f) * 8.0f;
    const float high = (dps - 54.81f) * 14.0f + 200.0f;
    float v = low > 0.0f ? low : 0.0f;
    v = v < 200.0f ? v : 200.0f;
    const float ap = high > v ? high : v;
    const int32_t n = static_cast<int32_t>(ap);
    result = n > 0 ? n : 0;
    return true;
}

// sub_1027ae80 (replaces 0x708540): the item's own level (else the cache's); DurabilityCosts row level - 1, the weapon
// column for class 2, the armor column (+0x15) for class 4, column 0 for anything else; no wrapped test.
uint32_t CoA::TooltipScaling::RepairCost(uint32_t entry, uint64_t guid, uint32_t durability, uint32_t maxDurability) {
    CacheItem c;

    if (!s_deps.itemCache || !s_deps.itemCache(entry, c) || !maxDurability) {
        return 0;
    }

    if (durability > maxDurability || maxDurability == durability) {
        return 0;
    }

    const uint32_t lost = maxDurability - durability;
    uint32_t level = ItemLevelOf(guid);

    if (!level) {
        level = c.itemLevel;
    }

    uint32_t column = c.subClass;

    if (c.cls == 2) {
    } else if (c.cls == 4) {
        column += 0x15;
    } else {
        column = 0;
    }

    column &= 0xFF;
    uint32_t cost = 0;
    float quality = 0.0f;

    if (!s_deps.durabilityCost || !s_deps.durabilityCost(level, column, cost)) {
        return 0;
    }

    if (!s_deps.durabilityQuality || !s_deps.durabilityQuality(c.quality, quality)) {
        return 0;
    }

    const uint32_t base = cost * lost;
    const int32_t total = static_cast<int32_t>(static_cast<int64_t>(UintToDouble(base) * static_cast<double>(quality)));
    return total ? static_cast<uint32_t>(total) : 1;
}

int32_t CoA::TooltipScaling::CanEquip(int32_t result, uint32_t requiredLevel, bool haveLastItem, uint64_t lastItemGuid) {
    if (result != 0) {
        return result;
    }

    Player p;

    if (HavePlayer(p) && requiredLevel > 1 && p.level < requiredLevel && haveLastItem) {
        const uint32_t level = ItemLevelOf(lastItemGuid);

        if (level != 0 && level - 5 <= p.level) {
            return 1;
        }
    }

    return result;
}

// sub_10283fe0 / sub_10284040: the quest (*0xC0D65C / *0xC23AD8) has a QuestTemplateScaling row.
void CoA::TooltipScaling::SetQuestContext(uint32_t quest) {
    Player p;

    if (!HavePlayer(p)) {
        return;
    }

    CoA::Dbc::Table& t = CoA::Dbc::Get("DBFilesClient\\QuestTemplateScaling.dbc");

    if (!t.Loaded() || quest < t.MinId() || quest > t.MaxId()) {
        return;
    }

    if (const uint8_t* row = t.Row(quest)) {
        s_questLevel = QuestScaledLevel(row, p.level);
    }
}

void CoA::TooltipScaling::SetTradeSkill(bool on) {
    s_tradeSkill = on ? 1 : 0;
}

// FUN_1027b070 (player) leaves the trade flag set afterwards; FUN_1027b0c0 (target) clears it. Both read argument 2.
void CoA::TooltipScaling::BeginTrade(lua_State* L) {
    s_trade = 1;
    s_tradeSlot = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)) - 1);
}

void CoA::TooltipScaling::EndTrade(bool clearFlag) {
    s_tradeSlot = 0;

    if (clearFlag) {
        s_trade = 0;
    }
}

// sub_1027afc0: the attachment's item GUID low part made an item GUID (high 0x4000 << 16; FUN_100a55d0).
void CoA::TooltipScaling::SetInboxItem(uint32_t itemGuidLow) {
    CoA::ItemStats::TooltipGuid() = itemGuidLow ? (static_cast<uint64_t>(0x40000000u) << 32 | itemGuidLow) : 0;
}

// FUN_10283e00: "bidder" / "owner" / "list"; the bound lets index == count + 1 through (one past the list).
void CoA::TooltipScaling::BeforeAuctionItem(lua_State* L) {
    const char* type = lua_tolstring(L, 2, nullptr);
    const int32_t index = ToInt(CheckNumber(L, 3));
    // Ruling: strcmp on a non-string argument faults in the original; here it is no list.
    const int list = !type ? -1 : strcmp(type, "bidder") == 0 ? 0 : strcmp(type, "owner") == 0 ? 1 : strcmp(type, "list") == 0 ? 2 : -1;
    const CoA::ItemStats::Deps& d = CoA::ItemStats::Dependencies();
    uint32_t count = 0;

    if (list < 0 || !d.auctionList || !d.auctionList(list, count) || !count) {
        return;
    }

    if (static_cast<uint32_t>(index - 1) < count + 1 && d.auctionEntry) {
        uint32_t item = 0;
        uint32_t level = 0;
        d.auctionEntry(list, static_cast<uint32_t>(index - 1), item, level);
        s_auctionLevel = level;
    }
}

void CoA::TooltipScaling::BeginLootRoll(lua_State* L) {
    s_lootRoll = 1;
    s_lootRollId = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
}

void CoA::TooltipScaling::EndLootRoll() {
    s_lootRoll = 0;
}

void CoA::TooltipScaling::SetGuildBankLevel(uint32_t level) {
    s_guildBankLevel = level;
}

bool CoA::TooltipScaling::BeginHyperlinkCompare(bool bankerOpen) {
    if (bankerOpen) {
        return false;
    }

    s_compare = 1;
    s_compareB = 0;
    return true;
}

void CoA::TooltipScaling::EndHyperlinkCompare() {
    s_compare = 0;
    s_compareB = 0;
}

uint64_t CoA::TooltipScaling::TooltipGuid() {
    return CoA::ItemStats::TooltipGuid();
}

bool CoA::TooltipScaling::OnTextChanged(const void* tooltip, int& ref) {
    auto it = s_onTextChanged.find(tooltip);

    if (it == s_onTextChanged.end()) {
        return false;
    }

    ref = it->second;
    return true;
}

void CoA::TooltipScaling::SetOnTextChanged(const void* tooltip, int ref) {
    s_onTextChanged[tooltip] = ref;
}

// SMSG 0x716: 19 u32 levels; unchecked reads in the original (CoA::Read: zeros past the end).
int32_t CoA::TooltipScaling::OnInspectLevels(void*, uint32_t, uint32_t, CDataStore* msg) {
    for (uint32_t i = 0; i < 0x13; ++i) {
        s_inspectLevels[i] = CoA::Read::U32(msg);
    }

    return 1;
}

uint32_t CoA::TooltipScaling::InspectLevel(uint32_t slot) {
    return slot < 20 ? s_inspectLevels[slot] : 0;
}

void CoA::TooltipScaling::Init() {
    COA_PACKET(0x716, CoA::TooltipScaling::OnInspectLevels);
}

void CoA::TooltipScaling::Reset() {
    s_deps = Deps();
    s_scaling = 1;
    s_rewriteFirst = 1;
    s_item = 0;
    s_linkGuid = 0;
    memset(s_damage, 0, sizeof(s_damage));
    s_haveCache = false;
    s_cache = CacheItem();
    s_trade = s_tradeSkill = s_lootRoll = s_compare = 0;
    s_lootRollId = s_tradeSlot = s_questLevel = s_guildBankLevel = s_auctionLevel = 0;
    s_compareA = s_compareB = 0;
    memset(s_inspectLevels, 0, sizeof(s_inspectLevels));
    s_onTextChanged.clear();
    CoA::ClientHooks::options.tooltipItemSet = true;
    CoA::ClientHooks::options.itemComparison = true;
}
