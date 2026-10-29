#include "CoA/Source/Runtime/ClientHooks.h"
#include "CoA/Tests/AppearanceFixture.h"

using namespace AppearanceFixture;

namespace {
    const std::vector<uint32_t> kNone = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // one per category index 0..9

    std::vector<uint8_t> Apply697(const std::vector<uint32_t>& pending) {
        CoATest::Bytes b;
        b.U32(static_cast<uint32_t>(pending.size()));

        for (uint32_t a : pending) {
            b.U32(a);
        }

        return CoATest::Expected(0x697, b);
    }
}

// Pending vs current: set, query, clear; an apply sends the pending list once every change validates, and the server's
// OK copies pending into current.
COA_TEST(Appearance_PendingAndApply) {
    lua_State* L = Setup();
    Current(kNone);
    Collect({ 100 });
    CHECK((Run(L, "return C_Appearance.GetAppearanceForCategory(1), C_Appearance.HasPendingAppearances()") == V{ "nil", "false" }));
    CoATest::RecordSignals();
    CHECK((Run(L, "return C_Appearance.SetPendingAppearance(1, 100)") == V{ "true" }));
    CoATest::StopRecordingSignals();
    CHECK((CoATest::Signals() == V{ "PENDING_APPEARANCE_CHANGED" }));
    CHECK((Run(L, "return C_Appearance.GetPendingAppearance(1), C_Appearance.HasPendingAppearances()") == V{ "100", "true" }));
    CHECK((Sent(L, "return C_Appearance.ApplyPendingAppearances(true)") == Packets { Apply697({ 0, 100, 0, 0, 0, 0, 0, 0, 0, 0 }) }));
    CHECK((Feed(0x698, CoATest::Bytes().Str("APPLY_APPEARANCES_OK")) == V{ "APPLY_PENDING_APPEARANCE_RESULT:true,APPLY_APPEARANCES_OK" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceForCategory(1), C_Appearance.HasPendingAppearances()") == V{ "100", "false" }));
    Run(L, "C_Appearance.SetPendingAppearance(1, 0)");
    Run(L, "C_Appearance.ClearPendingAppearance(1)");
    CHECK((Run(L, "return C_Appearance.GetPendingAppearance(1)") == V{ "nil" }));
    CHECK((Feed(0x698, CoATest::Bytes().Str("APPLY_APPEARANCES_UNKNOWN")) == V{ "APPLY_PENDING_APPEARANCE_RESULT:false,APPLY_APPEARANCES_UNKNOWN" }));
    Teardown(L);
}

// The validator's codes, in the original's order.
COA_TEST(Appearance_Validator) {
    lua_State* L = Setup();
    Current(kNone);
    auto can = [&](const char* args) { return Run(L, (std::string("return C_Appearance.CanSetAppearance(") + args + ")").c_str()); };
    CHECK((can("1, 100") == V{ "false", "APPLY_APPEARANCES_NOT_COLLECTED", "store/ascension/12/game", "store/ascension/12/checkout" }));
    Collect({ 100, 101, 102, 104, 105, 106, 107 });
    CHECK((can("1, 100") == V{ "true", "nil", "nil", "nil" }));
    CHECK((can("11, 100") == V{ "false", "APPLY_APPEARANCES_OUT_OF_RANGE", "nil", "nil" }));
    CHECK((can("1, 999") == V{ "false", "APPLY_APPEARANCES_BAD_APPEARANCE", "nil", "nil" }));
    CHECK((can("1, 106") == V{ "false", "APPLY_APPEARANCES_NOT_REALM_APPROPRIATE", "nil", "nil" }));
    CHECK((can("2, 100") == V{ "false", "APPLY_APPEARANCES_BAD_CATEGORY", "nil", "nil" }));
    CHECK((can("1, 102") == V{ "false", "APPLY_APPEARANCES_THROWN_APPEARANCE", "nil", "nil" }));
    CHECK((can("5, 107") == V{ "false", "APPLY_APPEARANCES_UNSUPPORTED_CATEGORY", "nil", "nil" }));
    CHECK((can("6, 105") == V{ "true", "nil", "nil", "nil" }));
    CHECK((can("4, 104") == V{ "false", "APPLY_APPEARANCES_PET_SUMMON_SPELL_NOT_LEARNED", "nil", "nil" }));
    s_knownSpells = { 0x10CC53 };
    CHECK((can("4, 104") == V{ "false", "APPLY_APPEARANCES_PET_DISMISS_SPELL_NOT_LEARNED", "nil", "nil" }));
    s_knownSpells = { 0x10CC53, 0x10D331 };
    CHECK((can("4, 104") == V{ "true", "nil", "nil", "nil" }));
    s_creatures.clear();
    CHECK((can("4, 104") == V{ "false", "APPLY_APPEARANCES_BAD_CREATURE_TEMPLATE", "nil", "nil" }));
    s_creatures = { 77 };
    s_class = 8;   // category 1's class mask is class 1 only
    CHECK((can("1, 100") == V{ "false", "APPLY_APPEARANCES_NOT_AVAILABLE_FOR_CLASS", "nil", "nil" }));
    s_player = false;
    CHECK((can("1, 100") == V{ "false", "APPLY_APPEARANCES_NOT_IN_WORLD", "nil", "nil" }));
    Teardown(L);
}

// A cosmetic pet needs the confirmation: unconfirmed, the apply signals the confirmation code and sends nothing.
COA_TEST(Appearance_PetNeedsConfirmation) {
    lua_State* L = Setup();
    Current(kNone);
    Collect({ 104 });
    s_knownSpells = { 0x10CC53, 0x10D331 };
    Run(L, "C_Appearance.SetPendingAppearance(4, 104)");
    CoATest::RecordSignals();
    V r;
    CHECK((Sent(L, "return C_Appearance.ApplyPendingAppearances(false)", &r).empty()));
    CoATest::StopRecordingSignals();
    CHECK((r == V{ "false" }));
    CHECK((CoATest::Signals() == V{ "APPLY_PENDING_APPEARANCE_RESULT:false,APPLY_APPEARANCES_COSMETIC_PET_APPEARANCE_CONFIRMATION" }));
    CHECK((Run(L, "return C_Appearance.CanApplyPendingAppearances()") == V{ "true", "nil" }));
    Teardown(L);
}

// An item set expands into its parts: robe (inventory type 20) counts as chest, the necklace (inventory type 2) is not a
// tracked set slot; the set is collected only when every part is.
COA_TEST(Appearance_ItemSets) {
    lua_State* L = Setup();
    Current(kNone);
    Collect({ 100 });
    CHECK((Run(L, "local t = C_ItemSet.GetAppearances(50) return #t, t[1], t[2], t[3], t[8]") == V{ "8", "100", "101", "0", "0" }));
    CHECK((Run(L, "return C_ItemSet.GetItemSetNumCollected(50)") == V{ "1", "2" }));
    CHECK((Run(L, "return C_ItemSet.GetItemSetNumCollected(51)") == V{ "nil", "nil" }));
    CHECK((Run(L, "return C_AppearanceCollection.IsAppearanceCollected(103)") == V{ "false" }));
    Collect({ 100, 101 });
    CHECK((Run(L, "return C_AppearanceCollection.IsAppearanceCollected(103)") == V{ "true" }));
    CHECK((Run(L, "return C_Appearance.SetPendingAppearance(3, 103)") == V{ "true" }));
    CHECK((Run(L, "return C_Appearance.GetPendingAppearance(1), C_Appearance.GetPendingAppearance(2)") == V{ "100", "101" }));
    CHECK((Run(L, "return C_Appearance.SetPendingAppearance(3, 999)") == V{ "false" }));
    CHECK((Run(L, "return C_Appearance.CanSetAppearance(3, 103)") == V{ "true", "nil", "nil", "nil" }));
    CHECK((Run(L, "return C_Appearance.GetItemSetAppearanceID(50), C_Appearance.GetItemAppearanceID(1003)") == V{ "103", "100" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceItemSet(100)") == V{ "Tier Set", "50", "103" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceItemSet(105)") == V{ "nil", "nil", "nil" }));
    Teardown(L);
}

// Collected / uncollected packets signal only on a change.
COA_TEST(Appearance_CollectedPackets) {
    lua_State* L = Setup();
    CHECK((Feed(0x69B, CoATest::Bytes().U32(101).U32(7)) == V{ "APPEARANCE_COLLECTED:101,7" }));
    CHECK((Feed(0x69B, CoATest::Bytes().U32(101).U32(7)) == V{}));
    CHECK((Run(L, "return C_AppearanceCollection.IsAppearanceCollected(101)") == V{ "true" }));
    CHECK((Feed(0x69C, CoATest::Bytes().U32(101).U32(8)) == V{ "APPEARANCE_UNCOLLECTED:101,8" }));
    CHECK((Feed(0x69C, CoATest::Bytes().U32(101).U32(8)) == V{}));
    CHECK((Feed(0x6EB, CoATest::Bytes().Str("COLLECT_OK")) == V{ "COLLECT_ITEM_APPEARANCE_RESULT:COLLECT_OK" }));
    Teardown(L);
}

// Visibility: SMSG 0x6A2 sets the flags, SetCanSeeAppearances sends CMSG 0x6A3 and sets them at once.
COA_TEST(Appearance_Visibility) {
    lua_State* L = Setup();
    CHECK((Run(L, "return C_Appearance.CanSeeAppearances()") == V{ "true", "true" }));
    Feed(0x6A2, CoATest::Bytes().U8(0).U8(1));
    CHECK((Run(L, "return C_Appearance.CanSeeAppearances()") == V{ "false", "true" }));
    CHECK((Sent(L, "return C_Appearance.SetCanSeeAppearances(true, false)") ==
        Packets { CoATest::Expected(0x6A3, CoATest::Bytes().U8(1).U8(0)) }));
    CHECK((Run(L, "return C_Appearance.CanSeeAppearances()") == V{ "true", "false" }));
    Teardown(L);
}

// Display info, details, store pages, alternates, transmog-ability.
COA_TEST(Appearance_DisplayAndStore) {
    lua_State* L = Setup();
    CHECK((Run(L, "return C_Appearance.GetAppearanceDisplayInfo(100)") ==
        V{ "APPEARANCE_DISPLAY_TYPE_ITEM", "1000", "nil", "Iron Helm", "1000", "nil" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceDisplayInfo(105)") ==
        V{ "APPEARANCE_DISPLAY_TYPE_SPELL", "0", "nil", "未知物品", "0", "9000" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceDisplayInfo(999)") == V{ "nil", "nil", "nil", "nil", "nil" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceDetails(100), C_Appearance.GetAppearanceDetails(101)") == V{ "Nice hat", "nil" }));
    CHECK((Run(L, "return C_Appearance.IsEtherealBazaarAppearance(100), C_Appearance.IsEtherealBazaarAppearance(101)") == V{ "true", "false" }));
    CHECK((Run(L, "return C_Appearance.GetAppearanceWebURL(101)") == V{ "nil", "nil" }));
    CHECK((Run(L, "return C_Appearance.GetEtherealBazaarWebURL()") == V{ "store/ascension/147/game", "store/ascension/147/checkout" }));
    CHECK((Run(L, "local t = C_Appearance.GetAlternativeIDs(100, 1000) return #t, t[1]") == V{ "1", "1003" }));
    CHECK((Run(L, "return C_Appearance.IsTransmogable(1), C_Appearance.IsTransmogable(5)") == V{ "true", "false" }));
    CHECK((Run(L, "return C_Appearance.GetActiveDiscount(1)") == V{ "false", "nil", "nil" }));
    CHECK((Run(L, "return C_Appearance.GetCreatureDisplayItems(100)") == V{ "nil" }));
    Teardown(L);
}

// The SpellVisuals query, ClearInvalidPendingAppearances and the glue reset.
COA_TEST(Appearance_VisualInvalidAndGlue) {
    lua_State* L = Setup();
    Current({ 0, 0, 0, 0, 0, 0, 105 });
    uint32_t visual = 0;
    CHECK(CoA::Appearance::PlayerSpellVisual(500, visual) && visual == 9000);
    CHECK(!CoA::Appearance::PlayerSpellVisual(501, visual));
    Run(L, "C_Appearance.SetPendingAppearance(1, 100)");   // not collected: invalid
    Run(L, "C_Appearance.ClearInvalidPendingAppearances()");
    CHECK((Run(L, "return C_Appearance.GetPendingAppearance(1)") == V{ "nil" }));
    Collect({ 100 });
    Run(L, "C_Appearance.SetPendingAppearance(1, 100)");
    CoA::Lifecycle::RunGlueScreen();
    CHECK((Run(L, "return C_Appearance.HasPendingAppearances(), C_Appearance.GetAppearanceForCategory(6)") == V{ "false", "nil" }));
    CHECK((Run(L, "return C_AppearanceCollection.IsAppearanceCollected(100)") == V{ "true" }));   // the list survives
    Teardown(L);
}

// The glue screen keeps the outfits and visibility but drops the browser's category (no pages).
COA_TEST(Appearance_GlueKeepsOutfits) {
    lua_State* L = Setup();
    CoATest::Bytes outfits;
    outfits.U32(1).Str("Tank").U32(1).U32(0);
    Feed(0x69D, outfits);
    Run(L, "C_AppearanceCollection.ApplyCategoryFilter(1, '', {}, {})");
    CHECK((Run(L, "return C_AppearanceCollection.GetCategoryMaxPages()") == V{ "1" }));
    CoA::Lifecycle::RunGlueScreen();
    CHECK((Run(L, "return C_AppearanceCollection.GetCategoryMaxPages(), unpack(C_AppearanceOutfit.GetAppearanceOutfits())") == V{ "0", "Tank" }));
    Teardown(L);
}

// The 0x724820 predicate: with others' appearances hidden, a flagged spell (SpellCustomAttr +0x14 & 0x2000000) is kept
// off everyone but the player.
COA_TEST(Appearance_AuraPredicate) {
    lua_State* L = Setup();
    const uint32_t flagged = 1234;
    const uint32_t plain = 1235;
    CHECK(CoA::ClientHooks::addAuraEffect.AllPass(reinterpret_cast<void*>(2), 0, &flagged));   // others visible
    Feed(0x6A2, CoATest::Bytes().U8(1).U8(0));
    CHECK(!CoA::ClientHooks::addAuraEffect.AllPass(reinterpret_cast<void*>(2), 0, &flagged));  // hidden: blocked
    CHECK(CoA::ClientHooks::addAuraEffect.AllPass(reinterpret_cast<void*>(1), 0, &flagged));   // the player
    CHECK(CoA::ClientHooks::addAuraEffect.AllPass(reinterpret_cast<void*>(2), 0, &plain));     // no custom attr row
    Teardown(L);
}

// A count past the bytes left reads nothing (Ruling: the 0x726 guard).
COA_TEST(Appearance_OversizedCounts) {
    lua_State* L = Setup();
    Feed(0x69A, CoATest::Bytes().U32(1000).U32(5).U32(100));
    CHECK((Run(L, "return C_Appearance.GetAppearanceForCategory(1)") == V{ "nil" }));
    Feed(0x699, CoATest::Bytes().U32(1000).U32(100).U32(1));
    CHECK((Run(L, "return C_AppearanceCollection.IsAppearanceCollected(100)") == V{ "false" }));
    Teardown(L);
}
