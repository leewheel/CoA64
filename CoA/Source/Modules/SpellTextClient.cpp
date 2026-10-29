// Spell text the original DLL builds from Spell.dbc (port of AscSpellText.cpp).
//
//   Description  FUN_101ac910 -> FUN_101ac3d0: the client's formatter SpellParserParseText (0x57ABC0) fills a 0x400-byte
//                buffer from the record, then FUN_101a8e30 rewrites the @-markup. Two caches, both appended on every
//                miss and read only with `cached`: the formatter output ({spell, char[0x400], time}, dropped after
//                300 s) and the markup result ({spell, string, time}, dropped after 20 s); the tick runs after
//                ClientIdle and every glue screen empties both.
//   Markup       FUN_101a8e30, below.
//   C_Format.Format (FUN_101acb30) and the global GetSpellDescription (FUN_10a4dd90) sit on top.
//   Name         spell -> Spell +0x220 ("" when the spell is missing).
//   FormText     FUN_101c6300: spell -> FUN_101c1b60.
#include "CoA/Source/Modules/Modules.h"
#include "CoA/Source/Modules/SpellText.h"
#include "CoA/Source/Runtime/CVars.h"
#include "CoA/Source/Runtime/Lifecycle.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/QuestTextParser.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_map>

namespace {
    const char* const s_questionMark = "Interface\\Icons\\INV_Misc_QuestionMark";

    // FetchSpell (Spell.dbc's compressed GetRecord, 0x4CFD20): the record copied out.
    bool FetchSpell(uint32_t id, SpellRec& rec) {
        return g_spellDB.GetRecord(static_cast<int32_t>(id), &rec) != 0;
    }

    template <class T>
    const T* Row(const WowClientDB<T>& db, uint32_t id) {
        return db.GetRecord(static_cast<int32_t>(id));
    }

    CoA::SpellText::Deps s_deps;

    bool SpellKnownByCA(uint32_t id) {
        return s_deps.spellKnown && s_deps.spellKnown(id, true);
    }

    bool MysticSlotted(uint32_t id) {
        return s_deps.mysticSlotted && s_deps.mysticSlotted(id);
    }

    // FUN_101c6160: the form's name, SpellShapeshiftForm +8.
    const std::string& FormName(uint32_t form) {
        static std::unordered_map<uint32_t, std::string> names;
        auto it = names.find(form);

        if (it != names.end()) {
            return it->second;
        }

        std::string name;

        if (const SpellShapeshiftFormRec* row = Row(g_spellShapeshiftFormDB, form)) {
            if (row->m_name) {
                name = row->m_name;
            }
        }

        return names.emplace(form, name).first->second;
    }

    // 中文化: 玩家可见 tooltip 文案（By leewheel 2026-10-29）


    // FUN_101c1b60: nothing when Spell +0x18 has bit 19 or the stance mask (+0x30/+0x34) is empty; otherwise
    // " <form> Requires <form>" for every form in the mask, in id order.
    std::string FormsOf(const SpellRec& rec) {
        std::string out;
        uint32_t attributes = static_cast<uint32_t>(rec.m_attributesExB);
        uint32_t mask0 = static_cast<uint32_t>(rec.m_shapeshiftMask[0]);
        uint32_t mask1 = static_cast<uint32_t>(rec.m_shapeshiftMask[1]);

        if ((attributes >> 19) & 1 || (mask0 == 0 && mask1 == 0)) {
            return out;
        }

        for (int32_t form = g_spellShapeshiftFormDB.m_minID; form <= g_spellShapeshiftFormDB.m_maxID; ++form) {
            uint32_t f = static_cast<uint32_t>(form);

            if (f == 0 || (f - 1) >> 5 >= 2) {
                continue;
            }

            if (!(static_cast<uint32_t>(rec.m_shapeshiftMask[(f - 1) >> 5]) & (1u << ((f - 1) & 0x1F)))) {
                continue;
            }

            const std::string& name = FormName(f);

            if (name.empty()) {
                continue;
            }

            out += " ";
            out += name;
            out += "需要";   // 中文化（By leewheel 2026-10-29）
            out += name;
        }

        return out;
    }

