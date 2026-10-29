// C_BuildCreator / C_BuildEditor: the packets, the natives, the container's filter and the glue reset.
#include "CoA/Source/Runtime/Lifecycle.h"
#include "CoA/Tests/BuildCreatorFixture.h"

using namespace BCFixture;
using CoATest::Bytes;
using CoATest::Expected;
using CoATest::SentPackets;

namespace {
    // The queried / bookmarked ids in order.
    V Bookmarks(lua_State* L) {
        return R(L, "local t = {} for i = 1, C_BuildCreator.GetNumBookmarkedBuilds() do t[#t + 1] = C_BuildCreator.GetBookmarkedBuildAtIndex(i) end "
                    "return unpack(t)");
    }

    // The filtered category page's ids in order.
    V Filtered(lua_State* L, const std::string& args = "{}", const std::string& sorts = "{}", const std::string& text = "") {
        R(L, "C_BuildCreator.UpdateFilter('" + text + "', " + args + ", " + sorts + ")");
        return R(L, "local t = {} for i = 1, C_BuildCreator.GetNumBuilds() do t[#t + 1] = C_BuildCreator.GetBuildAtIndex(i).ID end return unpack(t)");
    }

    V Spells(lua_State* L, const char* getter = "C_BuildEditor.GetPendingBuild()") {
        return R(L, std::string("local b = ") + getter + " local t = {} for i, s in ipairs(b.Spells) do t[#t + 1] = s.Spell .. '@' .. s.Level end return unpack(t)");
    }

    Bytes Ids(const std::vector<const char*>& ids) {
        Bytes b;
        b.U32(static_cast<uint32_t>(ids.size()));

        for (const char* id : ids) {
            b.Str(id);
        }

        return b;
    }

    std::vector<uint8_t> Sent() {
        return SentPackets().empty() ? std::vector<uint8_t>() : SentPackets().back();
    }
}

// SMSG 0x631: an unknown build is appended, a known one replaced, a known one with no spells erased; each signals
// BUILD_CREATOR_BUILD_RESULT(text, id). A hidden build is dropped silently unless it is owned or the player is a GM. The
// lists are sorted on receipt.
COA_TEST(BuildCreator_PacketBuild) {
    lua_State* L = Start();
    CHECK((Deliver631(Build("b1"), "first") == V{ "BUILD_CREATOR_BUILD_RESULT:first,b1" }));
    Deliver631(Build("b2"));
    CHECK((Bookmarks(L) == V{ "b1", "b2" }));
    E e = Build("b1", { { 5001, 20 }, { 5000, 10 } });
    e.name = "Renamed";
    e.weapons = { { 7 }, { 2 } };
    e.armor = { { 4 }, { 1 } };
    e.enchants = { { 1001, 1, 5 }, { 1002, 1, 3 }, { 1003, 1, 5 } };   // level 5: uncommon (1003) before rare (1001)
    Deliver631(e);
    CHECK((Bookmarks(L) == V{ "b1", "b2" }));
    CHECK((R(L, "return C_BuildCreator.GetBuild('b1').Name") == V{ "Renamed" }));
    CHECK((Spells(L, "C_BuildCreator.GetBuild('b1')") == V{ "5000@10", "5001@20" }));
    CHECK((R(L, "local b = C_BuildCreator.GetBuild('b1') return b.WeaponTypes[1].Type, b.WeaponTypes[2].Type, b.ArmorTypes[1].Type, b.ArmorTypes[2].Type, "
                "b.RandomEnchants[1].Enchant, b.RandomEnchants[2].Enchant, b.RandomEnchants[3].Enchant") ==
        V{ "ITEM_SUBCLASS_WEAPON_BOW", "ITEM_SUBCLASS_WEAPON_SWORD", "ITEM_SUBCLASS_ARMOR_CLOTH", "ITEM_SUBCLASS_ARMOR_PLATE", "1002", "1003", "1001" }));
    CHECK((Deliver631(Build("b1", {}), "gone") == V{ "BUILD_CREATOR_BUILD_RESULT:gone,b1" }));
    CHECK((Bookmarks(L) == V{ "b2" }));
    CHECK((Deliver631(Build("b3", {})) == V{ "BUILD_CREATOR_BUILD_RESULT:q,b3" }));   // unknown with no spells: appended
    CHECK((Bookmarks(L) == V{ "b2", "b3" }));
    E hidden = Build("h1");
    hidden.hidden = 1;
    CHECK((Deliver631(hidden) == V{}));
    CHECK((Bookmarks(L) == V{ "b2", "b3" }));
    Feed(0x634, Ids({ "h1" }));
    CHECK((Deliver631(hidden) == V{ "BUILD_CREATOR_BUILD_RESULT:q,h1" }));
    hidden.id = "h2";
    Gm(1);
    CHECK((Deliver631(hidden) == V{ "BUILD_CREATOR_BUILD_RESULT:q,h2" }));
    CHECK((Bookmarks(L) == V{ "b2", "b3", "h1", "h2" }));
    CHECK((R(L, "return C_BuildCreator.GetBuild('h2').NeedsRepairs") == V{ "true" }));
    Stop(L);
}

// SMSG 0x62F: page 0 clears the page (and the filter); a known id is erased and re-appended (moving it to the end), or
// only erased when it has no spells; hidden builds are dropped; BUILD_CREATOR_CATEGORY_RESULT(category of the last
// QueryAllBuilds, text) after the last page only.
COA_TEST(BuildCreator_PacketCategoryPage) {
    lua_State* L = Start();
    R(L, "C_BuildCreator.QueryAllBuilds('Level60PvE')");
    CHECK((DeliverPage({ Build("a"), Build("b") }, 0, 2, "t") == V{}));
    CHECK((Filtered(L) == V{ "a", "b" }));
    E hidden = Build("h");
    hidden.hidden = 1;
    CHECK((DeliverPage({ Build("c"), Build("a"), hidden }, 1, 2, "t") == V{ "BUILD_CREATOR_CATEGORY_RESULT:Level60PvE,t" }));
    // A later page does not reset the filter: it still holds page slots 0 and 1 (the original's pointers), now b and c.
    CHECK((Filtered(L) == V{ "b", "c" }));
    DeliverPage({ Build("b", {}) }, 5, 9);   // no clear, b erased, no signal before the last page
    CHECK((Filtered(L) == V{ "c", "a" }));
    CHECK((DeliverPage({ Build("z") }, 0, 1) == V{ "BUILD_CREATOR_CATEGORY_RESULT:Level60PvE,p" }));
    CHECK((Filtered(L) == V{ "z" }));
    CHECK((R(L, "return C_BuildCreator.GetNumBuilds()") == V{ "1" }));
    DeliverPage({ Build("y") }, 0, 1);   // page 0 resets the filter: nothing is listed until the next UpdateFilter
    CHECK((R(L, "return C_BuildCreator.GetNumBuilds(), C_BuildCreator.GetBuildAtIndex(1)") == V{ "0", "nil" }));
    R(L, "C_BuildCreator.QueryAllBuilds('Bogus')");   // an unknown category: UNEXPECTED_ENUM_VALUE_4294967295
    CHECK((DeliverPage({}, 0, 1, "x") == V{ "BUILD_CREATOR_CATEGORY_RESULT:UNEXPECTED_ENUM_VALUE_4294967295,x" }));
    CHECK((DeliverPage({}, 0, 0) == V{}));   // pages - 1 wraps: never the last page
    Stop(L);
}

// The 7a convention: counts are trusted only while a record still fits.
COA_TEST(BuildCreator_PacketCountGuards) {
    lua_State* L = Start();
    Bytes b;
    b.Str("t").U32(0).U32(1).U32(1000);
    Put(b, Build("only"));
    CHECK((Feed(0x62F, b) == V{ "BUILD_CREATOR_CATEGORY_RESULT:None,t" }));
    CHECK((Filtered(L) == V{ "only" }));
    Bytes c;
    c.Str("t");
    E e = Build("x", {});
    Put(c, e);
    c.data.resize(c.data.size() - 9 - 4 * 4);   // cut before the four (empty) list counts
    c.U32(5000000);                             // a spell count with nothing behind it
    Feed(0x631, c);
    CHECK((Spells(L, "C_BuildCreator.GetBuild('x')") == V{}));
    Feed(0x633, Bytes().U32(100000).Str("s0").U8(1).U8(1));
    CHECK((R(L, "return C_BuildCreator.GetActiveBuild(1), C_BuildCreator.GetActiveBuild(2)") == V{ "s0", "" }));
    Feed(0x634, Bytes().U32(1000).Str("o1"));
    CHECK((R(L, "return C_BuildCreator.IsOwnedBuild('o1'), C_BuildCreator.IsOwnedBuild('')") == V{ "true", "false" }));
    Feed(0x632, Bytes().U32(0x7FFFFFFF).Str("far").U8(0).U8(0));   // past the spec cap: signalled, not stored
    CHECK((R(L, "return C_BuildCreator.GetActiveSpecForBuild('far')") == V{ "-1" }));
    Stop(L);
}

