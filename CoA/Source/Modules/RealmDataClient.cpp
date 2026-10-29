// Realm data hot-swap, client side (port of AscRealmData.cpp).
//
// Switching (FUN_102fac20 -> tick FUN_102fc250, run after ClientIdle and after PaintScreen):
//   0..3 s    the AscensionRealmHotSwapOverlay progress bar fills to 80 %, then 99 %;
//   then      the realm archives are closed, the path is set, the new ones are opened, the UI reloads, every client
//             DBC table is marked unloaded so it reloads (ClientDBInitialize), the current map is reloaded when a
//             player exists, the realm list is re-read; the bar shows 100 %;
//   +250 ms   the overlay hides and the switch ends.
// Opening the realm archives also LoadLibraryA's every *.dll under <base>Data\<path> (recursively; a 64-bit process can
// load only 64-bit DLLs, others fail and are logged) and, when <base>Data\<path>buildinfo exists, takes the build
// number from it for the logon and auth session builds.
//
// Always on: -hd 0 refuses the eight HD archives and Data\patch-Q.MPQ sets the HD flag (SFile::OpenArchive), the WDB
// cache lives in Cache/WDB/<locale>/<realm> (GetCacheDirectory), InitializeClient no longer initializes the WDB cache
// (LoadWDBCache does), and InitializeAddOns also scans each realm archive's Interface\AddOns\ and -addonspath <dir>.
#include "CoA/Source/Modules/Modules.h"
#include "CoA/Source/Modules/RealmData.h"
#include "CoA/Source/Modules/RealmDataClient.h"
#include "CoA/Source/Modules/RealmInfo.h"
#include "CoA/Source/Runtime/Hd.h"
#include "CoA/Source/Runtime/Lifecycle.h"
#include "CoA/Source/Runtime/Log.h"
#include "CoA/Source/Runtime/LuaReg.h"
#include "CoA/Source/Runtime/Script.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Client.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/WowServices/RealmConnection.h"
#include "Engine/Source/Os/OsFile.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WowServices/GruntLogin.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int32_t CCommand_ReloadUI(const char* command, const char* arguments);

namespace {
    // Every client table the original knows by name is marked unloaded so ClientDBInitialize reloads it.
    void ClientDBMarkAllUnloaded() {
#define COA_CLIENT_DB(db) db.m_loaded = 0;
#include "CoA/Source/Modules/ClientDbcs.generated.inc"
#undef COA_CLIENT_DB
    }
}

namespace {
    std::string s_prefix;                  // 0x10BCC0C0: "Data\" + path + "\", its c_str in s_dataDirectories[2] (0xAB6160)
    bool s_overlayCreated = false;         // 0x10BCC0DA
    bool s_swapDue = false;                // 0x10BCC0DB
    int64_t s_startNs = 0;                 // 0x10BCC0E0
    int64_t s_finishNs = 0;                // 0x10BCC0E8
    std::vector<SArchive*> s_archives;     // 0x10BE3B0C