    int g_depth = 0;   // the thread-local recursion counter (TLS +8)

    struct FormattedEntry {
        uint32_t spell;
        char text[0x400];
        __time64_t time;
    };

    struct MarkupEntry {
        uint32_t spell;
        std::string text;
        __time64_t time;
    };

    std::vector<FormattedEntry> s_formatted;
    std::vector<MarkupEntry> s_markup;

    // The two world CVars the module registers; the markup tests their integer value == 1.
    bool CVarOn(const char* name) {
        CVar* var = CVar::Lookup(name);
        return var && var->m_intValue == 1;
    }

    bool KeyDown(uint32_t key) {
        return (static_cast<uint32_t>(EventIsKeyDown(static_cast<KEY>(key))) & 0xFF) != 0;
    }

    // Riding (skill line 762): the player's skill slot, then its value.
    int32_t RidingSkill(CGPlayer_C* player) {
        return static_cast<int32_t>(player->GetSkillValue(player->GetSkillIndex(0x2FA)));
    }

    std::string U(uint32_t v) {
        return std::to_string(v);
    }

    // std::format's "{}" for a float: the shortest round-trip form.
    std::string F(float v) {
        char buffer[64];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
        return std::string(buffer, result.ptr);
    }

    // FUN_101ae730: "|cff{color}{text}|r".
    std::string Colored(const char* color, const std::string& text) {
        return std::string("|cff") + color + text + "|r";
    }

    std::string IconTag(const std::string& icon, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return "|T" + icon + ":" + U(a) + ":" + U(b) + ":" + U(c) + ":" + U(d) + "|t";
    }

    // FUN_101acf80: the icon, unless it names no file the client can open (".blp" is appended for the test when
    // neither ".blp" nor ".BLP" appears) -- then the question mark.
    std::string ValidIcon(const std::string& icon) {
        if (icon.empty() || icon == s_questionMark) {
            return icon;
        }

        std::string path = icon;

        if ((path.size() < 4 || path.find(".blp") == std::string::npos) && (path.size() < 4 || path.find(".BLP") == std::string::npos)) {
            path += ".blp";
        }

        SFileImpl* file = nullptr;

        if (!SFile::OpenEx(nullptr, path.c_str(), 0x20000, &file)) {
            return s_questionMark;
        }

        SFileCloseFile(file);
        return icon;
    }

    std::string SpellIconPath(const SpellRec& rec) {
        std::string icon = s_questionMark;

        if (const SpellIconRec* row = Row(g_spellIconDB, static_cast<uint32_t>(rec.m_spellIconID))) {
            if (row->m_textureFilename) {
                icon = row->m_textureFilename;
            }
        }

        return ValidIcon(icon);
    }

    // The label and icon every spell-naming @tag@ builds: "Unknown Spell (<id>)" or the spell's name (plus
    // " (<rank>)" when asked and present), and its icon at 20x20.
    void SpellLabel(uint32_t id, bool withRank, std::string& label, std::string& icon) {
        label = "未知法术 (" + U(id) + ")";
        std::string rank;
        icon = s_questionMark;
        SpellRec rec;

        if (FetchSpell(id, rec)) {
            label = rec.m_name ? rec.m_name : "";

            if (withRank && rec.m_nameSubtext) {
                rank = rec.m_nameSubtext;
            }

            if (const SpellIconRec* row = Row(g_spellIconDB, static_cast<uint32_t>(rec.m_spellIconID))) {
                if (row->m_textureFilename) {
                    icon = row->m_textureFilename;
                }
            }
        }

        icon = IconTag(ValidIcon(icon), 20, 20, 0, 0);

        if (withRank && !rank.empty()) {
            label = label + " (" + rank + ")";
        }
    }