// SMSG 0x627: PUBLISH_BUILD_OK makes the created build the pending one, appends it to the queried list, the page (when no
// category or its category was queried) and the owned ids; the result is reported with CREATE_ for PUBLISH_.
COA_TEST(BuildCreator_PacketCreate) {
    lua_State* L = Start();
    auto create = [](const char* result, const E* e) {
        Bytes b;
        b.Str(result);

        if (e) {
            Put(b, *e);
        }

        return Feed(0x627, b);
    };
    E e = Build("n1", { { 5001, 20 }, { 5000, 10 } });
    CHECK((create("PUBLISH_BUILD_OK", &e) == V{ "BUILD_CREATOR_CREATE_RESULT:CREATE_BUILD_OK,n1" }));
    CHECK((R(L, "return C_BuildEditor.GetPendingBuild().ID, C_BuildCreator.IsOwnedBuild('n1')") == V{ "n1", "true" }));
    CHECK((Spells(L) == V{ "5000@10", "5001@20" }));
    CHECK((Bookmarks(L) == V{ "n1" }));
    CHECK((Filtered(L) == V{ "n1" }));
    R(L, "C_BuildCreator.QueryAllBuilds('Leveling')");
    E other = Build("n2");
    other.category = 2;
    create("PUBLISH_BUILD_OK", &other);
    CHECK((Filtered(L) == V{ "n1" }));   // category 2 is not the queried 1
    other.id = "n3";
    other.category = 1;
    create("PUBLISH_BUILD_OK", &other);
    CHECK((Filtered(L) == V{ "n1", "n3" }));
    CHECK((Bookmarks(L) == V{ "n1", "n2", "n3" }));
    CHECK((create("PUBLISH_BUILD_NO_NAME", nullptr) == V{ "BUILD_CREATOR_CREATE_RESULT:CREATE_BUILD_NO_NAME," }));
    CHECK((create("SOMETHING", nullptr) == V{ "BUILD_CREATOR_CREATE_RESULT:SOMETHING," }));
    CHECK((create("PUBLISH_", nullptr) == V{ "BUILD_CREATOR_CREATE_RESULT:CREATE_," }));
    CHECK((R(L, "return C_BuildEditor.GetPendingBuild().ID") == V{ "n3" }));
    Stop(L);
}

// SMSG 0x629: PUBLISH_BUILD_OK makes the saved build the pending one and replaces (or appends) its queried copy and
// replaces its page copy; a failure carries the id; reported with SAVE_.
COA_TEST(BuildCreator_PacketSave) {
    lua_State* L = Start();
    DeliverPage({ Build("p1"), Build("p2") });
    Deliver631(Build("q1"));
    auto save = [](const char* result, const E& e) {
        Bytes b;
        b.Str(result);
        Put(b, e);
        return Feed(0x629, b);
    };
    E e = Build("p2");
    e.name = "Saved";
    CHECK((save("PUBLISH_BUILD_OK", e) == V{ "BUILD_CREATOR_SAVE_RESULT:SAVE_BUILD_OK,p2" }));
    CHECK((Bookmarks(L) == V{ "q1", "p2" }));   // appended: it was not queried
    CHECK((Filtered(L) == V{ "p1", "p2" }));
    CHECK((R(L, "return C_BuildCreator.GetBuildAtIndex(2).Name, C_BuildEditor.GetPendingBuild().Name") == V{ "Saved", "Saved" }));
    e.id = "q1";
    e.name = "Q";
    save("PUBLISH_BUILD_OK", e);
    CHECK((Bookmarks(L) == V{ "q1", "p2" }));
    CHECK((R(L, "return C_BuildCreator.GetBookmarkedBuildAtIndex(1), C_BuildCreator.GetBuild('q1').Name") == V{ "q1", "Q" }));
    Bytes fail;
    fail.Str("PUBLISH_BUILD_NOT_FOUND").Str("zz");
    CHECK((Feed(0x629, fail) == V{ "BUILD_CREATOR_SAVE_RESULT:SAVE_BUILD_NOT_FOUND,zz" }));
    CHECK((R(L, "return C_BuildEditor.GetPendingBuild().ID") == V{ "q1" }));
    Stop(L);
}

// RateBuild remembers (id, up); SMSG 0x62B RATE_BUILD_OK adds the id to the upvoted list (+1 upvote) or removes it (-1);
// every result signals (result, id, the build's upvotes or 0).
COA_TEST(BuildCreator_PacketRate) {
    lua_State* L = Start();
    E e = Build("r1");
    e.upvotes = 5;
    Deliver631(e);
    CoATest::RecordPackets();
    CHECK((R(L, "return C_BuildCreator.RateBuild('r1', true)") == V{ "true" }));
    CHECK(Sent() == Expected(0x62A, Bytes().Str("r1").U8(1)));
    CHECK((Feed(0x62B, Bytes().Str("RATE_BUILD_OK")) == V{ "BUILD_CREATOR_RATE_RESULT:RATE_BUILD_OK,r1,6" }));
    CHECK((R(L, "return C_BuildCreator.IsUpvotedBuild('r1'), C_BuildCreator.GetBuild('r1').Upvotes") == V{ "true", "6" }));
    CHECK((Feed(0x62B, Bytes().Str("RATE_BUILD_ALREADY_UPVOTED")) == V{ "BUILD_CREATOR_RATE_RESULT:RATE_BUILD_ALREADY_UPVOTED,r1,6" }));
    CHECK((Feed(0x62B, Bytes().Str("NONSENSE")) == V{ "BUILD_CREATOR_RATE_RESULT:NONSENSE,r1,6" }));
    R(L, "C_BuildCreator.RateBuild('r1', false)");
    CHECK(Sent() == Expected(0x62A, Bytes().Str("r1").U8(0)));
    CHECK((Feed(0x62B, Bytes().Str("RATE_BUILD_OK")) == V{ "BUILD_CREATOR_RATE_RESULT:RATE_BUILD_OK,r1,5" }));
    CHECK((R(L, "return C_BuildCreator.IsUpvotedBuild('r1')") == V{ "false" }));
    R(L, "C_BuildCreator.RateBuild('unknown', true)");
    CHECK((Feed(0x62B, Bytes().Str("RATE_BUILD_OK")) == V{ "BUILD_CREATOR_RATE_RESULT:RATE_BUILD_OK,unknown,0" }));
    CHECK((R(L, "return C_BuildCreator.IsUpvotedBuild('unknown')") == V{ "true" }));
    CHECK((R(L, "return C_BuildCreator.RateBuild('r1')") == V{}));   // the boolean is required
    CHECK((R(L, "return C_BuildCreator.RateBuild(1, nil)") == V{ "true" }));   // a number reads as "1", nil as false
    CHECK(Sent() == Expected(0x62A, Bytes().Str("1").U8(0)));
    Stop(L);
}

// DeleteBuild remembers the id; SMSG 0x62D DELETE_BUILD_OK drops it from the queried list, the page, the owned and
// upvoted ids, and a pending copy of it becomes a fresh build of the player's class.
COA_TEST(BuildCreator_PacketDelete) {
    lua_State* L = Start();
    DeliverPage({ Build("d1"), Build("d2") });
    Deliver631(Build("d1"));
    Feed(0x634, Ids({ "d1", "d2" }));
    Feed(0x635, Ids({ "d1" }));
    R(L, "C_BuildEditor.EditBuild('d1')");
    CoATest::RecordPackets();
    CHECK((R(L, "return C_BuildCreator.DeleteBuild('d1')") == V{ "true" }));
    CHECK(Sent() == Expected(0x62C, Bytes().Str("d1")));
    CHECK((Feed(0x62D, Bytes().Str("DELETE_BUILD_NO_PERMISSION")) == V{ "BUILD_CREATOR_DELETE_RESULT:DELETE_BUILD_NO_PERMISSION" }));
    CHECK((Bookmarks(L) == V{ "d1" }));
    CHECK((Feed(0x62D, Bytes().Str("DELETE_BUILD_OK")) == V{ "BUILD_CREATOR_DELETE_RESULT:DELETE_BUILD_OK" }));
    CHECK((Bookmarks(L) == V{}));
    CHECK((Filtered(L) == V{ "d2" }));
    CHECK((R(L, "return C_BuildCreator.IsOwnedBuild('d1'), C_BuildCreator.IsOwnedBuild('d2'), C_BuildCreator.IsUpvotedBuild('d1')") ==
        V{ "false", "true", "false" }));
    CHECK((R(L, "local b = C_BuildEditor.GetPendingBuild() return b.ID, b.Name, b.Roles") == V{ "", "", "PLAYER_ROLE_DAMAGE" }));
    CHECK(CoA::BuildCreator::PendingHasSpell(5000) == false);
    CHECK((Feed(0x62D, Bytes().Str("??")) == V{ "BUILD_CREATOR_DELETE_RESULT:??" }));
    Stop(L);
}