    int64_t NowNs() {   // FUN_100b8230
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void RunLua(const char* code) {   // FrameScript_Execute(code, 0, 0)
        FrameScript_Execute(code, nullptr, nullptr);
    }

    // FUN_102fb830: a Lua string literal -- "..." with \n, \r, \" and \\ escaped.
    std::string Quote(const std::string& s) {
        std::string out = "\"";

        for (char c : s) {
            switch (c) {
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                default:
                    out += c;
                    break;
            }
        }

        return out + "\"";
    }

    const char* const s_overlay =
        "\nif UIParent and not AscensionRealmHotSwapOverlay then\n"
        "    local f = CreateFrame(\"Frame\", \"AscensionRealmHotSwapOverlay\", UIParent)\n"
        "    f:SetFrameStrata(\"FULLSCREEN_DIALOG\")\n"
        "    f:SetAllPoints(UIParent)\n"
        "    f:EnableMouse(true)\n"
        "    f:Hide()\n\n"
        "    f.bg = f:CreateTexture(nil, \"BACKGROUND\")\n"
        "    f.bg:SetAllPoints(f)\n"
        "    f.bg:SetTexture(0, 0, 0, 1)\n\n"
        "    f.bar_bg = f:CreateTexture(nil, \"ARTWORK\")\n"
        "    f.bar_bg:SetPoint(\"CENTER\", f, \"CENTER\", 0, -70)\n"
        "    f.bar_bg:SetWidth(360)\n"
        "    f.bar_bg:SetHeight(18)\n"
        "    f.bar_bg:SetTexture(0.08, 0.08, 0.08, 1)\n\n"
        "    f.bar = f:CreateTexture(nil, \"OVERLAY\")\n"
        "    f.bar:SetPoint(\"LEFT\", f.bar_bg, \"LEFT\", 0, 0)\n"
        "    f.bar:SetHeight(18)\n"
        "    f.bar:SetWidth(1)\n"
        "    f.bar:SetTexture(0.85, 0.78, 0.45, 1)\n\n"
        "    f.text = f:CreateFontString(nil, \"OVERLAY\", \"GameFontNormalLarge\")\n"
        "    f.text:SetPoint(\"BOTTOM\", f.bar_bg, \"TOP\", 0, 14)\n\n"
        "    f.percent = f:CreateFontString(nil, \"OVERLAY\", \"GameFontHighlightSmall\")\n"
        "    f.percent:SetPoint(\"TOP\", f.bar_bg, \"BOTTOM\", 0, -10)\n"
        "end\n";

    // FUN_102fbac0: create the overlay once, then show `progress` (clamped to 0..1).
    void ShowOverlay(float progress) {
        CoA::RealmData::State& state = CoA::RealmData::GetState();

        if (!s_overlayCreated) {
            RunLua(s_overlay);
            s_overlayCreated = true;
        }

        float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
        float width = p * 360.0f;

        if (width <= 1.0f) {
            width = 1.0f;
        }

        int percent = static_cast<int>(p * 100.0f + 0.5f);
        std::string text = "正在切换领域数据";   // 中文化（By leewheel 2026-10-29）

        if (!state.pending.empty()) {
            text += ": " + state.pending;
        }

        char widthText[32];
        snprintf(widthText, sizeof(widthText), "%g", width);   // ostream << float
        std::string code = std::string("if AscensionRealmHotSwapOverlay then ") + "local f = AscensionRealmHotSwapOverlay; " + "f:Show(); " +
            "f.bar:SetWidth(" + widthText + "); " + "f.percent:SetText(" + Quote(std::to_string(percent) + "%") + "); " +
            "f.text:SetText(" + Quote(text) + "); " + "end";
        RunLua(code.c_str());
    }

    // FUN_102fb780: hide the overlay and end the switch.
    void EndSwitch() {
        CoA::RealmData::State& state = CoA::RealmData::GetState();

        if (s_overlayCreated) {
            RunLua("if AscensionRealmHotSwapOverlay then AscensionRealmHotSwapOverlay:Hide() end");
        }

        state.active = false;
        s_swapDue = false;
        state.pending.clear();
    }

    // FUN_102fa650: the path, and the client's data prefix s_dataDirectories[2] (0xAB6160) -> "Data\<path>\".
    void SetPath(const std::string& path) {
        CoA::RealmData::GetState().path = path;
        s_prefix = "Data\\" + path + "\\";
        s_dataDirectories[2] = s_prefix.c_str();
    }

    // FUN_102f87d0: close every realm archive, then SFile::RebuildHash.
    void CloseArchives() {
        for (SArchive* archive : s_archives) {
            SFile::CloseArchive(archive);
        }

        s_archives.clear();
        SFile::RebuildHash();
    }

    // <client base> + the client's data directory.
    std::string BaseDir() {
        char base[0x104] = {};
        SFile::GetBasePath(base, sizeof(base));
        return std::string(base) + SFile::GetDataPath();
    }

    // FUN_102f8de0: open the realm's archives, load its DLLs, apply its build number, then SFile::RebuildHash.
    void OpenRealmArchives() {
        const std::string& path = CoA::RealmData::CurrentPath();

        if (!path.empty()) {
            std::string dir = BaseDir() + path;
            int32_t priority = 0x200;

            for (const std::string& name : CoA::RealmData::ListArchive(dir)) {
                SArchive* handle = nullptr;
                std::string archive = "Data\\" + path + "\\" + name;

                if (SFile::OpenArchive(archive.c_str(), priority++, 0xC00, &handle)) {
                    s_archives.push_back(handle);
                }
            }

            std::error_code ec;

            if (std::filesystem::exists(dir, ec)) {
                for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (it->path().extension() == L".dll" && !LoadLibraryA(it->path().string().c_str())) {
                        CoA::Log::Printf("RealmData: %s did not load (error %lu)", it->path().string().c_str(), GetLastError());
                    }
                }
            }

            // The original patches the build number into the logon challenge (0x8CA5F0) and CMSG_AUTH_SESSION
            // (0x4648B3) immediates, and the address of its digits over the build string 0x9F5200 (which corrupts
            // that string; it is left as it is here).
            uint32_t build = CoA::RealmData::BuildNumber(dir);
            g_coaLogonBuild = static_cast<uint16_t>(build);
            g_coaAuthSessionBuild = static_cast<uint16_t>(build);
        }

        SFile::RebuildHash();
    }

