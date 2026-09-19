#include "engine_harness.hpp"

#include "fcitx5/ime_config.hpp"

#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace llavon::ime::test;

// Shift+Left/Right marks a range inside the composition and Enter stores the
// marked text as a phrase override without committing (McBopomofo's marking mode).
// The config page's "manage phrase overrides" dialog reads and writes the same
// store through the addon sub config.
void engine_test_phrase_overrides(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        FCITX_ASSERT(harness.input_context());

        const auto* path_value = std::getenv("LLAVON_IME_PHRASE_OVERRIDES_PATH");
        FCITX_ASSERT(path_value != nullptr);
        const std::filesystem::path path(path_value);

        harness.type(". u;653c/6");
        const std::string original_name = harness.preedit();
        FCITX_ASSERT(!original_name.empty());

        // Ctrl+Shift+Enter is not an engine shortcut; it must stay pass-through.
        FCITX_ASSERT(!harness.key_accepted(fcitx::Key("Control+Shift+Return")));
        FCITX_ASSERT(harness.preedit() == original_name);

        // Escape without a mark keeps the composition.
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(harness.preedit() == original_name);

        // The caret starts at the end, so Shift+Left marks the whole phrase.
        // While marking, the hint panel shows what Enter would store; Escape
        // drops the mark again without storing anything.
        for (size_t i = 0; i < 4; ++i) harness.key(fcitx::Key(FcitxKey_Left, fcitx::KeyState::Shift));
        FCITX_ASSERT(harness.preedit() == original_name);
        FCITX_ASSERT(harness.candidate_count() == 1);
        FCITX_ASSERT(harness.candidate(0).find("Enter") != std::string::npos);
        harness.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(harness.preedit() == original_name);
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(!std::filesystem::exists(path));

        // Enter in the marking state stores the phrase and keeps composing.
        for (size_t i = 0; i < 4; ++i) harness.key(fcitx::Key(FcitxKey_Left, fcitx::KeyState::Shift));
        harness.key(fcitx::Key(FcitxKey_Return));
        FCITX_ASSERT(harness.preedit() == original_name);
        FCITX_ASSERT(!harness.has_candidates());
        FCITX_ASSERT(std::filesystem::exists(path));

        // The editor shows the saved entry with its normalized readings.
        auto* addon = instance->addonManager().addon("llavon-ime");
        const auto* sub_config = addon->getSubConfig("phraseoverrides");
        FCITX_ASSERT(sub_config != nullptr);
        const auto* editor = static_cast<const llavon::ime::PhraseOverrideEditorConfig*>(sub_config);
        FCITX_ASSERT(editor->entries->size() == 1);
        FCITX_ASSERT(editor->entries->front().phrase.value() == original_name);
        FCITX_ASSERT(editor->entries->front().readings.value() == "ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ");

        // The stored phrase matches the composition, so committing is unchanged.
        harness.expect_commit(original_name);

        // Saving the dialog replaces the store and takes effect immediately.
        fcitx::RawConfig replacement;
        replacement.setValueByPath("Entries/0/Phrase", "歐陽芷珩");
        replacement.setValueByPath("Entries/0/Readings", "ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ");
        addon->setSubConfig("phraseoverrides", replacement);

        harness.type(". u;653c/6");
        FCITX_ASSERT(harness.preedit() == "歐陽芷珩");
        harness.expect_commit("歐陽芷珩");

        // The editor keeps its identity while its rows are refreshed, so a
        // frontend that holds the pointer never reads freed memory.
        fcitx::RawConfig partial;
        partial.setValueByPath("Entries/0/Phrase", "歐陽芷珩");
        partial.setValueByPath("Entries/0/Readings", "ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ");
        partial.setValueByPath("Entries/1/Phrase", "宇文澄曜");
        addon->setSubConfig("phraseoverrides", partial);
        const auto* refreshed = addon->getSubConfig("phraseoverrides");
        FCITX_ASSERT(refreshed == sub_config);
        // A malformed row is dropped instead of discarding the whole edit.
        FCITX_ASSERT(editor->entries->size() == 1);
        FCITX_ASSERT(editor->entries->front().phrase.value() == "歐陽芷珩");

        // An absent list is an action-style trigger, never a silent wipe.
        addon->setSubConfig("phraseoverrides", fcitx::RawConfig());
        FCITX_ASSERT(editor->entries->size() == 1);

        // Editing the file by hand still wins after a config reload.
        {
            std::ofstream output(path, std::ios::trunc);
            output << "宇文澄曜 ㄩˇ-ㄨㄣˊ-ㄔㄥˊ-ㄧㄠˋ\n";
        }
        addon->reloadConfig();

        harness.type("m3jp6t/6ul4");
        FCITX_ASSERT(harness.preedit() == "宇文澄曜");
        harness.expect_commit("宇文澄曜");

        // Marking stores the current text and pins it immediately; continuing
        // to type keeps the forced phrase instead of washing it out.
        harness.type("su3cl3");
        FCITX_ASSERT(harness.preedit() == "你好");
        for (size_t i = 0; i < 2; ++i) harness.key(fcitx::Key(FcitxKey_Left, fcitx::KeyState::Shift));
        harness.key(fcitx::Key(FcitxKey_Return));
        {
            auto* state = harness.engine_state();
            FCITX_ASSERT(state != nullptr);
            FCITX_ASSERT(state->session->buffer.segments().size() == 2);
            FCITX_ASSERT(state->session->buffer.segments()[0].phrase_override_chosen);
            FCITX_ASSERT(state->session->buffer.segments()[1].phrase_override_chosen);
        }
        harness.type("su3");
        {
            auto* state = harness.engine_state();
            FCITX_ASSERT(state != nullptr);
            FCITX_ASSERT(state->session->buffer.segments().size() == 3);
            FCITX_ASSERT(state->session->buffer.segments()[0].phrase_override_chosen);
            FCITX_ASSERT(state->session->buffer.segments()[1].phrase_override_chosen);
        }
        FCITX_ASSERT(harness.preedit().rfind("你好", 0) == 0);
        harness.expect_commit("你好你");

        // The phrase also applies in the middle of a longer composition.
        fcitx::RawConfig middle_overrides;
        middle_overrides.setValueByPath("Entries/0/Phrase", "宇你");
        middle_overrides.setValueByPath("Entries/0/Readings", "ㄋㄧˇ-ㄏㄠˇ");
        addon->setSubConfig("phraseoverrides", middle_overrides);
        harness.type("su3su3cl3");
        FCITX_ASSERT(harness.preedit() == "你宇你");
        harness.expect_commit("你宇你");

        // Selecting another candidate while a phrase override is pinned
        // releases the pin instead of being ignored.
        fcitx::RawConfig select_overrides;
        select_overrides.setValueByPath("Entries/0/Phrase", "宇你");
        select_overrides.setValueByPath("Entries/0/Readings", "ㄋㄧˇ-ㄏㄠˇ");
        addon->setSubConfig("phraseoverrides", select_overrides);
        harness.type("su3cl3");
        FCITX_ASSERT(harness.preedit() == "宇你");
        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(harness.candidate_count() >= 2);
        harness.key(fcitx::Key(static_cast<fcitx::KeySym>('2')));
        {
            auto* state = harness.engine_state();
            FCITX_ASSERT(state != nullptr);
            FCITX_ASSERT(state->session->buffer.segments()[1].manually_chosen);
            FCITX_ASSERT(!state->session->buffer.segments()[1].phrase_override_chosen);
            FCITX_ASSERT(!state->session->buffer.segments()[0].phrase_override_chosen);
        }
        harness.expect_commit(harness.preedit());
    });
}
