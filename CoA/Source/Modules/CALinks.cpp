// AscCAMgr.cpp's build-link bindings: ExportBuild, ImportPendingBuild (a CoA build string or a Build Creator link) and
// ImportPendingBuildID, with the URL / link parsing they share. The Build Creator itself (export, the compressed link
// format, build seeds) answers through Deps until it is ported.
#include "CoA/Source/Modules/CANativesInternal.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <algorithm>
#include <sstream>

namespace {
#include "CoA/Source/Modules/CAEnums.generated.inc"
}

using namespace CoA::CA;
using namespace CoA::CA::detail;
using CoA::Script::PushBool;
using CoA::Script::PushInt;
using CoA::Script::PushNils;
using CoA::Script::PushStr;
using CoA::Script::ReadBool;
using CoA::Script::ReadString;

namespace {
    std::string LearnName(uint32_t r) {
        return r < 0x2E ? kLearnResults[r] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
    }

    std::string Format(const char* fmt, const std::vector<std::string>& args) {   // FUN_1009e480 "{}"
        std::string out;
        size_t next = 0;

        for (const char* p = fmt; *p; ++p) {
            if (p[0] == '{' && p[1] == '}') {
                out += next < args.size() ? args[next++] : std::string();
                ++p;
            } else {
                out += *p;
            }
        }

        return out;
    }

    std::string TrimWs(const std::string& s) {   // FUN_101805a0 (" \t\r\n")
        const size_t a = s.find_first_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }

