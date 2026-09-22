#include "raw_key_harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace llavon::ime::rawkey;

namespace {

void write_overrides(const std::filesystem::path& path, std::string_view line) {
    std::ofstream output(path, std::ios::trunc);
    output << line << "\n";
}

}  // namespace

// Shift+Left/Right marks a range inside the composition and Enter stores the
// marked text as a phrase override without committing (McBopomofo's marking
// mode). The store lives in the shared phrase override file, so a hand edit
// plus a reload must take effect too.
RAWKEY_SUITE("phrase overrides", phrase_overrides) {
    Harness harness;
    const std::filesystem::path path(harness.phrase_overrides_path());

    harness.type(". u;653c/6");
    const std::string original_name = harness.preedit();
    RAWKEY_ASSERT(!original_name.empty());

    // Ctrl+Shift+Enter is not an engine shortcut; it must stay pass-through.
    RAWKEY_ASSERT(!harness.key_accepted(Key("Control+Shift+Return")));
    RAWKEY_ASSERT(harness.preedit() == original_name);

    // Escape without a mark keeps the composition.
    harness.key(Key("Escape"));
    RAWKEY_ASSERT(harness.preedit() == original_name);

    // The caret starts at the end, so Shift+Left marks the whole phrase.
    // While marking, the hint panel shows what Enter would store; Escape drops
    // the mark again without storing anything.
    for (std::size_t i = 0; i < 4; ++i) harness.key(Key("Left").with(kShift));
    RAWKEY_ASSERT(harness.preedit() == original_name);
    RAWKEY_ASSERT(harness.candidate_count() == 1);
    RAWKEY_ASSERT(harness.candidate(0).find("Enter") != std::string::npos);
    harness.key(Key("Escape"));
    RAWKEY_ASSERT(harness.preedit() == original_name);
    RAWKEY_ASSERT(!harness.has_candidates());
    RAWKEY_ASSERT(!std::filesystem::exists(path));

    // Enter in the marking state stores the phrase and keeps composing.
    for (std::size_t i = 0; i < 4; ++i) harness.key(Key("Left").with(kShift));
    harness.key(Key("Return"));
    RAWKEY_ASSERT(harness.preedit() == original_name);
    RAWKEY_ASSERT(!harness.has_candidates());
    RAWKEY_ASSERT(std::filesystem::exists(path));

    // The stored phrase matches the composition, so committing is unchanged.
    harness.expect_commit(original_name);

    // Replacing the file takes effect after a reload.
    write_overrides(path, "歐陽芷珩 ㄡ-ㄧㄤˊ-ㄓˇ-ㄏㄥˊ");
    harness.reload_phrase_overrides();
    harness.type(". u;653c/6");
    RAWKEY_ASSERT(harness.preedit() == "歐陽芷珩");
    harness.expect_commit("歐陽芷珩");

    // Editing the file by hand still wins after a reload.
    write_overrides(path, "宇文澄曜 ㄩˇ-ㄨㄣˊ-ㄔㄥˊ-ㄧㄠˋ");
    harness.reload_phrase_overrides();
    harness.type("m3jp6t/6ul4");
    RAWKEY_ASSERT(harness.preedit() == "宇文澄曜");
    harness.expect_commit("宇文澄曜");

    // Marking stores the current text and pins it immediately; continuing to
    // type keeps the forced phrase instead of washing it out.
    harness.type("su3cl3");
    RAWKEY_ASSERT(harness.preedit() == "你好");
    for (std::size_t i = 0; i < 2; ++i) harness.key(Key("Left").with(kShift));
    harness.key(Key("Return"));
    {
        auto* state = harness.session();
        RAWKEY_ASSERT(state != nullptr);
        RAWKEY_ASSERT(state->buffer.segments().size() == 2);
        RAWKEY_ASSERT(state->buffer.segments()[0].phrase_override_chosen);
        RAWKEY_ASSERT(state->buffer.segments()[1].phrase_override_chosen);
    }
    harness.type("su3");
    {
        auto* state = harness.session();
        RAWKEY_ASSERT(state != nullptr);
        RAWKEY_ASSERT(state->buffer.segments().size() == 3);
        RAWKEY_ASSERT(state->buffer.segments()[0].phrase_override_chosen);
        RAWKEY_ASSERT(state->buffer.segments()[1].phrase_override_chosen);
    }
    RAWKEY_ASSERT(harness.preedit().rfind("你好", 0) == 0);
    harness.expect_commit("你好你");

    // The phrase also applies in the middle of a longer composition.
    write_overrides(path, "宇你 ㄋㄧˇ-ㄏㄠˇ");
    harness.reload_phrase_overrides();
    harness.type("su3su3cl3");
    RAWKEY_ASSERT(harness.preedit() == "你宇你");
    harness.expect_commit("你宇你");

    // Selecting another candidate while a phrase override is pinned releases
    // the pin instead of being ignored.
    write_overrides(path, "宇你 ㄋㄧˇ-ㄏㄠˇ");
    harness.reload_phrase_overrides();
    harness.type("su3cl3");
    RAWKEY_ASSERT(harness.preedit() == "宇你");
    harness.key(Key("Down"));
    RAWKEY_ASSERT(harness.has_candidates());
    RAWKEY_ASSERT(harness.candidate_count() >= 2);
    harness.key(Key('2'));
    {
        auto* state = harness.session();
        RAWKEY_ASSERT(state != nullptr);
        RAWKEY_ASSERT(state->buffer.segments()[1].manually_chosen);
        RAWKEY_ASSERT(!state->buffer.segments()[1].phrase_override_chosen);
        RAWKEY_ASSERT(!state->buffer.segments()[0].phrase_override_chosen);
    }
    harness.expect_commit(harness.preedit());
}
