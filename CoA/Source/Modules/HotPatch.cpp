#include "CoA/Source/Modules/HotPatch.h"
#include "CoA/Source/Modules/Modules.h"
#include "CoA/Source/Runtime/ClientHooks.h"
#include "CoA/Source/Runtime/DbcPatch.h"
#include "CoA/Source/Runtime/Lifecycle.h"
#include "CoA/Source/Runtime/Log.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Packet.h"
#include "CoA/Source/Runtime/Packets.h"
#include "CoA/Source/Runtime/Read.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <utility>
#include <vector>

// Port of the reconstruction's AscHotPatch.cpp (see HotPatch.h). The reconstruction reaches the client through fixed
// addresses (luaL_loadbuffer 0x84F860, lua_pcall 0x84EC50, the taint source 0xD4139C, FrameScript_Execute 0x819210);
// here they are the decomp's own lua_* functions, its lua_tainted global and FrameScript_Execute (HotPatchClient.cpp).
namespace {
    using CoA::HotPatch::Outcome;

    constexpr uint32_t kProtocolVersion = 1;
    constexpr size_t kMaxPatch = 32 * 1024;
    constexpr size_t kMaxSession = 128 * 1024;
    constexpr const char* kSettingsFile = "WTF\\HotPatch.txt";

    const char* const kOutcomeNames[] = {
        "ran", "applied", "deferred", "skipped", "duplicate", "refused-by-player", "refused-guard", "error"
    };

    enum Mode : uint8_t { Off, Ask, Allow };

    struct Patch {
        std::string id;
        std::string target;
        std::string source;
        uint64_t hash = 0;
    };

    struct Record {
        std::string id;
        std::string target;
        std::string detail;
        size_t size = 0;
        uint64_t hash = 0;
        Outcome outcome = CoA::HotPatch::Ran;
    };

    CoA::HotPatch::Deps s_deps;
    std::vector<Record> s_records;
    std::deque<Patch> s_awaitingConsent;
    std::vector<Patch> s_awaitingAddon;
    std::vector<Patch> s_received;   // this connection's patches, latest per id, replayed after a /reload
    size_t s_sessionBytes = 0;
    bool s_promptShown = false;

    std::string HexHash(uint64_t h) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    struct Settings {
        Mode mode = Allow;
        std::vector<std::pair<std::string, bool>> realms;
    };

    Settings LoadSettings() {
        Settings s;
        FILE* f = fopen(kSettingsFile, "r");

        if (!f) {
            return s;
        }

        char line[512];

        while (fgets(line, sizeof(line), f)) {
            std::string l(line);

            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
                l.pop_back();
            }

            if (l.rfind("mode=", 0) == 0) {
                const int m = atoi(l.c_str() + 5);
                s.mode = m <= 0 ? Off : m >= 2 ? Allow : Ask;
            } else if (l.rfind("realm=", 0) == 0) {
                const size_t eq = l.rfind('=');

                if (eq > 6) {
                    s.realms.emplace_back(l.substr(6, eq - 6), l.substr(eq + 1) == "1");
                }
            }
        }