// SMSG 0x632 (one spec's record, growing the list; BUILD_CREATOR_ACTIVE_BUILD_UPDATE(spec)) and 0x633 (the whole list,
// cleared first); GetActiveBuild is 1-based ("" past the end), GetActiveSpecForBuild 0-based (-1 for none).
COA_TEST(BuildCreator_PacketActiveBuilds) {
    lua_State* L = Start();
    CHECK((Feed(0x632, Bytes().U32(2).Str("b2").U8(1).U8(0)) == V{ "BUILD_CREATOR_ACTIVE_BUILD_UPDATE:2" }));
    CHECK((R(L, "return C_BuildCreator.GetActiveBuild(3), C_BuildCreator.GetActiveBuild(1), C_BuildCreator.GetActiveBuild(4)") == V{ "b2", "", "" }));
    CHECK((R(L, "return C_BuildCreator.GetActiveSpecForBuild('b2'), C_BuildCreator.GetActiveSpecForBuild('x')") == V{ "2", "-1" }));
    CHECK((R(L, "return C_BuildCreator.GetActiveSpecForBuild('')") == V{ "0" }));   // spec 0 is an empty record
    CHECK(CoA::BuildCreator::SpecHasActiveBuild(2));
    CHECK(!CoA::BuildCreator::SpecHasActiveBuild(0));
    CHECK((R(L, "return C_BuildCreator.GetActiveBuild(0)") == V{}));
    Feed(0x632, Bytes().U32(0).Str("b0").U8(0).U8(0));
    CHECK((R(L, "return C_BuildCreator.GetActiveBuild(1), C_BuildCreator.GetActiveBuild(3)") == V{ "b0", "b2" }));
    CHECK((Feed(0x633, Bytes().U32(1).Str("only").U8(0).U8(1)) == V{}));
    CHECK((R(L, "return C_BuildCreator.GetActiveBuild(1), C_BuildCreator.GetActiveBuild(3)") == V{ "only", "" }));
    CHECK(!CoA::BuildCreator::SpecHasActiveBuild(2));
    Feed(0x633, Bytes().U32(0));
    CHECK(!CoA::BuildCreator::SpecHasActiveBuild(0));
    Stop(L);
}

// SMSG 0x634 / 0x635 replace the owned / upvoted ids; 0x5C3 / 0x5C5 signal their result string.
COA_TEST(BuildCreator_PacketIdsAndResults) {
    lua_State* L = Start();
    Feed(0x634, Ids({ "a", "b" }));
    Feed(0x634, Ids({ "c" }));
    CHECK((R(L, "return C_BuildCreator.IsOwnedBuild('a'), C_BuildCreator.IsOwnedBuild('c')") == V{ "false", "true" }));
    Feed(0x635, Ids({ "x" }));
    Feed(0x635, Ids({ "y", "z" }));
    CHECK((R(L, "return C_BuildCreator.IsUpvotedBuild('x'), C_BuildCreator.IsUpvotedBuild('z'), C_BuildCreator.IsOwnedBuild('c')") ==
        V{ "false", "true", "true" }));
    CHECK((R(L, "return C_BuildCreator.IsOwnedBuild()") == V{}));
    CHECK((Feed(0x5C3, Bytes().Str("ACTIVATE_OK")) == V{ "BUILD_CREATOR_ACTIVATE_RESULT:ACTIVATE_OK" }));
    CHECK((Feed(0x5C5, Bytes().Str("DEACTIVATE_NO")) == V{ "BUILD_CREATOR_DEACTIVATE_RESULT:DEACTIVATE_NO" }));
    Stop(L);
}

// The entry table (FUN_1009c1f0).
COA_TEST(BuildCreator_EntryTable) {
    lua_State* L = Start();
    E e = Build("t1", { { 100, 10, 1 }, { 5000, 10, 0, 1, 0, 0, 7, "note" }, { 102, 20, 0, 0, 1 } });
    e.leveling = "lv";
    e.pve = "pve";
    e.pvp = "pvp";
    e.author = "";
    e.subtext = "sub";
    e.upvotes = 3;
    e.created = 0x100000005ull;
    e.updated = 9;
    e.category = 5;
    e.roles = 15;
    e.stat = 3;
    e.enchants = { { 1001, 2, 1, 4, "c1" }, { 1002, 1, 2, 0, "" } };
    e.weapons = { { 0x15, "w" } };
    e.armor = { { 0xB, "a" } };
    e.flags = 0x20;
    e.difficulty = 4;
    Deliver631(e);
    const std::string g = "local b = C_BuildCreator.GetBuild('t1') return ";
    CHECK((R(L, g + "b.ID, b.LevelingID, b.EndGameIDPVE, b.EndGameIDPVP, b.AuthorName, b.Name, b.Subtext, b.Description, b.Icon") ==
        V{ "t1", "lv", "pve", "pvp", "Tester", "A build", "sub", "Desc", "icon" }));
    CHECK((R(L, g + "b.CreatedTime, b.UpdatedTime, b.Category, b.Roles, b.PrimaryStat, b.NeedsRepairs, b.Flags, b.Upvotes, b.DifficultyRating") ==
        V{ "5", "9", "Level70PvE", "PLAYER_ROLE_ANY", "STAT_INTELLECT", "false", "32", "3", "BUILD_DIFFICULTY_RATING_IMPOSSIBLE" }));
    CHECK((R(L, g + "#b.Spells, b.Spells[2].Spell, b.Spells[2].Level, b.Spells[2].IsCoreAbility, b.Spells[2].IsOptimalAbility, "
                    "b.Spells[2].IsEmpoweringAbility, b.Spells[2].IsSynergisticAbility, b.Spells[2].Flags, b.Spells[2].Comment") ==
        V{ "3", "5000", "10", "false", "true", "false", "false", "7", "note" }));
    CHECK((R(L, g + "b.RandomEnchants[1].Enchant, b.RandomEnchants[1].Stacks, b.RandomEnchants[1].Level, b.RandomEnchants[1].Flags, "
                    "b.RandomEnchants[1].Comment, b.WeaponTypes[1].Type, b.WeaponTypes[1].Comment, b.ArmorTypes[1].Type, b.ArmorTypes[1].Comment") ==
        V{ "1001", "2", "1", "4", "c1", "ITEM_SUBCLASS_WEAPON_MAX", "w", "ITEM_SUBCLASS_ARMOR_MAX", "a" }));
    CHECK((R(L, g + "b.LegendaryEnchant") == V{ "1002" }));
    // The spell-kind counts index CharacterAdvancement.dbc by the spell value: spell 100 is row 100 (an ability, core),
    // 102 row 102 (a talent, empowering); 5000 has no row 5000.
    CHECK((R(L, g + "b.NumCoreSpells, b.NumCoreAbilities, b.NumCoreTalents, b.NumOptimalSpells, b.NumEmpoweringSpells, b.NumEmpoweringTalents, "
                    "b.NumKnownCoreSpells, b.NumSynergisticSpells") == V{ "1", "1", "0", "0", "1", "1", "0", "0" }));
    CoA::CA::ActiveBuild()->AddEntry(100, 1);
    CHECK((R(L, g + "b.NumKnownCoreSpells, b.NumKnownCoreAbilities, b.NumKnownEmpoweringTalents") == V{ "1", "1", "0" }));
    // Essence and costs are the PENDING entry's, whichever entry is serialised.
    CHECK((R(L, g + "b.AbilityEssence, b.TalentEssence, b.CommonCost, b.LegendaryCost") == V{ "0", "0", "0", "0" }));
    R(L, "C_BuildEditor.DiscardPendingBuild() C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ")");
    CoA::CA::BuildSeed seed;
    seed.classKey = 1;
    seed.spells = { { 5000, 10 } };
    const CoA::CA::Build b(seed, 60, false);
    CHECK((R(L, g + "b.AbilityEssence, b.TalentEssence") == V{ std::to_string(b.GlobalAE(0)), std::to_string(b.GlobalTE(0)) }));
    CHECK(b.GlobalAE(0) == 2);
    // The author falls back to the player's name, "" for a nameless one, "Unknown" without a player.
    s_bcName = nullptr;
    CHECK((R(L, g + "b.AuthorName") == V{ "" }));
    s_bcPlayer = false;
    CHECK((R(L, g + "b.AuthorName") == V{ "未知" }));
    E odd = Build("t2");
    odd.category = 14;
    odd.roles = 3;
    odd.stat = 6;
    odd.difficulty = 5;
    Deliver631(odd);
    CHECK((R(L, "local b = C_BuildCreator.GetBuild('t2') return b.Category, b.Roles, b.PrimaryStat, b.DifficultyRating, b.LegendaryEnchant") ==
        V{ "UNEXPECTED_ENUM_VALUE_14", "UNEXPECTED_ENUM_VALUE_3", "UNEXPECTED_ENUM_VALUE_6", "UNEXPECTED_ENUM_VALUE_5", "nil" }));
    CHECK((R(L, "return C_BuildCreator.GetBuild('none')") == V{ "nil" }));
    Stop(L);
}