    // ---- the tooltip lines FUN_101adc90 can add -----------------------------------------------------------------
    std::string RangeLine(const SpellRec& rec) {   // FUN_101a8d10
        const SpellRangeRec* row = Row(g_spellRangeDB, static_cast<uint32_t>(rec.m_rangeIndex));

        if (!row) {
            return "|cffFF0000未知射程|r";   // 中文化（By leewheel 2026-10-29）
        }

        return "|cffFFFFFF" + F(row->m_rangeMax[0]) + " 码射程|r";
    }

    std::string CooldownLine(const SpellRec& rec) {   // FUN_101a8720
        uint32_t v = std::max(static_cast<uint32_t>(rec.m_recoveryTime), static_cast<uint32_t>(rec.m_categoryRecoveryTime));
        float f = v < 60000 ? static_cast<float>(static_cast<double>(v)) / 1000.0f : static_cast<float>(static_cast<double>(v)) / 60000.0f;
        return "|cffFFFFFF" + F(f) + (v < 60000 ? " 秒冷却时间|r" : " 分钟冷却时间|r");   // 中文化（By leewheel 2026-10-29）
    }

    std::string CastLine(const SpellRec& rec) {   // FUN_101a8600
        const SpellCastTimesRec* row = Row(g_spellCastTimesDB, static_cast<uint32_t>(rec.m_castingTimeIndex));

        if (!row) {
            return "|cffFF0000未知施法时间|r";   // 中文化（By leewheel 2026-10-29）
        }

        return "|cffFFFFFF" + F(static_cast<float>(row->m_base) / 1000.0f) + " 秒施法时间|r";
    }

    std::string PowerLine(const SpellRec& rec) {   // FUN_101a88a0
        static const std::unordered_map<uint32_t, uint32_t> insanity = { { 0xC48FC, 0x1E } };   // DAT_10bdf330
        uint32_t cost = static_cast<uint32_t>(rec.m_manaCost);

        if (cost == 0) {
            auto it = insanity.find(static_cast<uint32_t>(rec.m_id));

            if (it != insanity.end()) {
                return "|cffa54cff消耗 " + U(it->second) + " 点理智值|r";   // 中文化（By leewheel 2026-10-29）
            }
        }

        if (uint32_t pct = static_cast<uint32_t>(rec.m_manaCostPct)) {   // a percentage of UNIT_FIELD_BASE_MANA
            CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();
            cost = player ? (player->m_unit->m_unitFieldBaseMana * pct) / 100 : 0;
        }

        std::string name = "未知能量类型";   // 中文化（By leewheel 2026-10-29）

        switch (rec.m_powerType) {
            case 0:
                name = "法力值";
                break;
            case 1:
                name = "怒气值";
                cost = static_cast<uint32_t>(static_cast<float>(static_cast<double>(cost)) * 0.1f);
                break;
            case 2:
                name = "集中值";
                break;
            case 3:
                name = "能量值";
                break;
            case 4:
                name = "快乐值";
                break;
            case 5:
                name = "符文";
                break;
            case 6:
                name = "符能值";
                break;
            case -2:
                name = "生命值";
                break;
            default:
                break;
        }

        return "|cffFFFFFF" + U(cost) + " " + name + "|r";
    }

    // FUN_101adc90 (@s:) / FUN_101ad1f0 (@re:): byte-identical apart from the error text.
    std::string Render(const char* kind, uint32_t id, bool nameOnly, bool descOnly, bool cached, bool divider, bool range,
        bool cooldown, bool cast, bool power) {
        std::string out;
        SpellRec rec;

        if (!FetchSpell(id, rec)) {
            out = std::string("|cffFF0000<未知 ") + kind + ": " + U(id) + "，请确认游戏已更新到最新版本>|r";   // 中文化（By leewheel 2026-10-29）
        } else {
            std::string head = IconTag(SpellIconPath(rec), 20, 20, 0, 0) + " |cffFFFFFF" + (rec.m_name ? rec.m_name : "") + "|r";

            if (nameOnly) {
                out = head;
            } else {
                std::string desc = CoA::SpellText::Description(id, false, cached, 0);

                if (descOnly) {
                    out = desc;
                } else {
                    out = head;

                    if (range) {
                        out = out + "\n" + RangeLine(rec);
                    }

                    if (cooldown) {
                        out = out + "\n" + CooldownLine(rec);
                    }

                    if (power) {
                        out = out + "\n" + PowerLine(rec);
                    }

                    if (cast) {
                        out = out + "\n" + CastLine(rec);
                    }

                    out = out + "\n" + desc;
                }
            }
        }

        if (divider) {
            out = IconTag("Interface\\Common\\ui-tooltipdivider", 12, 180, 0, 0) + "\n" + out;
        }

        return out;
    }