    void ReloadMapIfInWorld() {
        if (ClntObjMgrGetActivePlayerObj()) {
            if (const MapRec* map = g_mapDB.GetRecord(CGGameUI::m_iCurrentMapID)) {
                CMap::Load(map->m_directory, 1);
            }
        }
    }

    void ReloadRealmList() {
        ClientServices::InitLoginServerCVars(1, CoA::RealmData::CurrentPath().c_str());
    }

    // FUN_102fc250: the switch, one frame at a time.
    void Tick() {
        CoA::RealmData::State& state = CoA::RealmData::GetState();

        if (!state.active) {
            return;
        }

        int64_t now = NowNs();

        if (state.finished) {
            if (now - s_finishNs >= 250000000) {
                EndSwitch();
            }

            return;
        }

        int64_t elapsedMs = (now - s_startNs) / 1000000;

        if (s_swapDue) {
            std::string path = state.pending;
            CloseArchives();
            SetPath(path);
            OpenRealmArchives();
            CCommand_ReloadUI("", "");
            s_overlayCreated = false;
            ClientDBMarkAllUnloaded();
            ClientDBInitialize();
            ReloadMapIfInWorld();
            ReloadRealmList();
            CoA::Log::Printf("RealmData: switched to '%s', %u realm archive(s) open", state.path.c_str(), static_cast<unsigned>(s_archives.size()));

            if (state.active) {
                state.finished = true;
                s_finishNs = NowNs();
                ShowOverlay(1.0f);
            }

            return;
        }

        if (elapsedMs < 3000) {
            ShowOverlay(static_cast<float>(elapsedMs) * 0.8f / 3000.0f);
            return;
        }

        ShowOverlay(0.99f);
        s_swapDue = true;
    }

    // FUN_102fac20: already on `path`, or already switching to it: nothing. Otherwise start a switch.
    void Switch(const std::string& path) {
        CoA::RealmData::State& state = CoA::RealmData::GetState();

        if (!CoA::RealmData::NeedsSwitch(path)) {
            return;
        }

        state.active = true;
        state.finished = false;
        s_swapDue = false;
        s_startNs = NowNs();
        state.pending = path;
        ShowOverlay(0.0f);
    }