        fclose(f);
        return s;
    }

    void SaveSettings(const Settings& s) {
        FILE* f = fopen(kSettingsFile, "w");

        if (!f) {
            return;
        }

        fprintf(f, "mode=%d\n", static_cast<int>(s.mode));

        for (const auto& [realm, allowed] : s.realms) {
            fprintf(f, "realm=%s=%d\n", realm.c_str(), allowed ? 1 : 0);
        }

        fclose(f);
    }

    lua_State* State() {
        return s_deps.state ? s_deps.state() : nullptr;
    }

    void Execute(const char* source) {
        if (s_deps.execute) {
            s_deps.execute(source, "AscHotPatch");
        }
    }

    // Taint sources are kept by pointer in tainted values, so the names live for the whole process.
    const char* InternName(const std::string& name) {
        static std::set<std::string> names;
        return names.insert(name).first->c_str();
    }

    std::string CallString(const char* fn, const char* arg = nullptr) {
        lua_State* L = State();

        if (!L) {
            return std::string();
        }

        const int top = lua_gettop(L);
        std::string out;
        lua_getfield(L, LUA_GLOBALSINDEX, fn);

        if (lua_type(L, -1) == LUA_TFUNCTION) {
            if (arg) {
                lua_pushstring(L, arg);
            }

            if (lua_pcall(L, arg ? 1 : 0, 1, 0) == 0) {
                if (lua_type(L, -1) == LUA_TSTRING) {
                    out = lua_tostring(L, -1);
                } else if (lua_toboolean(L, -1)) {
                    out = "1";
                }
            }
        }

        lua_settop(L, top);
        return out;
    }

    std::string RealmName() {
        return CallString("GetRealmName");
    }

    bool AddonLoaded(const std::string& name) {
        return !CallString("IsAddOnLoaded", name.c_str()).empty();
    }

    // AscHotPatchApplied[id] = hash, a Lua global so it lasts exactly one Lua session.
    bool AlreadyRan(const Patch& p) {
        lua_State* L = State();

        if (!L) {
            return false;
        }

        const int top = lua_gettop(L);
        bool ran = false;
        lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");

        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_getfield(L, -1, p.id.c_str());
            ran = lua_type(L, -1) == LUA_TSTRING && HexHash(p.hash) == lua_tostring(L, -1);
        }

        lua_settop(L, top);
        return ran;
    }

    void MarkRan(const Patch& p) {
        lua_State* L = State();

        if (!L) {
            return;
        }

        const int top = lua_gettop(L);
        lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");

        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_settop(L, top);
            lua_createtable(L, 0, 4);
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");
        }

        lua_pushstring(L, HexHash(p.hash).c_str());
        lua_setfield(L, -2, p.id.c_str());
        lua_settop(L, top);
    }

    void PutSized(CoA::Packet& packet, const std::string& s) {
        packet.U32(static_cast<uint32_t>(s.size()));

        if (!s.empty()) {
            packet.Data(s.data(), static_cast<uint32_t>(s.size()));
        }
    }

    void Report(const Patch& p, Outcome outcome, std::string detail) {
        if (detail.size() > 255) {
            detail.resize(255);
        }

        CoA::Log::Printf("hot patch %s target=%s size=%u hash=%s: %s%s%s", p.id.c_str(),
            p.target.empty() ? "-" : p.target.c_str(), static_cast<unsigned>(p.source.size()), HexHash(p.hash).c_str(),
            kOutcomeNames[outcome], detail.empty() ? "" : " ", detail.c_str());
        s_records.push_back(Record { p.id, p.target, detail, p.source.size(), p.hash, outcome });
        CoA::Packet packet(0x8F2);
        PutSized(packet, p.id);
        packet.U8(outcome);
        PutSized(packet, detail);
        packet.Send();
    }

    void Run(const Patch& p) {
        if (AlreadyRan(p)) {
            Report(p, CoA::HotPatch::Duplicate, "");
            return;
        }

        lua_State* L = State();

        if (!L) {
            Report(p, CoA::HotPatch::Error, "no Lua state");
            return;
        }

        const int top = lua_gettop(L);
        // FrameScript_Execute (0x819210) keeps the running addon -- the taint source -- in lua_tainted (0xD4139C), set
        // around the call and restored after. A patch does the same.
        const char* saved = lua_tainted;
        lua_tainted = p.target.empty() ? nullptr : InternName(p.target);
        const std::string chunk = "=AscHotPatch:" + p.id;
        Outcome outcome = CoA::HotPatch::Ran;
        std::string detail;

        if (luaL_loadbuffer(L, p.source.data(), p.source.size(), chunk.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
            outcome = CoA::HotPatch::Error;

            if (const char* e = lua_tostring(L, -1)) {
                detail = e;
            }
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string r = lua_tostring(L, -1);

            if (r == "applied") {
                outcome = CoA::HotPatch::Applied;
            } else if (r.rfind("skipped", 0) == 0) {
                outcome = CoA::HotPatch::Skipped;
                detail = r.size() > 9 && r[7] == ':' ? r.substr(9) : r.substr(7);
            } else {
                detail = r;
            }
        }

        lua_tainted = saved;
        lua_settop(L, top);
        s_sessionBytes += p.source.size();
        MarkRan(p);
        Report(p, outcome, detail);
    }

    void RunOrDefer(const Patch& p) {
        if (!p.target.empty() && !AddonLoaded(p.target)) {
            s_awaitingAddon.push_back(p);
            Report(p, CoA::HotPatch::Deferred, "waiting for " + p.target);
            return;
        }

        Run(p);
    }

    // 中文化: 玩家可见的授权对话框与聊天提示（By leewheel 2026-10-29）
    const char* const kConsentLua = R"lua(
if not StaticPopupDialogs.ASC_HOT_PATCH_CONSENT then
    StaticPopupDialogs.ASC_HOT_PATCH_CONSENT = {
        text = "%s 想要用它发送的代码修改你的界面（%s）。\n\n允许来自该领域的界面补丁吗？之后可用 /hotpatch 修改此设置。",
        button1 = "允许",
        button2 = "拒绝",
        OnAccept = function() C_HotPatch.SetRealmConsent(true) end,
        OnCancel = function(_, _, reason) if reason ~= "override" then C_HotPatch.SetRealmConsent(false) end end,
        timeout = 0,
        whileDead = true,
        hideOnEscape = false,
        preferredIndex = 3,
    }
end
)lua";

    void Prompt() {
        if (s_promptShown) {
            return;
        }

        s_promptShown = true;
        std::string ids;

        for (const Patch& p : s_awaitingConsent) {
            if (ids.find(p.id) == std::string::npos) {
                ids += (ids.empty() ? "" : ", ") + p.id;
            }
        }

        Execute(kConsentLua);
        lua_State* L = State();

        if (!L) {
            return;
        }

        const int top = lua_gettop(L);
        lua_getfield(L, LUA_GLOBALSINDEX, "StaticPopup_Show");

        if (lua_type(L, -1) == LUA_TFUNCTION) {
            lua_pushstring(L, "ASC_HOT_PATCH_CONSENT");
            lua_pushstring(L, RealmName().c_str());
            lua_pushstring(L, ids.c_str());
            lua_pcall(L, 3, 0, 0);
        }

        lua_settop(L, top);
    }

    // The latest copy of each patch id, within the session limit, so a new Lua state can run them again.
    void Remember(const Patch& p) {
        size_t bytes = p.source.size();
        Patch* same = nullptr;

        for (Patch& r : s_received) {
            if (r.id == p.id) {
                same = &r;
            } else {
                bytes += r.source.size();
            }
        }

        if (bytes > kMaxSession) {
            return;
        }

        if (same) {
            *same = p;
        } else {
            s_received.push_back(p);
        }
    }

    void Handle(Patch p) {
        std::string reason;

        if (!CoA::HotPatch::Guard(p.id, p.source, s_sessionBytes, reason)) {
            Report(p, CoA::HotPatch::RefusedGuard, reason);
            return;
        }

        Remember(p);
        const Settings s = LoadSettings();

        if (s.mode == Off) {
            Report(p, CoA::HotPatch::RefusedByPlayer, "hot patches are off");
            return;
        }

        if (s.mode == Ask) {
            const std::string realm = RealmName();

            for (const auto& [name, allowed] : s.realms) {
                if (name == realm) {
                    if (!allowed) {
                        Report(p, CoA::HotPatch::RefusedByPlayer, "denied for this realm");
                        return;
                    }

                    RunOrDefer(p);
                    return;
                }
            }

            s_awaitingConsent.push_back(std::move(p));
            Prompt();
            return;
        }

        RunOrDefer(p);
    }

    int32_t OnHotPatch(void*, uint32_t, uint32_t, CDataStore* msg) {
        Patch p;
        p.id = CoA::Read::Name(msg);
        p.target = CoA::Read::Name(msg);
        p.source = CoA::Read::Name(msg);

        if (CoA::Read::Failed(msg)) {
            CoA::Log::Printf("hot patch: malformed SMSG 0x8F0");
            return 1;
        }

        p.hash = CoA::HotPatch::Hash(p.source);
        Handle(std::move(p));
        return 1;
    }

    void SendCapability() {
        CoA::Packet packet(0x8F1);
        packet.U32(kProtocolVersion);
        packet.U8(LoadSettings().mode);
        packet.Send();
    }

    int SetRealmConsent(lua_State* L) {
        const bool allowed = lua_toboolean(L, 1) != 0;
        Settings s = LoadSettings();
        const std::string realm = RealmName();
        bool found = false;

        for (auto& [name, value] : s.realms) {
            if (name == realm) {
                value = allowed;
                found = true;
            }
        }

        if (!found) {
            s.realms.emplace_back(realm, allowed);
        }

        SaveSettings(s);
        s_promptShown = false;
        std::deque<Patch> waiting;
        waiting.swap(s_awaitingConsent);

        for (const Patch& p : waiting) {
            if (allowed) {
                RunOrDefer(p);
            } else {
                Report(p, CoA::HotPatch::RefusedByPlayer, "denied for this realm");
            }
        }

        return 0;
    }

    int OnAddonLoaded(lua_State* L) {
        const std::string addon = CoA::Script::CheckString(L, 1);
        std::vector<Patch> waiting;

        for (auto it = s_awaitingAddon.begin(); it != s_awaitingAddon.end();) {
            if (it->target == addon) {
                waiting.push_back(*it);
                it = s_awaitingAddon.erase(it);
            } else {
                ++it;
            }
        }

        for (const Patch& p : waiting) {
            Run(p);
        }

        return 0;
    }

    int GetPatches(lua_State* L) {
        lua_createtable(L, static_cast<int>(s_records.size()), 0);
        int i = 0;

        for (const Record& r : s_records) {
            lua_createtable(L, 0, 6);
            lua_pushstring(L, r.id.c_str());
            lua_setfield(L, -2, "id");
            lua_pushstring(L, r.target.c_str());
            lua_setfield(L, -2, "target");
            lua_pushinteger(L, static_cast<int>(r.size));
            lua_setfield(L, -2, "size");
            lua_pushstring(L, HexHash(r.hash).c_str());
            lua_setfield(L, -2, "hash");
            lua_pushstring(L, kOutcomeNames[r.outcome]);
            lua_setfield(L, -2, "outcome");
            lua_pushstring(L, r.detail.c_str());
            lua_setfield(L, -2, "detail");
            lua_rawseti(L, -2, ++i);
        }

        return 1;
    }

    int GetMode(lua_State* L) {
        static const char* const names[] = { "off", "ask", "on" };
        lua_pushstring(L, names[LoadSettings().mode]);
        return 1;
    }

    int SetMode(lua_State* L) {
        const std::string m = CoA::Script::CheckString(L, 1);
        Settings s = LoadSettings();
        s.mode = m == "off" ? Off : m == "on" ? Allow : Ask;
        SaveSettings(s);
        SendCapability();
        return 0;
    }

    const char* const kWorldLua = R"lua(
