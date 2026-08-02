#include "engine_harness.hpp"

#include <fcitx-utils/log.h>
#include <fcitx/instance.h>

#include <string>

using namespace ime::fcitx5::test;

namespace {

int cursor_index(const EngineHarness& harness) {
    const auto list = harness.input_context()->inputPanel().candidateList();
    return list ? list->cursorIndex() : -1;
}

}  // namespace

// Down/Up move the cursor within the page; Home/End jump to list bounds.
void engine_test_candidate_navigation_tests(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        EngineHarness harness(instance);
        harness.type("su3");
        harness.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(harness.has_candidates());
        FCITX_ASSERT(cursor_index(harness) == 0);
        const std::string first = harness.candidate(0);

        harness.key(fcitx::Key(FcitxKey_Down));
        FCITX_ASSERT(cursor_index(harness) == 1);
        FCITX_ASSERT(harness.candidate(1) != first);

        harness.key(fcitx::Key(FcitxKey_Up));
        FCITX_ASSERT(cursor_index(harness) == 0);

        harness.key(fcitx::Key(FcitxKey_End));
        FCITX_ASSERT(cursor_index(harness) ==
                     static_cast<int>(harness.candidate_count()) - 1);

        harness.key(fcitx::Key(FcitxKey_Home));
        FCITX_ASSERT(cursor_index(harness) == 0);
        harness.expect_commit(first);

        // Left/Right flip pages with a small page size.
        EngineHarness pager(instance);
        pager.set_config("CandidatePageSize", "3");
        pager.type("su3");
        pager.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(pager.candidate_count() == 3);
        const std::string first_page = pager.candidate(0);

        pager.key(fcitx::Key(FcitxKey_Right));
        FCITX_ASSERT(pager.candidate_count() == 3);
        FCITX_ASSERT(pager.candidate(0) != first_page);

        pager.key(fcitx::Key(FcitxKey_Left));
        FCITX_ASSERT(pager.candidate(0) == first_page);

        // Page_Up / Page_Down flip pages and reset the cursor.
        pager.key(fcitx::Key(FcitxKey_Page_Down));
        FCITX_ASSERT(pager.candidate_count() == 3);
        FCITX_ASSERT(pager.candidate(0) != first_page);
        FCITX_ASSERT(cursor_index(pager) == 0);

        pager.key(fcitx::Key(FcitxKey_Page_Up));
        FCITX_ASSERT(pager.candidate(0) == first_page);
        FCITX_ASSERT(cursor_index(pager) == 0);
        pager.expect_commit(pager.candidate(0));

        // Tab opens the candidate list; a second Tab expands it, a third collapses.
        EngineHarness tab(instance);
        tab.set_config("CandidatePageSize", "3");
        tab.type("su3");
        tab.key(fcitx::Key(FcitxKey_Tab));
        FCITX_ASSERT(tab.has_candidates());
        const size_t collapsed = tab.candidate_count();

        tab.key(fcitx::Key(FcitxKey_Tab));
        FCITX_ASSERT(tab.candidate_count() > collapsed);

        tab.key(fcitx::Key(FcitxKey_Tab));
        FCITX_ASSERT(tab.candidate_count() == collapsed);
        tab.expect_commit(tab.candidate(0));

        // A digit selects the candidate at that index.
        EngineHarness digit(instance);
        digit.type("su3");
        digit.key(fcitx::Key(FcitxKey_space));
        const std::string second = digit.candidate(1);
        FCITX_ASSERT(!second.empty());
        digit.key(fcitx::Key(FcitxKey_2));
        FCITX_ASSERT(digit.preedit() == second);
        digit.expect_commit(second);

        // Return selects the candidate under the cursor.
        EngineHarness enter(instance);
        enter.type("su3");
        enter.key(fcitx::Key(FcitxKey_space));
        enter.key(fcitx::Key(FcitxKey_Down));
        const std::string second_enter = enter.candidate(1);
        enter.key(fcitx::Key(FcitxKey_Return));
        FCITX_ASSERT(enter.preedit() == second_enter);
        enter.expect_commit(second_enter);

        // ChooseCandidateUsingSpace=True: a second Space selects the cursor
        // candidate; False: Space only opens the list.
        EngineHarness space_true(instance);
        space_true.set_config("ChooseCandidateUsingSpace", "True");
        space_true.type("su3");
        space_true.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(space_true.has_candidates());
        const std::string top = space_true.candidate(0);
        space_true.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(!space_true.has_candidates());
        FCITX_ASSERT(space_true.preedit() == top);
        space_true.expect_commit(top);

        EngineHarness space_false(instance);
        space_false.set_config("ChooseCandidateUsingSpace", "False");
        space_false.type("su3");
        space_false.key(fcitx::Key(FcitxKey_space));
        space_false.key(fcitx::Key(FcitxKey_space));
        FCITX_ASSERT(space_false.has_candidates());
        FCITX_ASSERT(cursor_index(space_false) == 0);
        space_false.key(fcitx::Key(FcitxKey_Return));
        FCITX_ASSERT(space_false.preedit() == top);
        space_false.expect_commit(top);

        // Escape closes the candidate list, keeping the composition; a second
        // Escape keeps the composition intact.
        EngineHarness esc(instance);
        esc.type("su3");
        esc.key(fcitx::Key(FcitxKey_space));
        esc.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(!esc.has_candidates());
        FCITX_ASSERT(esc.preedit() == top);
        esc.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(esc.preedit() == top);
        esc.expect_commit(top);

        // EscKeyClearsEntireComposingBuffer=True: Escape clears the buffer.
        EngineHarness esc_clear(instance);
        esc_clear.set_config("EscKeyClearsEntireComposingBuffer", "True");
        esc_clear.type("su3");
        esc_clear.key(fcitx::Key(FcitxKey_Escape));
        FCITX_ASSERT(esc_clear.preedit().empty());
        FCITX_ASSERT(!esc_clear.has_candidates());
    });
}