    // ---- token scanning ------------------------------------------------------------------------------------------
    // FUN_101acd90: does the tag at the next '@' (from pos) open with `open`? On a match start = the opener, end = the
    // first `close` after it, true. Otherwise false, and pos moves to that '@' -- or to the end when no further '@'
    // follows.
    bool FindTag(const std::string& t, size_t& pos, size_t& start, size_t& end, const char* open, const char* close) {
        size_t len = t.size();
        size_t at = pos < len ? t.find('@', pos) : std::string::npos;

        if (at == std::string::npos) {
            pos = len;
            return false;
        }

        size_t ol = strlen(open);
        size_t cl = strlen(close);
        size_t o = len - at >= ol ? t.find(open, at) : std::string::npos;
        start = o == std::string::npos ? len : o;

        if (o == at) {
            size_t from = o + ol;
            size_t c = len - from >= cl ? t.find(close, from) : std::string::npos;
            end = c == std::string::npos ? len : c;

            if (c != std::string::npos) {
                return true;
            }

            if ((len > from ? t.find('@', from) : std::string::npos) == std::string::npos) {
                pos = len;
            }

            return false;
        }

        if ((len > at + 1 ? t.find('@', at + 1) : std::string::npos) != std::string::npos) {
            pos = at;
        } else {
            pos = len;
        }

        return false;
    }

    // FUN_101a8240 (@ext:) / FUN_101a8410 (@sim:): the body split at its first top-level '~' (nested tags of the
    // same kind are stepped over). @ext: without a '~' is all first part; @sim: then does not match.
    bool SplitTag(const std::string& t, size_t& pos, size_t& s, size_t& e, const char* open, const char* close, bool needTilde,
        std::string& first, std::string& second) {
        size_t os = 0;
        size_t oe = 0;

        if (!FindTag(t, pos, os, oe, open, close)) {
            return false;
        }

        size_t n = oe - (os + 5);
        size_t tilde = std::string::npos;
        int nest = 0;

        for (size_t i = 0; i < n;) {
            size_t avail = std::min<size_t>(5, n - i);
            const char* p = t.data() + os + 5 + i;

            if (avail == 5 && memcmp(p, open, 5) == 0) {
                i += 5;
                ++nest;
            } else if (avail == 5 && memcmp(p, close, 5) == 0) {
                i += 5;

                if (nest >= 1) {
                    --nest;
                }
            } else {
                if (*p == '~' && nest == 0 && tilde == std::string::npos) {
                    tilde = i;
                }

                ++i;
            }
        }

        if (tilde == std::string::npos) {
            if (needTilde) {
                return false;
            }

            first = t.substr(os + 5, n);
            second.clear();
        } else {
            first = t.substr(os + 5, std::min(tilde, n));
            second = t.substr(os + 5 + tilde + 1, n - (tilde + 1));
        }

        s = os;
        e = oe + 5;
        pos = s;
        return true;
    }

    // FUN_101a7e90 / FUN_101a7f00: the tag read with sscanf from its opener; on success [s, e) covers it through the
    // close delimiter and pos = s.
    template <class... A>
    bool ScanTag(const std::string& t, size_t& pos, size_t& s, size_t& e, const char* open, const char* close, const char* format,
        int want, A*... out) {
        size_t ts = 0;
        size_t te = 0;

        if (!FindTag(t, pos, ts, te, open, close)) {
            return false;
        }

        if (sscanf(t.c_str() + ts, format, out...) != want) {
            return false;
        }

        s = ts;
        e = te + strlen(close);
        pos = s;
        return true;
    }