    // AscBuildCreator's UrlDecode: %XX with two characters after it (a trailing "%XX" stays as it is).
    std::string UrlDecode(const std::string& in) {
        std::string out;

        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                std::istringstream is(in.substr(i + 1, 2));
                int v = 0;
                is >> std::hex >> v;
                out += static_cast<char>(v);
                i += 2;
            } else {
                out += in[i];
            }
        }

        return out;
    }

    // FUN_1016f050: the value of "<key>=" at the start, or after '#', '?' or '&', up to "& \t\r\n".
    std::string ParamValue(const std::string& s, const std::string& key) {
        const std::string k = key + "=";
        size_t at = std::string::npos;

        if (s.compare(0, k.size(), k) == 0) {
            at = 0;
        } else {
            for (char sep : { '#', '?', '&' }) {
                const size_t f = s.find(std::string(1, sep) + k);

                if (f != std::string::npos) {
                    at = f + 1;
                    break;
                }
            }
        }

        if (at == std::string::npos) {
            return std::string();
        }

        const size_t from = at + k.size();
        const size_t end = s.find_first_of("& \t\r\n", from);
        return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
    }

    // FUN_1016f880: what ImportPendingBuild does to the pasted text first.
    std::string NormaliseBuildUrl(const std::string& in) {
        std::string s = TrimWs(in);

        if (s.empty()) {
            return s;
        }

        if (s.compare(0, 8, "talents=") == 0) {
            const size_t end = s.find_first_of("& \t\r\n", 8);
            return s.substr(8, end == std::string::npos ? std::string::npos : end - 8);
        }

        for (const char* key : { "#talents=", "?talents=", "&talents=" }) {
            const size_t f = s.find(key);

            if (f != std::string::npos) {
                const size_t from = f + 9;
                const size_t end = s.find_first_of("& \t\r\n", from);
                return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
            }
        }

        const size_t cut = s.find_first_of("?#");

        if (cut != std::string::npos) {
            s = s.substr(0, cut);
        }

        while (!s.empty() && s.back() == '/') {
            s.pop_back();
        }

        const size_t ov = s.size() >= 10 ? s.find("/overview/") : std::string::npos;

        if (ov != std::string::npos) {
            return s.substr(ov + 10);
        }

        if (s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0) {
            const size_t slash = s.rfind('/');

            if (slash != std::string::npos) {
                return s.substr(slash + 1);
            }
        }

        return s;
    }

    // FUN_10182430: the whole string as a decimal u32 (std::stoul, which must consume every character).
    bool ParseU32(const std::string& s, uint32_t& out) {
        if (s.empty()) {
            return false;
        }

        try {
            size_t used = 0;
            const unsigned long v = std::stoul(s, &used, 10);

            if (used != s.size()) {
                return false;
            }

            out = static_cast<uint32_t>(v);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // FUN_101817c0: a plain CoA build string (after the talents= parameter and URL decoding, '.' as ':'): every token
    // N, NtN or NeN and at least one talent token. Normalised to ":tok:...:".
    bool PlainCoALink(const std::string& url, std::string& out) {
        std::string s = TrimWs(url);

        if (s.empty()) {
            return false;
        }

        const std::string v = ParamValue(s, "talents");

        if (!v.empty()) {
            s = v;
        }

        s = UrlDecode(s);
        std::replace(s.begin(), s.end(), '.', ':');

        while (!s.empty() && s.front() == ':') {
            s.erase(0, 1);
        }

        while (!s.empty() && s.back() == ':') {
            s.pop_back();
        }

        if (s.empty()) {
            return false;
        }

        std::istringstream is(s);
        std::string tok;
        std::string text;
        uint32_t count = 0;
        uint32_t a = 0;
        uint32_t b = 0;
        bool talent = false;

        while (std::getline(is, tok, ':')) {
            tok = TrimWs(tok);

            if (tok.empty()) {
                continue;
            }

            const size_t t = tok.find('t');
            const size_t e = tok.find('e');

            if (t == std::string::npos && e == std::string::npos) {
                for (char c : tok) {
                    if (c < '0' || c > '9') {
                        return false;
                    }
                }
            } else if (t == std::string::npos) {
                if (!ParseU32(tok.substr(0, e), a) || !ParseU32(tok.substr(e + 1), b)) {
                    return false;
                }
            } else if (e == std::string::npos) {
                if (!ParseU32(tok.substr(0, t), a) || !ParseU32(tok.substr(t + 1), b)) {
                    return false;
                }

                talent = true;
            } else {
                return false;
            }

            text += ':' + tok;   // FUN_100a4fe0 + FUN_10167240
            ++count;
        }

        if (!count || !talent) {
            return false;
        }

        out = text + ':';
        return true;
    }

    std::string PreviewOf(const std::string& s) {   // FUN_1016ef40
        return s.size() < 0x61 ? s : s.substr(0, 0x60) + "...";
    }

    // 中文化: 导入失败的提示会显示给玩家（By leewheel 2026-10-29）
    std::string NotYourClass(Row r, uint8_t cls) {   // FUN_1016f780
        if (r) {
            if (const uint8_t* ct = ClassTypes().Row(RowU32(r, 0x80))) {
                if (const uint32_t owner = RowU32(ct, 8)) {
                    return Format("属于 CoA 职业 {}，但你当前的职业是 {}", { std::to_string(owner), std::to_string(cls) });
                }
            }
        }

        return Format("不属于你当前的 CoA 职业 {}", { std::to_string(cls) });
    }

    // FUN_1016de70: a new id is appended; a repeated rank-1 token adds a rank (capped at the row's max), a repeated
    // higher rank keeps the larger.
    void MergeEntry(std::vector<Entry>& v, uint32_t id, uint32_t rank) {
        for (Entry& e : v) {
            if (e.id == id) {
                if (rank != 1) {
                    e.rank = std::max(e.rank, rank);
                } else if (Row r = FindRow(id)) {
                    e.rank = std::min(e.rank + 1, MaxRank(r));
                } else {
                    e.rank = e.rank + 1 == 0 ? 0xFFFFFFFF : e.rank + 1;
                }

                return;
            }
        }

        Entry e;
        e.row = FindRow(id);
        e.id = id;
        e.rank = rank;
        v.push_back(e);
    }

    bool RowHasSpell(Row r, uint32_t spell) {   // FUN_101c6540: the nine rank spells at +0x14
        for (uint32_t i = 0; i < 9; ++i) {
            if (RowU32(r, 0x14 + i * 4) == spell) {
                return true;
            }
        }

        return false;
    }

    // The first realm-visible Ability row listing the spell, optionally only of the class.
    Row AbilityOf(uint32_t spell, const uint8_t* cls) {
        CoA::Dbc::Table& rows = Rows();

        for (uint32_t id = rows.MinId(); rows.Loaded() && id <= rows.MaxId(); ++id) {
            if (Row r = FindRow(id)) {
                if (RowVisible(r) && (!cls || ClassTypeAdmits(r, *cls)) && RowType(r) == 1 && RowHasSpell(r, spell)) {
                    return r;
                }
            }
        }

        return nullptr;
    }

    // FUN_101806a0: the entries of a CoA build link for class `cls`, then the pending build's implicit entries (flags
    // 4 / 2 / 0x400000) the link lacks. False with the reason in `err`.
    bool ParseCoALink(const std::string& url, uint8_t cls, const Build* pending, std::vector<Entry>& entries, std::string& err) {
        std::string text;

        if (!PlainCoALink(url, text)) {
            const Deps& d = Dependencies();
            text = d.decodeLink ? d.decodeLink(url) : std::string();
        }

        if (text.empty()) {
            err = Format("无法解码 CoA 构建链接。输入长度: {}。输入预览: {}", { std::to_string(url.size()), PreviewOf(url) });
            return false;
        }

        std::istringstream is(text);
        std::string tok;
        uint32_t done = 0;

        while (std::getline(is, tok, ':')) {
            if (tok.empty()) {
                continue;
            }

            const std::string n = std::to_string(done + 1);
            const size_t t = tok.find('t');
            const size_t e = tok.find('e');

            if (t != std::string::npos) {
                uint32_t id = 0;
                uint32_t rank = 0;

                if (!ParseU32(tok.substr(0, t), id) || !ParseU32(tok.substr(t + 1), rank)) {
                    err = Format("无法解析 CoA 天赋标记: {}", { tok });
                    return false;
                }

                const Row r = FindRow(id);

                if (!r) {
                    err = Format("CoA 构建包含未知条目 {}。失败前已解析标记数: {}。内容预览: {}",
                        { std::to_string(id), n, PreviewOf(text) });
                    return false;
                }

                if (!RowVisible(r) || !ClassTypeAdmits(r, cls)) {
                    if (RowVisible(r)) {
                        err = Format("CoA 构建包含条目 {}（{}）。失败前已解析标记数: {}。", { std::to_string(id), NotYourClass(r, cls), n });
                    } else {
                        err = Format("CoA 构建包含本领域不可用的条目: {}。失败前已解析标记数: {}。", { std::to_string(id), n });
                    }

                    return false;
                }

                const int type = RowType(r);

                if (type < 1 || type > 4) {
                    err = Format("CoA 构建包含非进阶条目标记: {}。失败前已解析标记数: {}。", { std::to_string(id), n });
                    return false;
                }

                const uint32_t take = std::min(MaxRank(r), rank);

                if (!take) {
                    err = Format("CoA 构建中条目 {} 的等级无效。", { std::to_string(id) });
                    return false;
                }

                MergeEntry(entries, id, take);
            } else if (e == std::string::npos) {
                uint32_t spell = 0;

                if (!ParseU32(tok, spell)) {
                    err = Format("无法解析 CoA 法术标记: {}", { tok });
                    return false;
                }

                if (Row found = AbilityOf(spell, &cls)) {
                    MergeEntry(entries, RowU32(found, 0), 1);
                } else {
                    if (Row other = AbilityOf(spell, nullptr)) {
                        err = Format("CoA 构建包含技能法术 {}（条目 {}，{}）。",
                            { std::to_string(spell), std::to_string(RowU32(other, 0)), NotYourClass(other, cls) });
                    } else {
                        err = Format("CoA 构建包含该职业未知的技能法术: {}。失败前已解析标记数: {}。",
                            { std::to_string(spell), n });
                    }

                    return false;
                }
            }

            ++done;   // enchant tokens (NeN) are counted and skipped
        }

        if (pending) {
            for (const Entry& pe : pending->entries) {
                const Row r = pe.row ? pe.row : FindRow(pe.id);

                if (!r || !(RowHasFlag(r, 4) || RowHasFlag(r, 2) || RowHasFlag(r, 0x400000)) || !ClassTypeAdmits(r, cls)) {
                    continue;
                }

                bool have = false;

                for (const Entry& x : entries) {
                    have = have || x.id == pe.id;
                }

                if (!have) {
                    entries.push_back(pe);   // thunk_FUN_1014b750
                }
            }
        }

        if (entries.empty()) {
            err = "没有找到可导入的 CoA 标记。请确认该链接对应你当前的 CoA 职业，且此客户端版本包含这些进阶条目。";   // 中文化（By leewheel 2026-10-29）
            return false;
        }

        return true;
    }

    // FUN_10176220: the active build as a build-creator link (FUN_100fc240 + FUN_100fd8f0).
    int ExportBuild(lua_State* L) {
        bool includeREs;

        if (!ReadBool(L, includeREs)) {
            return Usage(L, "Usage: C_CharacterAdvancement.ExportBuild(includeREs)");
        }

        const Deps& d = Dependencies();
        PushStr(L, d.exportBuild ? d.exportBuild(*ActiveBuild(), includeREs).c_str() : "");
        return 1;
    }

    int PushValidateFailure(lua_State* L, uint32_t result, uint32_t id, uint32_t rank) {
        PushBool(L, false);
        PushStr(L, LearnName(result).c_str());   // FUN_10169de0
        PushInt(L, static_cast<int32_t>(id));
        PushInt(L, static_cast<int32_t>(rank));
        return 4;
    }

    // FUN_1017b910: a build link into the pending build. A CoA class reads the CoA link format; any other class a Build
    // Creator link, built at the player's level. The candidate is validated in full on a copy of the pending build
    // first. (false, reason | CA_LEARN_*, id, rank) on failure.
    int ImportPendingBuild(lua_State* L) {
        std::string url;

        if (!ReadString(L, url)) {
            return Usage(L, "Usage: C_CharacterAdvancement.ImportPendingBuild(buildURL)");
        }

        url = NormaliseBuildUrl(url);
        Build* pending = M().pending;

        if (!pending) {
            return Usage(L, "ImportPendingBuild: pending build is not available");
        }

        const uint64_t player = ActivePlayerGuid();

        if (!UnitExists(player)) {
            return Usage(L, "ImportPendingBuild: player not available");
        }

        const uint8_t cls = UnitClass(player);
        Build copy(*pending, true);

        if (CoAClass(cls)) {
            std::vector<Entry> parsed;
            std::string err;

            if (!ParseCoALink(url, cls, pending, parsed, err)) {
                PushBool(L, false);
                PushStr(L, err.c_str());
                PushNils(L, 2);
                return 4;
            }

            copy.entries = parsed;
            copy.Reorder(false);
        } else {
            const Deps& d = Dependencies();
            Build b(d.seedFromLink ? d.seedFromLink(url) : BuildSeed(), UnitLevel(player), false);
            copy.entries = b.entries;
        }

        uint32_t eid = 0;
        uint32_t rank = 0;
        uint32_t result = 0;

        if (!copy.ValidateAll(true, {}, {}, false, eid, rank, result)) {
            return PushValidateFailure(L, result, eid, rank);
        }

        pending->entries = copy.entries;
        pending->Reorder(false);
        BumpPendingVersion();
        PushBool(L, true);
        PushNils(L, 3);
        return 4;
    }

    // FUN_1017c060: the Build Creator build with this id, built at the player's level and validated in full; on
    // success the pending build takes its entries. (false, CA_LEARN_*, id, rank) otherwise.
    int ImportPendingBuildID(lua_State* L) {
        std::string id;

        if (!ReadString(L, id)) {
            return Usage(L, "Usage: C_CharacterAdvancement.ImportPendingBuildID(buildID)");
        }

        Build* pending = M().pending;

        if (!pending) {
            return Usage(L, "ImportPendingBuildID: pending build is not available");
        }

        const uint64_t player = ActivePlayerGuid();

        if (!UnitExists(player)) {
            return Usage(L, "ImportPendingBuildID: player not available");
        }

        BuildSeed seed;
        const Deps& d = Dependencies();

        if (!d.seedOfBuild || !d.seedOfBuild(id, seed)) {
            return Usage(L, "ImportPendingBuildID: invalid buildID");
        }

        Build b(seed, UnitLevel(player), true);
        uint32_t eid = 0;
        uint32_t rank = 0;
        uint32_t result = 0;

        if (!b.ValidateAll(true, {}, {}, false, eid, rank, result)) {
            return PushValidateFailure(L, result, eid, rank);
        }

        pending->entries = b.entries;   // FUN_10158010
        pending->Reorder(false);
        BumpPendingVersion();
        PushBool(L, true);
        PushNils(L, 3);
        return 4;
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("C_CharacterAdvancement", "ImportPendingBuildID", ImportPendingBuildID),
        COA_NATIVE("C_CharacterAdvancement", "ImportPendingBuild", ImportPendingBuild),
        COA_NATIVE("C_CharacterAdvancement", "ExportBuild", ExportBuild),
    };
}

void CoA::CA::detail::InitLinks() {
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
}
