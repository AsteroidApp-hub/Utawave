// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2025-2026 Utawave

// Utawave — プラグインチェーン Undo アクションの往復ユニットテスト
//
// 追加/削除/バイパス/並べ替え/トラック間移動を perform()/undo()/redo() で直接叩き、
// (1) チェーン状態の往復、(2) 同一インスタンスが undo/redo を跨いで保たれること (identity)、
// (3) 置換/移動で退避したインスタンスが復元されること、(4) resolver が nullptr (削除済み
// トラック) のとき no-op で安全に終わること、を検証する。
//
// 依存: PluginChain.cpp (リンク済み)。PluginActions.h はヘッダオンリー。
// 実プラグインの代わりに最小スタブ FakePlugin (stereo I/O) を使う (デバイス/スキャン不要)。

#include <JuceHeader.h>
#include "../Source/Edit/PluginActions.h"
#include "../Source/VST/PluginChain.h"

namespace
{

// 最小の AudioPluginInstance スタブ。id で識別し、ポインタ identity を追跡できる。
class FakePlugin : public juce::AudioPluginInstance
{
public:
    explicit FakePlugin(int idIn)
        : juce::AudioPluginInstance(BusesProperties()
              .withInput ("In",  juce::AudioChannelSet::stereo())
              .withOutput("Out", juce::AudioChannelSet::stereo())),
          id(idIn) {}

    int id { 0 };