if not AscHotPatchFrame then
    AscHotPatchFrame = CreateFrame("Frame")
    AscHotPatchFrame:RegisterEvent("ADDON_LOADED")
    AscHotPatchFrame:SetScript("OnEvent", function(_, _, addon) C_HotPatch.OnAddonLoaded(addon) end)
end
SLASH_ASCHOTPATCH1 = "/hotpatch"
SlashCmdList.ASCHOTPATCH = function(msg)
    msg = strlower(strtrim(msg or ""))
    if msg == "on" or msg == "off" or msg == "ask" then
        C_HotPatch.SetMode(msg)
    end
    local patches = C_HotPatch.GetPatches()
    DEFAULT_CHAT_FRAME:AddMessage(format("界面补丁: %s（/hotpatch on|ask|off）。本次会话共 %d 个。", C_HotPatch.GetMode(), #patches))
    for _, p in ipairs(patches) do
        DEFAULT_CHAT_FRAME:AddMessage(format("  %s%s: %s%s（%d 字节，%s）", p.id, p.target ~= "" and (" -> " .. p.target) or "",
            p.outcome, p.detail ~= "" and (" - " .. p.detail) or "", p.size, p.hash))
    end
end
)lua";

    // AscHotPatchFrame is created by kWorldLua; a state without it is a new Lua session (login or /reload).
    bool NewLuaSession() {
        lua_State* L = State();

        if (!L) {
            return false;
        }

        const int top = lua_gettop(L);
        lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchFrame");
        const bool fresh = lua_type(L, -1) == LUA_TNIL;
        lua_settop(L, top);
        return fresh;
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("C_HotPatch", "SetRealmConsent", SetRealmConsent),
        COA_NATIVE("C_HotPatch", "OnAddonLoaded", OnAddonLoaded),
        COA_NATIVE("C_HotPatch", "GetPatches", GetPatches),
        COA_NATIVE("C_HotPatch", "GetMode", GetMode),
        COA_NATIVE("C_HotPatch", "SetMode", SetMode),
    };
}

