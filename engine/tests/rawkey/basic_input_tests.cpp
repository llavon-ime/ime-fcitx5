#include "raw_key_harness.hpp"

using namespace llavon::ime::rawkey;

// Types "su3" (ㄋㄧˇ in the standard layout), opens the candidate list with
// Space, selects with a digit, and commits with Return.
RAWKEY_SUITE("basic input", basic_input) {
    Harness harness;

    harness.type("su3");
    // A completed syllable converts to its top candidate immediately.
    RAWKEY_ASSERT(harness.preedit() == "你");

    harness.key("space");
    RAWKEY_ASSERT(harness.has_candidates());
    const std::string first = harness.candidate(0);
    RAWKEY_ASSERT(!first.empty());

    // 1 selects the first candidate, which commits it directly.
    harness.key("1");
    harness.expect_commit(first);
}