    // FUN_101ac9b0 (after ClientIdle): markup results live 20 s, formatter output 300 s.
    void Tick() {
        for (size_t i = 0; i < s_markup.size();) {
            if (_time64(nullptr) - s_markup[i].time > 0x14) {
                s_markup.erase(s_markup.begin() + i);
            } else {
                ++i;
            }
        }

        for (size_t i = 0; i < s_formatted.size();) {
            if (_time64(nullptr) - s_formatted[i].time > 0x12C) {
                s_formatted.erase(s_formatted.begin() + i);
            } else {
                ++i;
            }
        }
    }

    void ResetCaches() {   // FUN_101a85c0
        s_markup.clear();
        s_formatted.clear();
    }

    // C_Format.Format(text [, full [, stripQuotes [, spellId]]]) -> text, {collected}   (FUN_101acb30)
    int Format(lua_State* L) {
        int t2 = lua_type(L, 2);
        int t3 = lua_type(L, 3);
        int t4 = lua_type(L, 4);
        const int types[4] = { LUA_TSTRING, t2 > 0 ? LUA_TBOOLEAN : LUA_TNIL, t3 > 0 ? LUA_TNUMBER : LUA_TNIL, t4 > 0 ? LUA_TNUMBER : LUA_TNIL };

        if (!CoA::Script::ValidateInput(L, types, 4)) {
            return 0;
        }

        std::string text = CoA::Script::CheckString(L, 1);
        bool full = t2 > 0 && lua_toboolean(L, 2) != 0;
        int stripQuotes = t3 > 0 ? CoA::Script::ToInt(CoA::Script::CheckNumber(L, 3)) : 0;
        uint32_t spell = t4 > 0 ? static_cast<uint32_t>(CoA::Script::ToInt(CoA::Script::CheckNumber(L, 4))) : 0;
        std::vector<std::string> collected;
        CoA::SpellText::Markup(text, full, false, stripQuotes, spell, &collected, true, false);
        CoA::Script::PushStr(L, text.c_str());
        lua_createtable(L, 0, static_cast<int>(collected.size()));   // FUN_100beb90
        lua_checkstack(L, 2);

        for (size_t i = 0; i < collected.size(); ++i) {
            CoA::Script::PushNum(L, static_cast<double>(i + 1));
            CoA::Script::PushStr(L, collected[i].c_str());
            lua_settable(L, -3);
        }

        return 2;
    }

    // GetSpellDescription([spellId [, full]]) (FUN_10a4dd90 -> FUN_101ac910(spell, full, 0, 0))
    int GetSpellDescription(lua_State* L) {
        uint32_t spell = lua_gettop(L) >= 1 ? static_cast<uint32_t>(CoA::Script::ToInt(CoA::Script::CheckNumber(L, 1))) : 0;
        bool full = lua_gettop(L) >= 2 && lua_toboolean(L, 2) != 0;
        CoA::Script::PushStr(L, CoA::SpellText::Description(spell, full, false, 0).c_str());
        return 1;
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("C_Format", "Format", Format),
        COA_NATIVE("", "GetSpellDescription", GetSpellDescription),
    };
}

CoA::SpellText::Deps& CoA::SpellText::Dependencies() {
    return s_deps;
}

const std::string& CoA::SpellText::Name(uint32_t spellId) {
    static std::unordered_map<uint32_t, std::string> names;
    auto it = names.find(spellId);

    if (it != names.end()) {
        return it->second;
    }

    SpellRec rec;
    std::string name;

    if (FetchSpell(spellId, rec) && rec.m_name) {
        name = rec.m_name;
    }

    return names.emplace(spellId, name).first->second;
}

const std::string& CoA::SpellText::FormText(uint32_t spellId) {
    static std::unordered_map<uint32_t, std::string> forms;
    auto it = forms.find(spellId);

    if (it != forms.end()) {
        return it->second;
    }

    SpellRec rec;
    std::string text;

    if (FetchSpell(spellId, rec)) {
        text = FormsOf(rec);
    }

    return forms.emplace(spellId, text).first->second;
}