    const juce::String getName() const override            { return "Fake" + juce::String(id); }
    void prepareToPlay(double, int) override               {}
    void releaseResources() override                       {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    using juce::AudioProcessor::processBlock;
    double getTailLengthSeconds() const override           { return 0.0; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    juce::AudioProcessorEditor* createEditor() override    { return nullptr; }
    bool hasEditor() const override                        { return false; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram(int) override                   {}
    const juce::String getProgramName(int) override        { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override  {}
    void setStateInformation(const void*, int) override    {}
    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = getName(); }
};

static std::unique_ptr<juce::AudioPluginInstance> makeFake(int id)
{
    return std::make_unique<FakePlugin>(id);
}
static int idAt(PluginChain& c, int slot)
{
    auto* p = c.getPlugin(slot);
    return p ? static_cast<FakePlugin*>(p)->id : -1;
}

class PluginActionsTests : public juce::UnitTest
{
public:
    PluginActionsTests() : juce::UnitTest("PluginActions (undo/redo)") {}

    void runTest() override
    {
        testAddRoundTripAndIdentity();
        testRemoveRoundTripAndIdentity();
        testReplacePreservesDisplaced();
        testBypassRoundTrip();
        testSwapInvolutive();
        testMoveAcrossChains();
        testMoveOntoOccupiedPreservesDisplaced();
        testResolverNullIsNoOp();
        // バイパス状態が Undo/Redo を跨いで保たれること (extract/insert が bypassed を
        // false に落とすため、アクション側で退避・復元する修正の回帰テスト)
        testRemovePreservesBypass();
        testAddWithBypass();
        testReplacePreservesBypass();
        testMovePreservesBypass();
    }

    static EditActions::ChainResolver resolverFor(PluginChain& c)
    {
        return [&c]() -> PluginChain* { return &c; };
    }

    // ── 追加: perform=挿入 / undo=除去 / redo=同一インスタンスで復活 ──
    void testAddRoundTripAndIdentity()
    {
        beginTest("PluginSlotAction add: insert / undo removes / redo restores same instance");
        PluginChain chain;
        auto plugin = makeFake(1);
        auto* rawPtr = plugin.get();

        int changes = 0;
        EditActions::PluginSlotAction act(resolverFor(chain), 0, std::move(plugin),
            [&] { ++changes; }, {});

        act.perform();
        expect(chain.getNumPlugins() == 1, "perform -> 1 plugin");
        expect(chain.getPlugin(0) == rawPtr, "perform -> same instance inserted");

        act.undo();
        expect(chain.getNumPlugins() == 0, "undo -> empty");

        act.perform();   // redo
        expect(chain.getNumPlugins() == 1 && chain.getPlugin(0) == rawPtr,
               "redo -> same instance restored (identity preserved)");
        expect(changes == 3, "onChanged fired on perform/undo/redo");
    }

    // ── 削除: perform=取り出し / undo=同一インスタンスで復活 ──
    void testRemoveRoundTripAndIdentity()
    {
        beginTest("PluginSlotAction remove: extract / undo restores same instance");
        PluginChain chain;
        auto* rawPtr = makeFakeInto(chain, 7);

        EditActions::PluginSlotAction act(resolverFor(chain), 0, /*afterInstance*/ nullptr,
            {}, {});

        act.perform();
        expect(chain.getNumPlugins() == 0, "perform -> removed (empty)");

        act.undo();
        expect(chain.getNumPlugins() == 1 && chain.getPlugin(0) == rawPtr,
               "undo -> same instance restored");

        act.perform();   // redo
        expect(chain.getNumPlugins() == 0, "redo -> removed again");
    }

    // ── 置換: dst にあった既存を退避し undo で戻す ──
    void testReplacePreservesDisplaced()
    {
        beginTest("PluginSlotAction replace: displaced instance restored on undo");
        PluginChain chain;
        auto* origPtr = makeFakeInto(chain, 1);   // slot0 = id1
        auto incoming  = makeFake(2);
        auto* newPtr   = incoming.get();

        int willRemoveCalls = 0;
        EditActions::PluginSlotAction act(resolverFor(chain), 0, std::move(incoming),
            {}, [&](juce::AudioPluginInstance*) { ++willRemoveCalls; });

        act.perform();
        expect(idAt(chain, 0) == 2 && chain.getPlugin(0) == newPtr, "perform -> slot holds new (id2)");
        expect(willRemoveCalls == 1, "perform -> willRemove called for displaced original");

        act.undo();
        expect(idAt(chain, 0) == 1 && chain.getPlugin(0) == origPtr, "undo -> original (id1) restored");

        act.perform();   // redo
        expect(idAt(chain, 0) == 2 && chain.getPlugin(0) == newPtr, "redo -> new (id2) again");
    }

    // ── バイパス: トグルの往復 ──
    void testBypassRoundTrip()
    {
        beginTest("PluginBypassAction: toggle round-trip");
        PluginChain chain;
        makeFakeInto(chain, 1);
        expect(!chain.isBypassed(0), "initial: not bypassed");

        EditActions::PluginBypassAction act(resolverFor(chain), 0,
            /*before*/ false, /*after*/ true, {});

        act.perform();
        expect(chain.isBypassed(0), "perform -> bypassed");
        act.undo();
        expect(!chain.isBypassed(0), "undo -> not bypassed");
        act.perform();
        expect(chain.isBypassed(0), "redo -> bypassed");
    }

    // ── 並べ替え: swap は対合 (perform==undo) ──
    void testSwapInvolutive()
    {
        beginTest("PluginSwapAction: swap is involutive (perform/undo restore order)");
        PluginChain chain;
        makeFakeInto(chain, 1);   // slot0
        makeFakeInto(chain, 2);   // slot1

        EditActions::PluginSwapAction act(resolverFor(chain), 0, 1, {});

        act.perform();
        expect(idAt(chain, 0) == 2 && idAt(chain, 1) == 1, "perform -> swapped");
        act.undo();
        expect(idAt(chain, 0) == 1 && idAt(chain, 1) == 2, "undo -> original order");
    }

    // ── トラック間移動: src→dst、undo で戻る (identity 保持) ──
    void testMoveAcrossChains()
    {
        beginTest("PluginMoveAction: move src->dst, undo moves back (identity)");
        PluginChain src, dst;
        auto* movedPtr = makeFakeInto(src, 5);   // src slot0 = id5

        EditActions::PluginMoveAction act(resolverFor(src), 0, resolverFor(dst), 0, {}, {});

        act.perform();
        expect(src.getNumPlugins() == 0, "perform -> src empty");
        expect(dst.getNumPlugins() == 1 && dst.getPlugin(0) == movedPtr,
               "perform -> dst holds moved instance (same pointer)");

        act.undo();
        expect(dst.getNumPlugins() == 0, "undo -> dst empty");
        expect(src.getNumPlugins() == 1 && src.getPlugin(0) == movedPtr,
               "undo -> src holds moved instance again");
    }

    // ── 移動先が埋まっていたら退避 → undo で復元 ──
    void testMoveOntoOccupiedPreservesDisplaced()
    {
        beginTest("PluginMoveAction: dst occupied -> displaced restored on undo");
        PluginChain src, dst;
        auto* movedPtr = makeFakeInto(src, 5);   // src slot0 = id5
        auto* dstPtr   = makeFakeInto(dst, 9);   // dst slot0 = id9

        EditActions::PluginMoveAction act(resolverFor(src), 0, resolverFor(dst), 0, {}, {});

        act.perform();
        expect(src.getNumPlugins() == 0, "perform -> src empty");
        expect(dst.getPlugin(0) == movedPtr, "perform -> dst slot0 = moved (id5)");

        act.undo();
        expect(src.getPlugin(0) == movedPtr, "undo -> moved back to src");
        expect(dst.getPlugin(0) == dstPtr, "undo -> dst original (id9) restored");
    }

    // ── resolver が nullptr (削除済みトラック) → no-op・クラッシュしない ──
    void testResolverNullIsNoOp()
    {
        beginTest("Actions with null resolver: safe no-op (deleted-track guard)");
        EditActions::ChainResolver nullResolver = []() -> PluginChain* { return nullptr; };

        EditActions::PluginSlotAction addAct(nullResolver, 0, makeFake(1), {}, {});
        expect(!addAct.perform(), "add perform -> false (no chain)");
        expect(!addAct.undo(),    "add undo -> false (no chain)");

        EditActions::PluginBypassAction bypAct(nullResolver, 0, false, true, {});
        expect(!bypAct.perform(), "bypass perform -> false");

        EditActions::PluginSwapAction swapAct(nullResolver, 0, 1, {});
        expect(!swapAct.perform(), "swap perform -> false");

        EditActions::PluginMoveAction moveAct(nullResolver, 0, nullResolver, 0, {}, {});
        expect(!moveAct.perform(), "move perform -> false");
    }

    // ── 削除 → Undo でバイパス状態が復元される (回帰: extract が bypassed を落とす) ──
    void testRemovePreservesBypass()
    {
        beginTest("PluginSlotAction remove: bypass state restored on undo (not reset to false)");
        PluginChain chain;
        makeFakeInto(chain, 7);
        chain.setBypassed(0, true);
        expect(chain.isBypassed(0), "setup: slot0 bypassed");

        EditActions::PluginSlotAction act(resolverFor(chain), 0, /*afterInstance*/ nullptr, {}, {});
        act.perform();
        expect(chain.getNumPlugins() == 0, "perform -> removed");

        act.undo();
        expect(chain.getNumPlugins() == 1, "undo -> restored");
        expect(chain.isBypassed(0), "undo -> bypass ON restored");

        act.perform();   // redo
        expect(chain.getNumPlugins() == 0, "redo -> removed again");
        act.undo();
        expect(chain.isBypassed(0), "undo again -> bypass ON still restored");
    }

    // ── 追加時に afterBypass を指定 (バイパス中プラグインの複製でバイパスを継承する経路) ──
    void testAddWithBypass()
    {
        beginTest("PluginSlotAction add with afterBypass=true (copy of a bypassed plugin)");
        PluginChain chain;
        EditActions::PluginSlotAction act(resolverFor(chain), 0, makeFake(3), {}, {},
                                          /*afterBypass*/ true);
        act.perform();
        expect(chain.getNumPlugins() == 1, "perform -> inserted");
        expect(chain.isBypassed(0), "perform -> inserted bypassed (afterBypass honored)");

        act.undo();
        expect(chain.getNumPlugins() == 0, "undo -> removed");

        act.perform();   // redo
        expect(chain.isBypassed(0), "redo -> bypassed again");
    }

    // ── 置換: 退避した既存プラグインのバイパスも往復する ──
    void testReplacePreservesBypass()
    {
        beginTest("PluginSlotAction replace: displaced plugin's bypass restored on undo");
        PluginChain chain;
        makeFakeInto(chain, 1);          // slot0 = id1
        chain.setBypassed(0, true);      // id1 をバイパス
        EditActions::PluginSlotAction act(resolverFor(chain), 0, makeFake(2), {}, {});  // afterBypass=false
        act.perform();
        expect(idAt(chain, 0) == 2, "perform -> id2 in slot");
        expect(!chain.isBypassed(0), "perform -> new id2 not bypassed");

        act.undo();
        expect(idAt(chain, 0) == 1, "undo -> id1 restored");
        expect(chain.isBypassed(0), "undo -> id1 bypass restored");

        act.perform();   // redo
        expect(idAt(chain, 0) == 2 && !chain.isBypassed(0), "redo -> id2 not bypassed again");
    }

    // ── トラック間移動: 移動プラグイン + 退避プラグインのバイパスが往復する ──
    void testMovePreservesBypass()
    {
        beginTest("PluginMoveAction: bypass of moved and displaced preserved across undo/redo");
        PluginChain src, dst;
        makeFakeInto(src, 5); src.setBypassed(0, true);   // src id5 をバイパス
        makeFakeInto(dst, 9); dst.setBypassed(0, true);   // dst id9 をバイパス

        EditActions::PluginMoveAction act(resolverFor(src), 0, resolverFor(dst), 0, {}, {});
        act.perform();
        expect(idAt(dst, 0) == 5, "perform -> id5 moved to dst");
        expect(dst.isBypassed(0), "perform -> moved id5 keeps bypass at dst");

        act.undo();
        expect(idAt(src, 0) == 5 && src.isBypassed(0), "undo -> id5 back to src with bypass");
        expect(idAt(dst, 0) == 9 && dst.isBypassed(0), "undo -> displaced id9 restored to dst with bypass");

        act.perform();   // redo
        expect(idAt(dst, 0) == 5 && dst.isBypassed(0), "redo -> id5 to dst bypassed again");
    }

private:
    // 指定 id の FakePlugin をチェーン末尾へ追加し、その生ポインタを返す
    static juce::AudioPluginInstance* makeFakeInto(PluginChain& c, int id)
    {
        auto p = makeFake(id);
        auto* raw = p.get();
        c.addPlugin(std::move(p));
        return raw;
    }
};

static PluginActionsTests pluginActionsTests;

}  // namespace