CoA::HotPatch::Deps& CoA::HotPatch::Dependencies() {
    return s_deps;
}

uint64_t CoA::HotPatch::Hash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;

    for (unsigned char c : s) {
        h = (h ^ c) * 1099511628211ull;
    }

    return h;
}

bool CoA::HotPatch::Guard(const std::string& id, const std::string& source, size_t sessionBytes, std::string& reason) {
    reason.clear();

    if (id.empty() || source.empty()) {
        reason = "empty patch id or source";
    } else if (source.size() > kMaxPatch) {
        reason = "larger than 32 KB";
    } else if (sessionBytes + source.size() > kMaxSession) {
        reason = "over the 128 KB session limit";
    } else if (source.find('\x1b') != std::string::npos) {
        reason = "contains an ESC byte (bytecode)";
    } else if (source.find("loadstring") != std::string::npos) {
        reason = "calls loadstring";
    } else if (source.find("string.dump") != std::string::npos) {
        reason = "calls string.dump";
    }

    return reason.empty();
}

bool CoA::HotPatch::FormatterAllowed(const std::string& text, std::string& reason) {
    static const std::set<std::string> kCalls = {
        "C_Quest.GetTitleByID", "SecondsToTime", "GetCreatureNameByID", "GetMoneyString",
        "C_Item.GetColoredItemName", "GetColoredReputationName", "GetFactionInfoByID",
    };
    static const std::set<std::string> kWords = { "value", "and", "or", "not", "nil", "true", "false" };
    size_t i = 0;
    const size_t n = text.size();
    auto skipSpace = [&] {
        while (i < n && isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
    };
    skipSpace();

    if (text.compare(i, 6, "return") != 0 || (i + 6 < n && (isalnum(static_cast<unsigned char>(text[i + 6])) || text[i + 6] == '_'))) {
        reason = "does not start with return";
        return i == n;
    }

    i += 6;
    int depth = 0;
    bool any = false;

    while (true) {
        skipSpace();

        if (i >= n) {
            break;
        }

        const char c = text[i];
        any = true;

        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string name;

            while (true) {
                const size_t start = i;

                while (i < n && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
                    ++i;
                }

                name.append(text, start, i - start);

                if (i + 1 < n && text[i] == '.' && text[i + 1] != '.' &&
                    (isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_')) {
                    name += '.';
                    ++i;
                    continue;
                }

                break;
            }

            if (!kCalls.count(name) && !kWords.count(name)) {
                reason = "uses " + name;
                return false;
            }
        } else if (isdigit(static_cast<unsigned char>(c))) {
            while (i < n && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
                ++i;
            }
        } else if (c == '"' || c == '\'') {
            ++i;

            while (i < n && text[i] != c) {
                if (text[i] == '\n') {
                    break;
                }

                i += text[i] == '\\' ? 2 : 1;
            }

            if (i >= n || text[i] != c) {
                reason = "unterminated string";
                return false;
            }

            ++i;
        } else if (c == '(') {
            ++depth;
            ++i;
        } else if (c == ')') {
            if (--depth < 0) {
                reason = "unbalanced )";
                return false;
            }

            ++i;
        } else if (c == '-' && i + 1 < n && text[i + 1] == '-') {
            reason = "contains a comment";
            return false;
        } else if (strchr("+-*/%^#,", c)) {
            ++i;
        } else if (c == '.' && i + 1 < n && text[i + 1] == '.') {
            i += 2;
        } else if ((c == '=' || c == '~' || c == '<' || c == '>') && i + 1 < n && text[i + 1] == '=') {
            i += 2;
        } else if (c == '<' || c == '>') {
            ++i;
        } else {
            reason = std::string("uses '") + c + "'";
            return false;
        }
    }

    if (depth != 0) {
        reason = "unbalanced (";
        return false;
    }

    if (!any) {
        reason = "returns nothing";
        return false;
    }

    return true;
}

bool CoA::HotPatch::ValidateRequirementType(uint32_t opcode, const std::map<uint32_t, std::string>& strings) {
    for (uint32_t field : { 6u, 7u, 8u }) {
        auto it = strings.find(field);

        if (it == strings.end() || it->second.empty()) {
            continue;
        }

        std::string reason;

        if (!CoA::HotPatch::FormatterAllowed(it->second, reason)) {
            auto token = strings.find(1);
            CoA::Log::Printf("SMSG 0x%X: refused requirement type %s, formatter %u %s", opcode,
                token != strings.end() ? token->second.c_str() : "?", field - 5, reason.c_str());
            return false;
        }
    }

    return true;
}

void CoA::HotPatch::OnEnteredWorld() {
    const bool fresh = NewLuaSession();

    if (fresh) {
        s_awaitingConsent.clear();
        s_awaitingAddon.clear();
        s_sessionBytes = 0;
        s_promptShown = false;
    }

    Execute(kWorldLua);
    SendCapability();

    if (fresh) {
        const std::vector<Patch> replay = s_received;

        for (const Patch& p : replay) {
            Handle(p);
        }
    }
}

// Leaving the world for the glue screen ends the connection's patches; the next login gets its own.
void CoA::HotPatch::OnGlueScreen() {
    s_received.clear();
}

void CoA::HotPatch::Reset() {
    s_records.clear();
    s_awaitingConsent.clear();
    s_awaitingAddon.clear();
    s_received.clear();
    s_sessionBytes = 0;
    s_promptShown = false;
}

void CoA::HotPatch::Init() {
    COA_PACKET(0x8F0, OnHotPatch);
    CoA::DbcPatch::SetValidator(0x5BC, CoA::HotPatch::ValidateRequirementType);
    CoA::ClientHooks::afterEnterWorld.Add(CoA::HotPatch::OnEnteredWorld);
    CoA::Lifecycle::OnGlueScreen(CoA::HotPatch::OnGlueScreen);
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
}