bool CoA::SpellText::Markup(std::string& t, bool a, bool cached, int stripQuotes, uint32_t spell, std::vector<std::string>* collected,
    bool collect, bool noRequirements) {
    bool changed = false;

    if (++g_depth < 11) {
        size_t pos = 0;
        size_t s = 0;
        size_t e = 0;
        bool shift = a || KeyDown(0) || CVarOn("extendedTooltips");
        bool hint = false;

        auto eraseLines = [&t](size_t at, size_t end) {
            t.erase(at, end - at);

            while (at < t.size() && (t[at] == '\n' || t[at] == '\r')) {
                t.erase(at, 1);
            }
        };

        auto put = [&](const std::string& text) {   // collect mode moves the text out instead of inlining it
            if (!collect) {
                t.replace(s, e - s, text);
            } else {
                collected->push_back(text);
                t.erase(s, e - s);
            }
        };

        auto known = [](uint32_t id) { return ClntObjMgrGetActivePlayerObj() && SpellKnownByCA(id); };

        for (int iter = 0; iter < 100; ++iter) {
            if (t.empty() || t.size() <= pos || !FindTag(t, pos, s, e, "@", "@")) {
                break;
            }

            std::string first;
            std::string second;
            std::string label;
            std::string icon;
            uint32_t id = 0;
            int32_t flags = 0;
            char buffer[256] = {};

            if (SplitTag(t, pos, s, e, "@ext:", ":ext@", false, first, second)) {
                std::vector<std::string> sub;
                Markup(first, a, cached, stripQuotes, spell, &sub, false, false);
                Markup(second, a, cached, stripQuotes, spell, &sub, false, false);

                if (shift) {
                    t.replace(s, e - s, first);
                } else if (second.empty()) {
                    eraseLines(s, e);
                } else {
                    t.replace(s, e - s, second);
                }

                hint = true;
            } else if (SplitTag(t, pos, s, e, "@sim:", ":sim@", true, first, second)) {
                std::vector<std::string> sub;
                Markup(first, a, cached, stripQuotes, spell, &sub, false, false);
                Markup(second, a, cached, stripQuotes, spell, &sub, false, false);
                t.replace(s, e - s, CVarOn("simplifiedTooltips") ? second : first);
            } else if (ScanTag(t, pos, s, e, "@ifknown:", ":ifknown@", "@ifknown:%u:%255[^:]:ifknown@", 2, &id, buffer)) {
                if (a || (ClntObjMgrGetActivePlayerObj() && (SpellKnownByCA(id) || MysticSlotted(id)))) {
                    t.replace(s, e - s, buffer);
                } else {
                    eraseLines(s, e);
                }
            } else if (ScanTag(t, pos, s, e, "@ifnotknown:", ":ifnotknown@", "@ifnotknown:%255[^:]:%u:ifnotknown@", 2, buffer, &id)) {
                if (a || (ClntObjMgrGetActivePlayerObj() && !SpellKnownByCA(id) && !MysticSlotted(id))) {
                    t.replace(s, e - s, buffer);
                } else {
                    eraseLines(s, e);
                }
            } else if (ScanTag(t, pos, s, e, "@learns:", "@", "@learns:%u@", 1, &id)) {
                uint32_t mode = s_deps.gameMode ? s_deps.gameMode() : 0;
                bool caSpell = s_deps.isCASpell && s_deps.isCASpell(id);

                if ((((mode >> 6) & 1) && caSpell) || known(id)) {
                    eraseLines(s, e);
                } else {
                    SpellLabel(id, false, label, icon);
                    t.replace(s, e - s, Colored("FFFFFF", "解锁 " + icon + " " + label));   // 中文化（By leewheel 2026-10-29）
                }
            } else if (ScanTag(t, pos, s, e, "@nousewith:", "@", "@nousewith:%u@", 1, &id)) {
                SpellLabel(id, false, label, icon);
                t.replace(s, e - s, Colored("FF1A1A", "不能与 " + icon + " " + label + " 同时使用"));
            } else if (ScanTag(t, pos, s, e, "@re:", "@", "@re:%u:%d@", 2, &id, &flags)) {
                uint32_t f = flags < 0 ? static_cast<uint32_t>(-flags) : 0;

                if ((((f >> 2) & 1) && !shift) || (((f >> 3) & 1) && shift)) {
                    t.erase(s, e - s);
                    hint = true;
                } else {
                    put(Render("附魔", id, f & 1, (f >> 1) & 1, cached, true, (f >> 4) & 1, (f >> 5) & 1, (f >> 6) & 1, (f >> 7) & 1));   // 中文化（By leewheel 2026-10-29）
                }
            } else if (ScanTag(t, pos, s, e, "@replaces:", "@", "@replaces:%u@", 1, &id)) {
                bool k = known(id);
                SpellLabel(id, true, label, icon);
                t.replace(s, e - s, Colored(k ? "1AFF1A" : "F83600", "替换 " + icon + " " + label));
            } else if (ScanTag(t, pos, s, e, "@req:", "@", "@req:%u@", 1, &id)) {
                if (noRequirements) {
                    t.erase(s, e - s);
                } else {
                    bool k = known(id);
                    SpellLabel(id, true, label, icon);
                    t.replace(s, e - s, Colored(k ? "FFFFFF" : "FF1A1A", "需要 " + icon + " " + label));
                }
            } else if (ScanTag(t, pos, s, e, "@unlockby:", "@", "@unlockby:%u@", 1, &id)) {
                if (noRequirements) {
                    t.erase(s, e - s);
                } else {
                    bool k = known(id);
                    SpellLabel(id, true, label, icon);
                    t.replace(s, e - s, Colored(k ? "1AFF1A" : "F83600", "由 " + icon + " " + label + " 解锁"));
                }
            } else if (ScanTag(t, pos, s, e, "@usewith:", "@", "@usewith:%u@", 1, &id)) {
                SpellLabel(id, false, label, icon);
                t.replace(s, e - s, Colored("FFFFFF", "可与 " + icon + " " + label + " 搭配使用"));
            } else if (ScanTag(t, pos, s, e, "@wflocation:", "@", "@wflocation:%255[^@]@", 1, buffer)) {
                bool worldforged = s_deps.mysticWorldforged && s_deps.mysticWorldforged(spell);
                bool collectedMystic = s_deps.mysticCollected && s_deps.mysticCollected(spell);
                std::string where = Colored("FC8A00", buffer);
                std::string note = Colored("00CCFF", "铸世者秘纹附魔为灵魂绑定，需要在世界各地发现！");   // 中文化（By leewheel 2026-10-29）

                if (!a && (!worldforged || collectedMystic)) {
                    put("\n" + where);
                } else {
                    put("\n" + where + "\n" + note);
                }
            } else if (ScanTag(t, pos, s, e, "@s:", "@", "@s:%u:%d@", 2, &id, &flags)) {
                uint32_t f = flags < 0 ? static_cast<uint32_t>(-flags) : 0;

                if ((((f >> 2) & 1) && !shift) || (((f >> 3) & 1) && shift)) {
                    t.erase(s, e - s);
                    hint = true;
                } else {
                    put(Render("法术", id, f & 1, (f >> 1) & 1, cached, true, (f >> 4) & 1, (f >> 5) & 1, (f >> 6) & 1, (f >> 7) & 1));   // 中文化（By leewheel 2026-10-29）
                }
            } else if (t.find("@mountspeed@", pos) != std::string::npos) {   // FUN_10169050: anywhere after pos
                s = t.find("@mountspeed@", pos);
                pos = s;
                std::string v = "0";

                if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
                    int32_t skill = RidingSkill(player);

                    if (skill == 150 || skill == 225 || skill == 300) {
                        v = "100";
                    } else if (skill == 0) {
                        v = "40";
                    } else if (skill == 75) {
                        v = "60";
                    }
                }

                t.replace(s, 12, v);
            } else if (t.find("@flyingmountspeed@", pos) != std::string::npos) {
                s = t.find("@flyingmountspeed@", pos);
                pos = s;
                std::string v = "0";

                if (CGPlayer_C* player = ClntObjMgrGetActivePlayerObj()) {
                    int32_t skill = RidingSkill(player);

                    if (skill == 225) {
                        v = "150";
                    } else if (skill == 300) {
                        SpellRec rec;

                        if (player->IsSpellKnown(0x1667E)) {
                            v = "310";
                        } else if (spell != 0 && FetchSpell(spell, rec)) {
                            v = "280";

                            for (uint32_t i = 0; i < 3; ++i) {   // SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED
                                if (rec.m_effectAura[i] == 0xCF) {
                                    int32_t speed = rec.m_effectDieSides[i] + rec.m_effectBasePoints[i];

                                    if (speed > 0x118) {
                                        v = std::to_string(speed);
                                    }

                                    break;
                                }
                            }
                        }
                    }
                }

                t.replace(s, 18, v);
            } else {
                break;   // an '@...@' nothing recognises ends the pass
            }

            changed = true;
        }

        for (int i = 0; i < 100; ++i) {
            size_t at = t.size() >= 3 ? t.find("\n\n\n") : std::string::npos;

            if (at == std::string::npos) {
                break;
            }

            t.replace(at, 3, "\n\n");
            changed = true;
        }

        auto blank = [](char c) { return c == '\n' || c == '\r' || c == ' '; };

        while (!t.empty() && blank(t.front())) {
            t.erase(0, 1);
            changed = true;
        }

        while (!t.empty() && blank(t.back())) {
            t.pop_back();
            changed = true;
        }

        if (stripQuotes != 0 && !t.empty() && t.front() == '"' && t.back() == '"') {
            t.erase(0, 1);

            if (!t.empty()) {
                t.pop_back();
            }

            changed = true;
        }

        if (hint && !shift) {
            t += "\n\n|cff00DDFF按住 SHIFT 键获取更多信息|r";   // 中文化（By leewheel 2026-10-29）
        }
    } else {
        t = "|cffFF0000<错误: 超过最大递归深度>|r";   // 中文化（By leewheel 2026-10-29）
        changed = true;
    }

    --g_depth;
    return changed;
}