// The pending entry's setters: names, enums (an unknown category is no value, an unknown role 0, an unknown stat /
// difficulty 0); EditBuild copies a known build or starts a fresh one of the player's class.
COA_TEST(BuildCreator_EditorSetters) {
    lua_State* L = Start();
    R(L, "C_BuildEditor.SetName('N') C_BuildEditor.SetIcon('I') C_BuildEditor.SetDescription('D')");
    R(L, "C_BuildEditor.SetCategory('Level70PvP') C_BuildEditor.SetRoles('PLAYER_ROLE_TANK') C_BuildEditor.SetPrimaryStat('STAT_SPIRIT')");
    R(L, "C_BuildEditor.SetDifficultyRating('BUILD_DIFFICULTY_RATING_HARD')");
    const std::string g = "local b = C_BuildEditor.GetPendingBuild() return b.Name, b.Icon, b.Description, b.Category, b.Roles, b.PrimaryStat, b.DifficultyRating";
    CHECK((R(L, g) == V{ "N", "I", "D", "Level70PvP", "PLAYER_ROLE_TANK", "STAT_SPIRIT", "BUILD_DIFFICULTY_RATING_HARD" }));
    R(L, "C_BuildEditor.SetCategory('x') C_BuildEditor.SetRoles('x') C_BuildEditor.SetPrimaryStat('x') C_BuildEditor.SetDifficultyRating('x')");
    CHECK((R(L, g) == V{ "N", "I", "D", "UNEXPECTED_ENUM_VALUE_4294967295", "PLAYER_ROLE_NONE", "STAT_STRENGTH", "BUILD_DIFFICULTY_RATING_EASY" }));
    CHECK((R(L, "return C_BuildEditor.SetName()") == V{}));
    E e = Build("e1");
    e.name = "Known";
    e.cls = 7;
    Deliver631(e);
    CoATest::RecordPackets();
    R(L, "C_BuildEditor.EditBuild('e1') C_BuildEditor.SetName('Edited') C_BuildEditor.PublishBuild()");
    E sent = e;
    sent.name = "Edited";
    Bytes body;
    Put(body, sent);
    CHECK(Sent() == Expected(0x628, body));   // a pending entry with an id is saved
    CHECK((R(L, "return C_BuildCreator.GetBuild('e1').Name") == V{ "Known" }));   // a copy
    CAFixture::s_class = 4;
    R(L, "C_BuildEditor.EditBuild('nope')");
    E fresh;
    fresh.cls = 4;
    fresh.author = "";
    fresh.name = "";
    fresh.description = "";
    fresh.icon = "";
    Bytes freshBody;
    Put(freshBody, fresh);
    CHECK((R(L, "return C_BuildEditor.PublishBuild()") == V{ "true" }));
    CHECK(Sent() == Expected(0x626, freshBody));   // no id: created; class stamped
    R(L, "C_BuildEditor.SetName('x') C_BuildEditor.DiscardPendingBuild()");
    CHECK(Sent() == Expected(0x626, freshBody));
    R(L, "C_BuildEditor.PublishBuild()");
    CHECK(Sent() == Expected(0x626, freshBody));
    s_bcPlayer = false;
    R(L, "C_BuildEditor.DiscardPendingBuild() C_BuildEditor.PublishBuild()");
    fresh.cls = 0;
    Bytes noClass;
    Put(noClass, fresh);
    CHECK(Sent() == Expected(0x626, noClass));
    Stop(L);
}

// AddSpell / CanAddSpell (FUN_100fac40): BAD_SPELL without a row, ALREADY_EXISTS, BAD_RANK unless it is the next rank of
// its chain, LEVEL_TOO_HIGH above the server's max, LEVEL_TOO_LOW below the row's level; the mutator reports the first.
COA_TEST(BuildCreator_AddSpell) {
    lua_State* L = Start();
    CHECK((R(L, "return C_BuildEditor.AddSpell(" + SpellArg(5000, 10, ", IsCoreAbility = true, Flags = 3, Comment = 'c'") + ")") == V{ "true", "nil" }));
    CHECK((R(L, "local s = C_BuildEditor.GetSpellByID(5000) return s.Level, s.IsCoreAbility, s.Flags, s.Comment") == V{ "10", "true", "3", "c" }));
    CHECK((R(L, "return C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ")") == V{ "false", "ADD_SPELL_ALREADY_EXISTS" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanAddSpell(" + SpellArg(5000, 10) + ") return ok, t[1], t[2], t[3]") ==
        V{ "false", "ADD_SPELL_ALREADY_EXISTS", "ADD_SPELL_BAD_RANK", "nil" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanAddSpell(" + SpellArg(9999, 61) + ") return ok, t[1], t[2], t[3]") ==
        V{ "false", "ADD_SPELL_BAD_SPELL", "ADD_SPELL_LEVEL_TOO_HIGH", "nil" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanAddSpell(" + SpellArg(6001, 10) + ") return ok, t[1], t[2]") == V{ "false", "ADD_SPELL_BAD_RANK", "nil" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanAddSpell(" + SpellArg(5200, 19) + ") return ok, t[1], t[2]") == V{ "false", "ADD_SPELL_LEVEL_TOO_LOW", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddSpell(" + SpellArg(5200, 20) + ")") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddSpell(" + SpellArg(5001, 60) + ")") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.AddSpell(" + SpellArg(5001, 5) + ")") == V{ "true", "nil" }));
    CHECK((Spells(L) == V{ "5001@5", "5000@10" }));   // sorted by level
    CHECK((R(L, "return C_BuildEditor.AddSpell(5000)") == V{}));
    CHECK((R(L, "return C_BuildEditor.CanAddSpell()") == V{}));
    CHECK((R(L, "return C_BuildEditor.AddSpell({ Spell = 5100 })") == V{ "true", "nil" }));   // a missing Level reads as 0
    CHECK((R(L, "return C_BuildEditor.DoesBuildHaveSpellID(5100), C_BuildEditor.DoesBuildHaveSpellID(5200), C_BuildEditor.GetSpellByID(5200)") ==
        V{ "true", "false", "nil" }));
    Stop(L);
}

// RemoveSpell / CanRemoveSpell (FUN_100fb9b0): DOESNT_EXIST, BAD_RANK unless it is the chain's highest rank present;
// CanRemoveSpell answers like the mutator (one name).
COA_TEST(BuildCreator_RemoveSpell) {
    lua_State* L = Start();
    R(L, "C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ") C_BuildEditor.AddSpell(" + SpellArg(5001, 20) + ")");
    CHECK((R(L, "return C_BuildEditor.CanRemoveSpell(5100)") == V{ "false", "REMOVE_SPELL_DOESNT_EXIST" }));
    CHECK((R(L, "return C_BuildEditor.CanRemoveSpell(5000)") == V{ "false", "REMOVE_SPELL_BAD_RANK" }));
    CHECK((R(L, "return C_BuildEditor.RemoveSpell(5000)") == V{ "false", "REMOVE_SPELL_BAD_RANK" }));
    CHECK((R(L, "return C_BuildEditor.CanRemoveSpell(5001)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.RemoveSpell(5001)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.RemoveSpell(5000)") == V{ "true", "nil" }));
    CHECK((Spells(L) == V{}));
    CHECK((R(L, "return C_BuildEditor.RemoveSpell('x')") == V{}));
    Stop(L);
}

// SetSpellLevel / CanSetSpellLevel (FUN_100fbd20): SAME, DOESNT_EXIST, BAD_SPELL, TOO_LOW (the row's level), TOO_HIGH,
// and BAD_RANK when a lower rank would sit above it or a higher rank below it.
COA_TEST(BuildCreator_SetSpellLevel) {
    lua_State* L = Start();
    R(L, "C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ") C_BuildEditor.AddSpell(" + SpellArg(5001, 20) + ") C_BuildEditor.AddSpell(" +
        SpellArg(5200, 30) + ")");
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5000, 10)") == V{ "false", "SET_SPELL_LEVEL_SAME" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetSpellLevel(9999, 61) return ok, t[1], t[2], t[3], t[4]") ==
        V{ "false", "SET_SPELL_LEVEL_DOESNT_EXIST", "SET_SPELL_LEVEL_BAD_SPELL", "SET_SPELL_LEVEL_TOO_HIGH", "nil" }));
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5000, 25)") == V{ "false", "SET_SPELL_LEVEL_BAD_RANK" }));   // above rank 2's 20
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5001, 5)") == V{ "false", "SET_SPELL_LEVEL_BAD_RANK" }));    // below rank 1's 10
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5200, 19)") == V{ "false", "SET_SPELL_LEVEL_TOO_LOW" }));
    CHECK((R(L, "return C_BuildEditor.CanSetSpellLevel(5000, 20)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5000, 20)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.GetSpellByID(5000).Level") == V{ "20" }));
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5001, 60)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.SetSpellLevel(5001)") == V{}));
    Stop(L);
}

