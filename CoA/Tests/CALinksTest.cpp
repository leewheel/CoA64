#include "CoA/Tests/CAFixture.h"

using namespace CAFixture;

namespace {
    lua_State* Start(uint8_t cls) {
        Setup();
        s_class = cls;
        CoA::CA::Init();
        Feed(0x725, CoATest::Bytes().U32(0).U32(1));
        Feed(0x726, CoATest::Bytes().U32(0));
        return CoATest::NewState(false);
    }

    void Stop(lua_State* L) {
        lua_close(L);
        Teardown();
    }

    V Import(lua_State* L, const std::string& url) {
        return Run(L, ("return C_CharacterAdvancement.ImportPendingBuild('" + url + "')").c_str());
    }
}

// A CoA class imports a plain CoA build string: the talents= parameter, '.' as ':', talent tokens NtN and ability
// spells N; the pending build takes the entries.
COA_TEST(CA_LinkImportCoA) {
    lua_State* L = Start(12);
    CHECK((Import(L, "https://db.example/build?talents=151t2.5800&x=1") == V{ "true", "nil", "nil", "nil" }));
    CHECK(CoA::CA::PendingBuild()->RankOf(151) == 2);
    CHECK(CoA::CA::PendingBuild()->RankOf(150) == 1);
    CHECK((Import(L, "151t1:151t1") == V{ "true", "nil", "nil", "nil" }));   // a repeated rank-1 token adds a rank
    CHECK(CoA::CA::PendingBuild()->RankOf(151) == 2);
    CHECK(!CoA::CA::PendingBuild()->Has(150));
    Stop(L);
}

// The reasons a CoA link is refused, as the original words them.
COA_TEST(CA_LinkImportErrors) {
    lua_State* L = Start(12);
    CHECK((Import(L, "999t1") == V{ "false", "CoA 构建包含未知条目 999。失败前已解析标记数: 1。内容预览: :999t1:", "nil", "nil" }));
    CHECK((Import(L, "100t1") ==
        V{ "false", "CoA 构建包含条目 100（属于 CoA 职业 1，但你当前的职业是 12）。失败前已解析标记数: 1。", "nil", "nil" }));
    CHECK((Import(L, "5800") == V{ "false", "无法解码 CoA 构建链接。输入长度: 4。输入预览: 5800", "nil", "nil" }));
    CHECK((Import(L, "151t1:5000") ==
        V{ "false", "CoA 构建包含技能法术 5000（条目 100，属于 CoA 职业 1，但你当前的职业是 12）。", "nil", "nil" }));
    CHECK((Run(L, "return C_CharacterAdvancement.ImportPendingBuild({})") == V{ "<error> Usage: C_CharacterAdvancement.ImportPendingBuild(buildURL)" }));
    Stop(L);
}

// Without the Build Creator: ExportBuild is "", a build id is invalid, a stock-class link seeds an empty build.
COA_TEST(CA_LinkBuildCreatorAbsent) {
    lua_State* L = Start(1);
    CHECK((Run(L, "return C_CharacterAdvancement.ExportBuild(true)") == V{ "" }));
    CHECK((Run(L, "return C_CharacterAdvancement.ImportPendingBuildID('abc')") == V{ "<error> ImportPendingBuildID: invalid buildID" }));
    CoA::CA::PendingBuild()->AddEntry(100, 1);
    CHECK((Import(L, "https://db.example/overview/xyz") == V{ "true", "nil", "nil", "nil" }));
    CHECK(!CoA::CA::PendingBuild()->Has(100));
    CoA::CA::Dependencies().exportBuild = [](const CoA::CA::Build&, bool res) { return std::string(res ? "with" : "without"); };
    CHECK((Run(L, "return C_CharacterAdvancement.ExportBuild(false)") == V{ "without" }));
    Stop(L);
}