std::string CoA::SpellText::Description(uint32_t spellId, bool a, bool cached, int c) {
    SpellRec rec;
    const char* desc = FetchSpell(spellId, rec) ? rec.m_description : nullptr;

    if (!desc || !*desc) {
        return std::string();
    }

    static char buffer[0x400];   // DAT_10bdef30
    bool hit = false;

    if (cached) {
        for (const FormattedEntry& f : s_formatted) {
            if (f.spell == spellId) {
                memcpy(buffer, f.text, sizeof(buffer));
                hit = true;
                break;
            }
        }
    }

    if (!hit) {
        SpellParserParseText(&rec, buffer, sizeof(buffer), 0, 0, 0, 0, 1, 0);
        FormattedEntry f;
        f.spell = spellId;
        memcpy(f.text, buffer, sizeof(buffer));
        f.time = _time64(nullptr);
        s_formatted.push_back(f);
    }

    std::string text = buffer;

    if (cached) {
        for (const MarkupEntry& m : s_markup) {
            if (m.spell == spellId) {
                return m.text;
            }
        }
    }

    std::vector<std::string> collectedText;
    CoA::SpellText::Markup(text, a, cached, 0, spellId, &collectedText, false, c != 0);
    s_markup.push_back({ spellId, text, _time64(nullptr) });
    return text;
}

void CoA::SpellTextClient::Init() {
    CoA::Lifecycle::OnAfterIdle(Tick);
    CoA::Lifecycle::OnGlueScreen(ResetCaches);
    // FUN_10114540 x2 (storage 0x10BDEF10 / 0x10BDEF14): no help, flags 1, "0", no callback, category 3.
    COA_CVAR("extendedTooltips", nullptr, 1, "0", nullptr, 3, false, nullptr, true);
    COA_CVAR("simplifiedTooltips", nullptr, 1, "0", nullptr, 3, false, nullptr, true);
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
}