    void SwitchFromRealmInfo(const char* path) {
        Switch(path);
    }

    // FUN_102fa4c0, the start of SFile::OpenArchive: -hd 0 refuses the eight HD archives; Data\patch-Q.MPQ sets the HD
    // flag. False refuses the open.
    bool OpenArchiveFilter(const char* name) {
        static const char* const hdArchives[] = {
            "Data\\patch-4.MPQ",
            "Data\\patch-5.MPQ",
            "Data\\patch-X.MPQ",
            "Data\\patch-U.MPQ",
            "Data\\patch-Z.MPQ",
            "Data\\patch-Q.MPQ",
            "Data\\patch-R.MPQ",
            "Data\\patch-V.MPQ",
        };

        const char* hd = CoA::Script::CommandLineArg("hd");

        // CoA64: HD also needs the Vulkan (DXVK) renderer.
        if ((hd && strcmp(hd, "0") == 0) || !CoA::Hd::Available()) {
            for (const char* archive : hdArchives) {
                if (SStrCmpI(name, archive, STORM_MAX_STR) == 0) {
                    return false;
                }
            }
        }

        if (SStrCmpI(name, "Data\\patch-Q.MPQ", STORM_MAX_STR) == 0) {
            CoA::RealmData::GetState().hdPatch = true;
        }

        return true;
    }

    // FUN_102f8820: "realmdata" with no argument returns to the stock data at once; with one, switches.
    int32_t RealmDataCommand(const char* command, const char* arguments) {
        if (!*arguments) {
            CloseArchives();
            SetPath("");
            CCommand_ReloadUI("", "");
            ReloadMapIfInWorld();
            ReloadRealmList();
            return 1;
        }

        Switch(arguments);
        return 1;
    }

    // FUN_102fa570: SetDataPath(path).
    int SetDataPath(lua_State* L) {
        Switch(CoA::Script::CheckString(L, 1));
        return 0;
    }

    const CoA::Binding s_bindings[] = {
        COA_NATIVE("", "SetDataPath", SetDataPath),
    };
}

// FUN_102fb1c0: after the client's own archives.
void CoA::RealmDataClient::AfterOpenArchives() {
    OpenRealmArchives();
}

// FUN_102fb130: after InitializeAddOns, each realm archive's Interface\AddOns\ and -addonspath <dir>.
void CoA::RealmDataClient::AfterInitializeAddOns(int32_t (*archiveCallback)(const char*, void*), int32_t (*diskCallback)(OS_FILE_DATA&, void*), void* param) {
    for (SArchive* archive : s_archives) {
        ListArchiveFiles(archive, "Interface\\AddOns\\", archiveCallback, param);
    }

    if (const char* dir = AddOnsPath()) {
        OsFileList(dir, "*", diskCallback, param, 0);
    }
}

const char* CoA::RealmDataClient::AddOnsPath() {
    return CoA::Script::CommandLineArg("addonspath");
}

// FUN_102fb520.
void CoA::RealmDataClient::CacheDirectory(char* path) {
    std::string cache = CoA::RealmData::CachePath(s_localeCode, ClientServices::s_realmNameVar->m_stringValue.m_str);
    SStrCopy(path, cache.c_str(), 0x104);
}

void CoA::RealmDataClient::Init() {
    const char* initial = CoA::Script::CommandLineArg("realmdata");
    SetPath(initial ? initial : "");
    g_sfileOpenArchiveHook = OpenArchiveFilter;
    CoA::Lifecycle::OnAfterIdle(Tick);
    CoA::Lifecycle::OnAfterPaint(Tick);
    CoA::RealmInfo::SetDataPathHandler(SwitchFromRealmInfo);
    ConsoleCommandRegister("realmdata", RealmDataCommand, static_cast<CATEGORY>(1), nullptr);
    CoA::LuaReg::AddModule(s_bindings, sizeof(s_bindings) / sizeof(s_bindings[0]));
}