// The per-chain setters: flags and the ability kinds on every rank of the chain (setting one kind clears the others),
// their Can* checks (ALREADY_SET / DOESNT_EXIST), comments (an enchant id first, else the spell's chain; or a type name),
// the comment length checks and the always-ok Can* natives.
COA_TEST(BuildCreator_SpellChainSetters) {
    lua_State* L = Start();
    R(L, "C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ") C_BuildEditor.AddSpell(" + SpellArg(5001, 20) + ") C_BuildEditor.AddSpell(" +
        SpellArg(5100, 30) + ")");
    R(L, "C_BuildEditor.SetSpellFlags(5001, 9)");
    CHECK((R(L, "return C_BuildEditor.GetSpellByID(5000).Flags, C_BuildEditor.GetSpellByID(5001).Flags, C_BuildEditor.GetSpellByID(5100).Flags") ==
        V{ "9", "9", "0" }));
    R(L, "C_BuildEditor.SetIsOptimalAbility(5000, true) C_BuildEditor.SetIsCoreAbility(5001, true)");
    const std::string kinds = "local s = C_BuildEditor.GetSpellByID(5000) return s.IsCoreAbility, s.IsOptimalAbility, s.IsEmpoweringAbility, s.IsSynergisticAbility";
    CHECK((R(L, kinds) == V{ "true", "false", "false", "false" }));
    R(L, "C_BuildEditor.SetIsCoreAbility(5000, false) C_BuildEditor.SetIsSynergisticAbility(5000, true)");
    CHECK((R(L, kinds) == V{ "false", "false", "false", "true" }));
    R(L, "C_BuildEditor.SetIsEmpoweringAbility(5001, true) C_BuildEditor.SetIsSynergisticAbility(5001, false)");
    CHECK((R(L, kinds) == V{ "false", "false", "true", "false" }));
    CHECK((R(L, "return C_BuildEditor.CanSetIsEmpoweringAbility(5000, true)") == V{ "false", "table" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetIsEmpoweringAbility(5000, true) return t[1]") == V{ "SET_IS_EMPOWERING_ABILITY_ALREADY_SET" }));
    CHECK((R(L, "return C_BuildEditor.CanSetIsEmpoweringAbility(5000, false)") == V{ "true", "nil" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetIsCoreAbility(4242, true) return ok, t[1]") == V{ "false", "SET_IS_CORE_ABILITY_DOESNT_EXIST" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetIsOptimalAbility(5000, false) return ok, t[1]") == V{ "false", "SET_IS_OPTIMAL_ABILITY_ALREADY_SET" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetIsSynergisticAbility(5000, nil) return ok, t[1]") == V{ "false", "SET_IS_SYNERGISTIC_ABILITY_ALREADY_SET" }));
    CHECK((R(L, "return C_BuildEditor.CanSetIsSynergisticAbility(5000)") == V{}));   // the boolean is required
    CHECK((R(L, "return C_BuildEditor.SetIsCoreAbility(5000, 'x')") == V{}));
    R(L, "C_BuildEditor.AddRandomEnchant({ Enchant = 5100 })");   // an enchant sharing the spell's number
    R(L, "C_BuildEditor.SetComment(5100, 'enchant') C_BuildEditor.SetComment(5001, 'chain')");
    CHECK((R(L, "return C_BuildEditor.GetSpellByID(5100).Comment, C_BuildEditor.GetPendingBuild().RandomEnchants[1].Comment, "
                "C_BuildEditor.GetSpellByID(5000).Comment") == V{ "", "enchant", "chain" }));
    R(L, "C_BuildEditor.AddWeaponType({ Type = 'ITEM_SUBCLASS_WEAPON_GUN' }) C_BuildEditor.AddArmorType({ Type = 'ITEM_SUBCLASS_ARMOR_MAIL' })");
    R(L, "C_BuildEditor.SetComment('ITEM_SUBCLASS_WEAPON_GUN', 'gun') C_BuildEditor.SetComment('ITEM_SUBCLASS_ARMOR_MAIL', 'mail') "
         "C_BuildEditor.SetComment('ITEM_SUBCLASS_ARMOR_PLATE', 'none')");
    CHECK((R(L, "local b = C_BuildEditor.GetPendingBuild() return b.WeaponTypes[1].Comment, b.ArmorTypes[1].Comment, #b.ArmorTypes") == V{ "gun", "mail", "1" }));
    const std::string longText(0x81, 'x');
    CHECK((R(L, "return C_BuildEditor.CanSetSpellComment(1, '" + std::string(0x80, 'x') + "')") == V{ "true", "nil" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetSpellComment(1, '" + longText + "') return ok, t[1]") == V{ "false", "SET_SPELL_COMMENT_TOO_LONG" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetRandomEnchantComment(1, '" + longText + "') return ok, t[1]") == V{ "false", "SET_RANDOM_ENCHANT_COMMENT_TOO_LONG" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetWeaponTypeComment('x', '" + longText + "') return ok, t[1]") == V{ "false", "SET_WEAPON_TYPE_COMMENT_TOO_LONG" }));
    CHECK((R(L, "local ok, t = C_BuildEditor.CanSetArmorTypeComment('ITEM_SUBCLASS_ARMOR_MAIL', '" + longText + "') return ok, t[1]") ==
        V{ "false", "SET_ARMOR_TYPE_COMMENT_TOO_LONG" }));
    CHECK((R(L, "return C_BuildEditor.CanSetArmorTypeComment('a', 'b')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanSetSpellComment('a', 'b')") == V{}));
    CHECK((R(L, "return C_BuildEditor.CanSetSpellFlags(1, 2)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanSetEnchantLevel(1, 2)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanSetEnchantFlags(1, 0)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanSetRandomEnchantStacks(1, 2)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanSetEnchantFlags(1)") == V{}));
    Stop(L);
}

// Random enchants and weapon / armour types: added (or replaced by id / type) and sorted, removed, their fields set; a
// missing Type is the default (weapon 0x15, armour 0xB); every native but SetEnchantStacks answers (true, nil).
COA_TEST(BuildCreator_EnchantsAndTypes) {
    lua_State* L = Start();
    CHECK((R(L, "return C_BuildEditor.AddRandomEnchant({ Enchant = 1001, Stacks = 2, Level = 9, Flags = 1, Comment = 'a' })") == V{ "true", "nil" }));
    R(L, "C_BuildEditor.AddRandomEnchant({ Enchant = 1002, Level = 3 }) C_BuildEditor.AddRandomEnchant({ Enchant = 1001, Stacks = 4, Level = 1 })");
    const std::string list = "local b = C_BuildEditor.GetPendingBuild() local t = {} for i, c in ipairs(b.RandomEnchants) do "
                             "t[#t + 1] = c.Enchant .. ':' .. c.Stacks .. ':' .. c.Level .. ':' .. c.Flags .. ':' .. c.Comment end return unpack(t)";
    CHECK((R(L, list) == V{ "1001:4:1:0:", "1002:0:3:0:" }));
    CHECK((R(L, "return C_BuildEditor.DoesBuildHaveEnchant(1002), C_BuildEditor.DoesBuildHaveEnchant(1003)") == V{ "true", "false" }));
    CHECK((R(L, "return C_BuildEditor.SetEnchantLevel(1001, 7)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.SetEnchantFlags(1002, 5)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.SetEnchantStacks(1002, 8)") == V{}));
    CHECK((R(L, list) == V{ "1001:4:7:0:", "1002:8:3:5:" }));   // not re-sorted by the field setters
    CHECK((R(L, "return C_BuildEditor.RemoveRandomEnchant(1001)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.RemoveRandomEnchant(4444)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanRemoveRandomEnchant(1)") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddRandomEnchant({})") == V{ "true", "nil" }));
    CHECK((R(L, list) == V{ "1002:8:3:5:" }));
    CHECK((R(L, "return C_BuildEditor.AddRandomEnchant(1)") == V{}));
    R(L, "C_BuildEditor.AddWeaponType({ Type = 'ITEM_SUBCLASS_WEAPON_STAFF', Comment = 's' }) C_BuildEditor.AddWeaponType({ Comment = 'default' }) "
         "C_BuildEditor.AddWeaponType({ Type = 'ITEM_SUBCLASS_WEAPON_AXE' }) C_BuildEditor.AddWeaponType({ Type = 'bogus' })");
    const std::string weapons = "local b = C_BuildEditor.GetPendingBuild() local t = {} for i, w in ipairs(b.WeaponTypes) do t[#t + 1] = w.Type .. '/' .. w.Comment end "
                                "return unpack(t)";
    CHECK((R(L, weapons) == V{ "ITEM_SUBCLASS_WEAPON_AXE/", "ITEM_SUBCLASS_WEAPON_STAFF/s", "ITEM_SUBCLASS_WEAPON_MAX/" }));   // 'bogus' kept 0x15
    CHECK((R(L, "return C_BuildEditor.RemoveWeaponType('ITEM_SUBCLASS_WEAPON_STAFF')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.RemoveWeaponType('bogus')") == V{ "true", "nil" }));   // no value: nothing removed
    CHECK((R(L, "return C_BuildEditor.CanRemoveWeaponType('x')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddWeaponType({})") == V{ "true", "nil" }));
    CHECK((R(L, weapons) == V{ "ITEM_SUBCLASS_WEAPON_AXE/", "ITEM_SUBCLASS_WEAPON_MAX/" }));
    CHECK((R(L, "return C_BuildEditor.RemoveWeaponType('ITEM_SUBCLASS_WEAPON_MAX')") == V{ "true", "nil" }));
    CHECK((R(L, weapons) == V{ "ITEM_SUBCLASS_WEAPON_AXE/" }));
    R(L, "C_BuildEditor.AddArmorType({}) C_BuildEditor.AddArmorType({ Type = 'ITEM_SUBCLASS_ARMOR_CLOTH' })");
    CHECK((R(L, "local b = C_BuildEditor.GetPendingBuild() return b.ArmorTypes[1].Type, b.ArmorTypes[2].Type") ==
        V{ "ITEM_SUBCLASS_ARMOR_CLOTH", "ITEM_SUBCLASS_ARMOR_MAX" }));
    CHECK((R(L, "return C_BuildEditor.RemoveArmorType('ITEM_SUBCLASS_ARMOR_MAX')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanRemoveArmorType('a')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddArmorType({})") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildEditor.CanAddArmorType()") == V{}));
    CHECK((R(L, "return #C_BuildEditor.GetPendingBuild().ArmorTypes") == V{ "1" }));
    Stop(L);
}

// CanPublishBuild (FUN_100fb090): the checks in their order. (The fixture's level-60 essence row lets a build pass.)
COA_TEST(BuildCreator_CanPublish) {
    lua_State* L = Start();
    auto code = [&]() {
        const V v = R(L, "local ok, t = C_BuildEditor.CanPublishBuild() if ok then return 'OK' end return t[1]");
        return v.empty() ? std::string() : v[0];
    };
    auto rep = [](char c, size_t n) { return "'" + std::string(n, c) + "'"; };
    // The glue-reset pending entry has class 0, which no build of it traverses; a discarded one has the player's.
    R(L, "C_BuildEditor.DiscardPendingBuild() C_BuildEditor.SetCategory('BuildDraft')");
    CHECK(code() == "PUBLISH_BUILD_BAD_CATEGORY");
    R(L, "C_BuildEditor.SetCategory('BuildDraftEndGamePvE')");
    CHECK(code() == "PUBLISH_BUILD_BAD_CATEGORY");
    R(L, "C_BuildEditor.SetCategory('BuildDraftEndGamePvP')");
    CHECK(code() == "PUBLISH_BUILD_BAD_CATEGORY");
    s_bcPlayer = false;
    CHECK(code() == "PUBLISH_BUILD_NO_SPELLS");
    s_bcPlayer = true;
    Gm(2);
    CHECK(code() == "PUBLISH_BUILD_NO_SPELLS");
    Gm(0);
    R(L, "C_BuildEditor.SetCategory('None') C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ")");
    CHECK(code() == "PUBLISH_BUILD_NO_WEAPON_TYPES");
    R(L, "C_BuildEditor.AddWeaponType({})");
    CHECK(code() == "PUBLISH_BUILD_NO_ARMOR_TYPES");
    R(L, "C_BuildEditor.AddArmorType({})");
    CHECK(code() == "PUBLISH_BUILD_NO_NAME");
    R(L, "C_BuildEditor.SetName('Build Name')");
    CHECK(code() == "PUBLISH_BUILD_DEFAULT_NAME");
    R(L, "C_BuildEditor.SetName(" + rep('n', 0x81) + ")");
    CHECK(code() == "PUBLISH_BUILD_NAME_TOO_LONG");
    R(L, "C_BuildEditor.SetName(" + rep('n', 0x80) + ")");
    CHECK(code() == "PUBLISH_BUILD_NO_ICON");
    R(L, "C_BuildEditor.SetIcon('inv_misc_questionmark')");
    CHECK(code() == "PUBLISH_BUILD_DEFAULT_ICON");
    R(L, "C_BuildEditor.SetIcon(" + rep('i', 0x81) + ")");
    CHECK(code() == "PUBLISH_BUILD_ICON_TOO_LONG");
    R(L, "C_BuildEditor.SetIcon('icon')");
    CHECK(code() == "PUBLISH_BUILD_NO_DESCRIPTION");
    R(L, "C_BuildEditor.SetDescription('There is no description yet.')");
    CHECK(code() == "PUBLISH_BUILD_DEFAULT_DESCRIPTION");
    R(L, "C_BuildEditor.SetDescription(" + rep('d', 0x4001) + ")");
    CHECK(code() == "PUBLISH_BUILD_DESCRIPTION_TOO_LONG");
    R(L, "C_BuildEditor.SetDescription(" + rep('d', 0x4000) + ")");
    CHECK(code() == "OK");
    R(L, "C_BuildEditor.SetComment(5000, " + rep('c', 0x81) + ")");
    CHECK(code() == "PUBLISH_BUILD_SPELL_COMMENT_TOO_LONG");
    R(L, "C_BuildEditor.SetComment(5000, " + rep('c', 0x80) + ") C_BuildEditor.SetIsCoreAbility(5000, true)");
    CHECK(code() == "OK");
    R(L, "C_BuildEditor.AddRandomEnchant({ Enchant = 1001, Comment = " + rep('c', 0x81) + " })");
    CHECK(code() == "PUBLISH_BUILD_RANDOM_ENCHANT_COMMENT_TOO_LONG");
    R(L, "C_BuildEditor.RemoveRandomEnchant(1001) C_BuildEditor.AddWeaponType({ Comment = " + rep('c', 0x81) + " })");
    CHECK(code() == "PUBLISH_BUILD_WEAPON_TYPE_COMMENT_TOO_LONG");
    R(L, "C_BuildEditor.AddWeaponType({}) C_BuildEditor.AddArmorType({ Comment = " + rep('c', 0x81) + " })");
    CHECK(code() == "PUBLISH_BUILD_ARMOR_TYPE_COMMENT_TOO_LONG");
    R(L, "C_BuildEditor.AddArmorType({})");
    CHECK(code() == "OK");
    // Each level up to the highest spell level must traverse: 5200 (level 20 row) placed at 20 does, an unknown spell
    // never adds a rank; a spell below its row's level is refused by AddSpell, so the per-level pass is reached through
    // a received build.
    E bad = Build("lv", { { 5200, 15 } });
    bad.weapons = { { 1 } };
    bad.armor = { { 1 } };
    Deliver631(bad);
    R(L, "C_BuildEditor.EditBuild('lv')");
    CHECK(code() == "PUBLISH_BUILD_LEVEL_INAPPROPRIATE_SPELL");
    // A saved build (pending id set) must be known and of the player's class.
    Deliver631(Build("mine"));
    R(L, "C_BuildEditor.EditBuild('mine')");
    R(L, "C_BuildEditor.AddWeaponType({}) C_BuildEditor.AddArmorType({})");
    CHECK(code() == "OK");
    CAFixture::s_class = 2;
    CHECK(code() == "PUBLISH_BUILD_CLASS_MISMATCH");
    s_bcPlayer = false;
    CHECK(code() == "OK");
    s_bcPlayer = true;
    CAFixture::s_class = 1;
    Deliver631(Build("mine", {}));   // erased
    CHECK(code() == "PUBLISH_BUILD_NOT_FOUND");
    // A new build whose (empty) id is already known.
    R(L, "C_BuildEditor.DiscardPendingBuild() C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ") C_BuildEditor.AddWeaponType({}) "
         "C_BuildEditor.AddArmorType({}) C_BuildEditor.SetName('n') C_BuildEditor.SetIcon('i') C_BuildEditor.SetDescription('d')");
    CHECK(code() == "OK");
    Deliver631(Build(""));
    CHECK(code() == "PUBLISH_BUILD_ALREADY_EXISTS");
    Deliver631(Build("", {}));
    CHECK(code() == "OK");
    // The essence minimums of the 60 (30) / 70 (35) categories.
    R(L, "C_BuildEditor.SetCategory('Level60PvP')");
    CHECK(code() == "PUBLISH_BUILD_NOT_ENOUGH_AE_INVESTMENT");
    R(L, "C_BuildEditor.SetCategory('Level70PvPvE')");
    CHECK(code() == "PUBLISH_BUILD_NOT_ENOUGH_AE_INVESTMENT");
    R(L, "C_BuildEditor.SetCategory('Archived')");
    CHECK(code() == "OK");
    R(L, "C_BuildEditor.SetIsCoreAbility(5000, true) C_BuildEditor.SetIsEmpoweringAbility(5000, true)");
    CHECK(code() == "OK");   // setting one kind clears the others
    E multi = Build("", { { 5000, 10, 1, 1 } });
    multi.weapons = { { 1 } };
    multi.armor = { { 1 } };
    Deliver631(multi);
    R(L, "C_BuildEditor.EditBuild('')");
    CHECK(code() == "PUBLISH_BUILD_ALREADY_EXISTS");
    Stop(L);
}

// The server round-trips' CMSGs and the getters that answer from the lists.
COA_TEST(BuildCreator_RoundTrips) {
    lua_State* L = Start();
    CoATest::RecordPackets();
    CHECK((R(L, "return C_BuildCreator.ActivateBuild('a1', true, false)") == V{ "true" }));
    CHECK(Sent() == Expected(0x5C2, Bytes().Str("a1").U8(1).U8(0)));
    CHECK((R(L, "return C_BuildCreator.ActivateBuild('a1', nil, true)") == V{ "true" }));
    CHECK(Sent() == Expected(0x5C2, Bytes().Str("a1").U8(0).U8(1)));
    CHECK((R(L, "return C_BuildCreator.ActivateBuild('a1', 'x', true)") == V{}));
    CHECK((R(L, "return C_BuildCreator.DeactivateBuild('a1')") == V{ "true" }));
    CHECK(Sent() == Expected(0x5C4, Bytes().Str("a1")));
    CHECK((R(L, "return C_BuildCreator.QueryAllBuilds('Leveling')") == V{ "true" }));
    CHECK(Sent() == Expected(0x62E, Bytes().Str("Leveling")));
    CHECK((R(L, "return C_BuildCreator.QueryBuild('q9')") == V{ "true" }));
    CHECK(Sent() == Expected(0x630, Bytes().Str("q9")));
    const size_t before = SentPackets().size();
    CHECK((R(L, "return C_BuildCreator.QueryBuild()") == V{}));
    CHECK((R(L, "return C_BuildCreator.DeleteBuild()") == V{}));
    CHECK((R(L, "return C_BuildCreator.DeactivateBuild()") == V{}));
    CHECK((R(L, "return C_BuildCreator.QueryAllBuilds()") == V{}));
    CHECK(SentPackets().size() == before);
    CHECK((R(L, "return C_BuildCreator.CanActivateBuild('a1')") == V{ "false", "nil" }));
    CHECK((R(L, "return C_BuildCreator.CanDeactivateBuild('a1')") == V{ "false", "nil" }));
    CHECK((R(L, "return C_BuildCreator.CanActivateBuild()") == V{}));
    Deliver631(Build("a1"));
    CHECK((R(L, "return C_BuildCreator.CanActivateBuild('a1')") == V{ "true", "nil" }));
    DeliverPage({ Build("pg") });
    CHECK((R(L, "return C_BuildCreator.CanDeactivateBuild('pg')") == V{ "true", "nil" }));
    CHECK((R(L, "return C_BuildCreator.GetSpell('a1', 5000).Level, C_BuildCreator.GetSpell('a1', 5001), C_BuildCreator.GetSpell('zz', 5000)") ==
        V{ "10", "nil", "nil" }));
    CHECK((R(L, "return C_BuildCreator.GetSpell('a1')") == V{}));
    CHECK((R(L, "return C_BuildCreator.GetBookmarkedBuildAtIndex(2), C_BuildCreator.GetBookmarkedBuildAtIndex(0)") == V{ "" }));
    Stop(L);
}

// UpdateFilter (FUN_100fca40): text in name / description / author (case-insensitive), argument groups (OR within, AND
// across) and sort selections in the map's order. Filters and sorts are name-keyed (0x10B1F780 / 0x10B1E854).
COA_TEST(BuildCreator_UpdateFilter) {
    lua_State* L = Start();
    E a = Build("a");
    a.name = "Fire Mage";
    a.upvotes = 5;
    a.updated = 100;
    a.difficulty = 0;
    a.roles = 4;
    a.cls = 8;
    a.stat = 3;
    E b = Build("b");
    b.description = "a FIRE build";
    b.upvotes = 1;
    b.updated = static_cast<uint64_t>(s_now - 10);
    b.difficulty = 2;
    b.roles = 2;
    b.flags = 1;
    b.cls = 1;
    E c = Build("c");
    c.author = "Ascension";
    c.upvotes = 9;
    c.updated = 50;
    c.difficulty = 4;
    c.roles = 8;
    c.cls = 1;
    c.stat = 1;
    DeliverPage({ a, b, c });
    Feed(0x634, Ids({ "c" }));
    CHECK((Filtered(L) == V{ "a", "b", "c" }));
    CHECK((Filtered(L, "{}", "{}", "FIRE") == V{ "a", "b" }));
    CHECK((Filtered(L, "{}", "{}", "ascen") == V{ "c" }));
    CHECK((Filtered(L, "{ FILTER_FEATURED = true }") == V{ "b" }));                                  // flag 1
    CHECK((Filtered(L, "{ FILTER_DIFFICULTY_RATING_EASY = true, FILTER_DIFFICULTY_RATING_IMPOSSIBLE = true }") == V{ "a", "c" }));
    CHECK((Filtered(L, "{ FILTER_DIFFICULTY_RATING_EASY = true, FILTER_DIFFICULTY_RATING_IMPOSSIBLE = false }") == V{ "a" }));
    CHECK((Filtered(L, "{ FILTER_OWNED_BUILDS = true }") == V{ "c" }));
    CHECK((Filtered(L, "{ FILTER_PRIMARY_STAT_AGILITY = true, FILTER_PRIMARY_STAT_INTELLECT = true }") == V{ "a", "c" }));
    CHECK((Filtered(L, "{ FILTER_PRIMARY_STAT_STRENGTH = true }") == V{ "b" }));
    CHECK((Filtered(L, "{ FILTER_ROLE_TANK = true }") == V{ "b" }));
    CHECK((Filtered(L, "{ FILTER_ROLE_HEALER = true, FILTER_ROLE_DAMAGE = true }") == V{ "a", "c" }));
    CHECK((Filtered(L, "{ FILTER_CLASS_WARRIOR = true }") == V{ "b", "c" }));                       // class 1
    CHECK((Filtered(L, "{ FILTER_CLASS_MAGE = true, FILTER_CLASS_WARRIOR = true }") == V{ "a", "b", "c" }));
    CHECK((Filtered(L, "{ FILTER_CLASS_WARRIOR = true, FILTER_ROLE_TANK = true }") == V{ "b" }));   // AND across groups
    CHECK((Filtered(L, "{ FILTER_LAST_30_DAYS = true }") == V{ "b", "c" }));    // Ascension's, or updated in the last 30 days
    CHECK((Filtered(L, "{ FILTER_FEATURED = 1 }") == V{ "b" }));                // a number value is a boolean
    CHECK((Filtered(L, "{ FILTER_FEATURED = true, FILTER_ROLE_TANK = nil }") == V{ "b" }));
    CHECK((Filtered(L, "{ NOT_A_FILTER = true }") == Filtered(L, "{ FILTER_NONE = true }")));  // unknown: logged, FILTER_NONE
    CHECK((Filtered(L, "{ FILTER_FEATURED = {} }") == V{ "a", "b", "c" }));     // a table value: an empty map
    CHECK((Filtered(L, "{}", "{ SORT_RATING_ASCENDING = true }") == V{ "b", "a", "c" }));   // upvotes ascending
    CHECK((Filtered(L, "{}", "{ SORT_RATING_DESCENDING = true }") == V{ "c", "a", "b" }));  // descending
    CHECK((Filtered(L, "{}", "{ SORT_DATE_ASCENDING = true }") == V{ "c", "a", "b" }));     // updated ascending
    CHECK((Filtered(L, "{}", "{ SORT_DATE_DESCENDING = true }") == V{ "b", "a", "c" }));    // descending
    R(L, "C_BuildCreator.UpdateFilter('x', {}, {})");
    CHECK((R(L, "return C_BuildCreator.UpdateFilter('a', {})") == V{}));      // not applied
    CHECK((R(L, "return C_BuildCreator.GetNumBuilds(), C_BuildCreator.GetBuildAtIndex(1), C_BuildCreator.GetBuildAtIndex(0)") == V{ "0", "nil" }));
    Stop(L);
}

// BookmarkBuild (FUN_100fa7f0): a queried build moves to the front; one only on the page is copied to the front.
COA_TEST(BuildCreator_Bookmark) {
    lua_State* L = Start();
    Deliver631(Build("q1"));
    Deliver631(Build("q2"));
    DeliverPage({ Build("p1") });
    R(L, "C_BuildCreator.BookmarkBuild('q2')");
    CHECK((Bookmarks(L) == V{ "q2", "q1" }));
    R(L, "C_BuildCreator.BookmarkBuild('p1')");
    CHECK((Bookmarks(L) == V{ "p1", "q2", "q1" }));
    R(L, "C_BuildCreator.BookmarkBuild('zz')");
    CHECK((Bookmarks(L) == V{ "p1", "q2", "q1" }));
    CHECK((R(L, "return C_BuildCreator.BookmarkBuild('q1')") == V{}));
    CHECK((Bookmarks(L) == V{ "q1", "p1", "q2" }));
    Stop(L);
}

// GetEssenceForLevel / GetQualityInfo / GetQualityInfoForLevel: builds of the pending entry at a level (the server's max
// without one).
COA_TEST(BuildCreator_EssenceAndQuality) {
    lua_State* L = Start();
    R(L, "C_BuildEditor.DiscardPendingBuild() C_BuildEditor.AddSpell(" + SpellArg(5000, 10) + ") C_BuildEditor.AddSpell(" + SpellArg(5001, 20) + ") C_BuildEditor.AddSpell(" +
        SpellArg(5100, 30) + ")");
    CoA::CA::BuildSeed seed;
    seed.classKey = 1;
    seed.spells = { { 5000, 10 }, { 5001, 20 }, { 5100, 30 } };
    auto essence = [&](uint32_t level) {
        const CoA::CA::Build b(seed, level, false);
        return V{ std::to_string(b.GlobalAE(0)), std::to_string(b.RemainingAE()), std::to_string(b.GlobalTE(0)), std::to_string(b.RemainingTE()) };
    };
    CHECK((R(L, "return C_BuildEditor.GetEssenceForLevel(15)") == essence(15)));
    CHECK((R(L, "return C_BuildEditor.GetEssenceForLevel()") == essence(60)));
    CHECK((R(L, "return C_BuildEditor.GetEssenceForLevel(30)") == essence(30)));
    CHECK(essence(15)[0] == "2" && essence(30)[0] == "4" && essence(30)[2] == "1");
    const CoA::CA::Build full(seed, 60, true);
    const CoA::CA::Row r = CoA::CA::RowBySpell(5000);
    const int q = CoA::CA::QualityIndexOf(full, r);
    CHECK((R(L, "return C_BuildEditor.GetQualityInfo(5000)") == V{ CoA::CA::QualityName(q), std::to_string(CoA::CA::QualityCostOf(full, r)) }));
    CHECK((R(L, "return C_BuildEditor.GetQualityInfo(5000, 60)") == R(L, "return C_BuildEditor.GetQualityInfo(5000)")));
    CHECK((R(L, "return C_BuildEditor.GetQualityInfo(9999)") == V{ "nil" }));
    CHECK((R(L, "return C_BuildEditor.GetQualityInfo('x')") == V{}));
    const CoA::CA::Build at30(seed, 30, false);
    V sums;

    for (int k = 1; k <= 5; ++k) {
        sums.push_back(std::to_string(CoA::CA::QualitySumOf(at30, k)));
    }

    CHECK((R(L, "return C_BuildEditor.GetQualityInfoForLevel(30)") == sums));
    CHECK(R(L, "return C_BuildEditor.GetQualityInfoForLevel()").size() == 5);
    Stop(L);
}

// The glue screen (FUN_100fc070): every list and memory emptied, a fresh pending entry WITHOUT the player's class, the
// filter reset, and the CA editor container emptied.
COA_TEST(BuildCreator_GlueReset) {
    lua_State* L = Start();
    R(L, "C_BuildCreator.QueryAllBuilds('Leveling') C_BuildCreator.RateBuild('r', true) C_BuildCreator.DeleteBuild('d')");
    Deliver631(Build("q"));
    DeliverPage({ Build("p") });
    Filtered(L);
    Feed(0x632, Bytes().U32(0).Str("s").U8(0).U8(0));
    Feed(0x634, Ids({ "o" }));
    Feed(0x635, Ids({ "u" }));
    R(L, "C_BuildEditor.EditBuild('q')");
    R(L, "C_BuildEditor.SetFilteredEntries('', { FILTER_KNOWN = true })");
    CHECK((R(L, "return C_BuildEditor.GetNumFilteredEntries()") == V{ "1" }));
    CoA::Lifecycle::RunGlueScreen();
    CHECK((R(L, "return C_BuildEditor.GetNumFilteredEntries()") == V{ "0" }));
    CHECK((Bookmarks(L) == V{}));
    CHECK((R(L, "return C_BuildCreator.GetNumBuilds(), C_BuildCreator.GetActiveBuild(1), C_BuildCreator.IsOwnedBuild('o'), C_BuildCreator.IsUpvotedBuild('u')") ==
        V{ "0", "", "false", "false" }));
    CHECK((Filtered(L) == V{}));
    CHECK(!CoA::BuildCreator::SpecHasActiveBuild(0));
    CoATest::RecordPackets();
    R(L, "C_BuildEditor.PublishBuild()");
    E fresh;
    fresh.cls = 0;
    fresh.author = "";
    fresh.name = "";
    fresh.description = "";
    fresh.icon = "";
    Bytes body;
    Put(body, fresh);
    CHECK(Sent() == Expected(0x626, body));
    // The rate / delete memory is gone: an OK result now concerns the empty id; the category is None.
    CHECK((Feed(0x62B, Bytes().Str("RATE_BUILD_OK")) == V{ "BUILD_CREATOR_RATE_RESULT:RATE_BUILD_OK,,0" }));
    CHECK((R(L, "return C_BuildCreator.IsUpvotedBuild('')") == V{ "false" }));   // rateUp was cleared: removed, not added
    CHECK((DeliverPage({}, 0, 1, "x") == V{ "BUILD_CREATOR_CATEGORY_RESULT:None,x" }));
    Stop(L);
}
