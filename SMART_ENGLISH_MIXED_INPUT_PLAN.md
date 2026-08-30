# SmartEnglish Mixed-Input Decoder Plan

## Conclusion

Do not keep extending the current "longest suffix plus rule checks" prototype.
Replace it with a mixed-input decoder that constructs Latin, Bopomofo,
number, and symbol interpretations for the complete pending input, then ranks
and presents complete output paths.

The existing regression tests remain useful. The prototype logic around
`derive_pending_suffix_reading()`, `english_locked`, and format checks inside
`ImeEngine` should be removed after the decoder replaces it.

## Behavior Contract

| Input | Preferred result | Alternative |
| --- | --- | --- |
| `su3` | `你` | Other candidates for ㄋㄧˇ |
| `hello` + Space | `hello ` | No unnecessary Chinese interference |
| `gmail.com` + Space | `gmail.com ` | Do not split `com` as Bopomofo |
| `gmail.com283` | `gmail.com打` | `gmail.com283` |
| `gmail.com5j/` + Space | `gmail.com中` | `gmail.com5j/` |
| `hello4` | Rank `hello4` according to confidence | `hell欸` |
| `mp3` | `mp3` | A valid Chinese interpretation, if one exists |
| Hsu `nef` | `你` | Other Chinese candidates |
| Hsu `hda...` | Continue entering English | Home-row selection keys must not consume letters |
| Unknown English followed by Chinese | Preserve a complete mixed candidate | The original ASCII must always remain selectable |

Shared rules:

- Do not commit text to the client before explicit confirmation.
- Every parse path must cover the exact raw input without gaps, duplication,
  or ASCII rewriting.
- The raw ASCII path must always exist.
- A tone key is strong evidence for Chinese, but not an unconditional language
  switch.
- Space may mean either an English boundary or a first-tone Bopomofo boundary.
- Continuing to type recomputes the decode result without forcing an immediate
  candidate selection.
- Shift, Caps Lock, and disabling SmartEnglish remain explicit control paths.

## Core Model

Add a shared decoder that has no fcitx runtime dependency:

```cpp
enum class MixedSegmentKind {
    Latin,
    Bopomofo,
    Number,
    Symbol,
};

struct MixedSegment {
    MixedSegmentKind kind;
    size_t begin;
    size_t end;
    std::u16string raw;
    std::u16string reading;
    std::vector<char32_t> candidates;
    double score;
};

struct MixedPath {
    std::vector<MixedSegment> segments;
    std::u16string rendered;
    double score;
};

struct MixedDecodeResult {
    std::u16string raw;
    std::vector<MixedPath> paths;
};
```

Suggested files:

- `fcitx5/shared/input/mixed_input_decoder.hpp`
- `fcitx5/shared/input/mixed_input_decoder.cpp`
- `fcitx5/shared/input/ascii_tokenizer.hpp`
- `fcitx5/shared/input/ascii_tokenizer.cpp`
- `fcitx5/tests/mixed_input_decoder_tests.cpp`
- `fcitx5/tests/ascii_tokenizer_tests.cpp`

`ImeEngine` should only manage keys, state, UI, and commits. It should not
contain Email, URL, slash, dot, or Bopomofo-suffix classification rules.

## Decode Pipeline

1. Receive the complete pending raw input, keyboard layout, and trigger data.
2. Create a directed acyclic graph with one vertex at every input position.
3. Generate Latin, structured token, number, and symbol edges.
4. Generate strict Bopomofo edges for Standard or Hsu.
5. Attach source ranges, output alternatives, and scores to every edge.
6. Run dynamic programming while retaining only the top K paths per vertex.
7. Deduplicate paths that render the same complete output.
8. Always inject the exact raw ASCII path as a fallback.
9. Render the highest-ranked path in the preedit.
10. Render the other complete paths in the candidate list.
11. Recompute when the raw input changes.
12. Commit only after Return, FocusOut, an explicit candidate selection, or an
    English Space boundary.

## Bopomofo Edges

A Bopomofo edge must satisfy all of the following:

- It uses `BopomofoKeyboardLayout::Standard` or
  `BopomofoKeyboardLayout::Hsu`.
- One trigger completes exactly one segment.
- An embedded tone must not silently create multiple segments.
- Strict replay must reject input that only succeeds by overwriting an
  existing Bopomofo slot.
- Standard `,`, `.`, `;`, `/`, and `-` retain their Bopomofo key roles.
- Hsu `d`, `f`, `j`, and `s` produce both Latin and tone interpretations.
- Space creates a first-tone interpretation but does not automatically discard
  the English path.
- The reading must have candidates in `bopomofo_char.json` before it is treated
  as a valid Chinese edge.

## ASCII Edges

The ASCII tokenizer should use generic grammar rather than specific websites,
domains, or English words. It should classify at least:

- `LatinWord`
- `Alphanumeric`
- `Email`
- `Domain`
- `URL`
- `FilesystemPath`
- `Identifier`
- `Number`
- `OperatorOrSymbol`

For example, `gmail.com` is recognized by Domain grammar. Neither `gmail` nor
`.com` may be hardcoded in the engine.

Email, URL, path, and identifier recognition should use deterministic state
machines or explicit grammar. Rules such as "any slash locks English" are not
acceptable.

`tokens/latin.json` supplies English confidence and frequency. It must not be
the sole authority on whether a span can be Latin.

## Ranking

All ranking features and weights belong in the decoder. No ranking decisions
should be scattered through `ImeEngine`.

| Feature | Ranking effect |
| --- | --- |
| Complete strict Bopomofo with explicit tone | Strong Chinese boost |
| Space-completed first tone | Moderate Chinese boost |
| Latin lexicon frequency | English boost |
| Complete Email, URL, path, or identifier grammar | Strong ASCII boost |
| Unknown contiguous Latin token | Lower-confidence English path |
| Excessive Latin fragmentation | Penalty |
| Excessive language switching | Penalty |
| Complete English prefix plus tone-closed Chinese suffix | Mixed-path boost |
| Raw ASCII fallback | Never removed |
| User selection history | Optional later-stage boost |

The initial weights must be centralized and covered by fixtures. Later tuning
should use a scenario corpus, not new branches for individual failures.

## Candidate UX

- Candidate text represents the complete output, such as `gmail.com打`, not
  only the final character.
- The preedit renders the current highest-ranked path.
- The exact raw input remains available as a complete candidate.
- Alphabetic keys continue editing a language decision. They must not select a
  candidate when Hsu or home-row selection keys overlap.
- Digits, arrows, Enter, and mouse selection may select complete paths.
- Space on an English path commits the English output plus a trailing space.
- Space on a first-tone Chinese path closes the syllable without adding a
  literal space.
- The first Escape closes alternatives and restores raw rendering.
- A second Escape clears the pending input.
- Backspace changes raw input first and then reruns decoding.
- Left, Right, Delete, Home, and End must not silently commit an English prefix.

## State Refactor

`PendingToken` should ultimately contain input facts rather than a language
guess:

```cpp
struct PendingInput {
    std::u16string raw;
    BopomofoKeyboardLayout layout;
    uint64_t revision;
};
```

Store the current decode state separately:

```cpp
struct MixedDecisionState {
    MixedDecodeResult result;
    size_t selected_path;
    uint64_t source_revision;
};
```

Remove after decoder integration:

- `PendingToken::english_locked`
- `LanguageDecision`
- `pending_should_lock_english()`
- `pending_is_english_plausible()`
- `derive_pending_suffix_reading()`
- Email, URL, dot, and slash checks in `ImeEngine`

Retain:

- `FallbackEngine::lookup()`
- `FallbackEngine::is_known_english()`
- `CompositionBuffer::add_bopomofo_keys()`
- UTF-16 complete candidate storage
- Independent state for every input context
- Config test-isolation fixes
- Existing SmartEnglish regression scenarios

## Test Strategy

All changes follow the repository's test-first requirement.

| Test layer | Coverage |
| --- | --- |
| Tokenizer unit tests | Email, URL, domain, path, identifier, number, and unknown English |
| Decoder unit tests | DAG coverage, path ranking, deduplication, and lossless output |
| Bopomofo exhaustive tests | Standard and Hsu valid syllables and tones |
| Collision tests | Latin lexicon entries that collide with Bopomofo keys |
| Engine tests | fcitx candidates, preedit, commits, and key consumption |
| State tests | Context switching, FocusOut, reset, and config reload |
| Property tests | Raw round trip, Backspace, and candidate/application agreement |
| Fuzz tests | Random ASCII without crashes, loss, or duplicate output |
| Performance tests | Long Email, URL, code, and unknown tokens |

## Scenario Matrix

Standard layout must cover:

- English followed by second-, third-, fourth-, and neutral-tone Chinese.
- English followed by first-tone Chinese.
- Email, domain, URL, path, and identifier followed by Chinese.
- Chinese followed by English.
- English, Chinese, then English without a client commit between segments.
- `283`, `5j/`, `ru,3`, `5.`, `g;4`, `x/3`, and `-4`.
- `mp3`, `hello4`, versions, IP addresses, ports, dates, and decimals.
- Single-letter English and single-key first-tone Bopomofo.
- Out-of-vocabulary English.
- Backspace, Escape, arrows, Delete, Tab, PageUp, and PageDown.
- Numeric and home-row candidate selection.

Hsu layout must cover:

- `d`, `f`, `j`, and `s` inside English words.
- Chinese sequences such as `nef`, `dyf`, and `jxl`.
- Collisions such as `hd` where both English and Chinese are valid.
- Email, URL, and domain prefixes followed by Hsu Chinese.
- Home-row candidate keys that must not consume continued English.
- Re-decoding when more characters arrive after a tone-looking key.
- Existing Hsu numeric behavior.

## Exhaustive Validation

1. Enumerate Standard body-key combinations and all five tones.
2. Enumerate bounded Hsu key combinations and tone keys.
3. Keep only sequences accepted as exactly one strict syllable.
4. Prefix every valid syllable with representative English input.
5. Include normal words, unknown words, Email, domain, URL, path, and
   identifiers as prefixes.
6. Verify that a `prefix + Chinese` path exists for every valid reading.
7. Verify that the exact raw ASCII path always exists.
8. Verify that every path covers every raw input position exactly once.
9. Verify that selecting and committing a path produces exactly its displayed
   candidate text.

## Implementation Phases

| Phase | Work | Exit condition |
| --- | --- | --- |
| 0 | Freeze known failures | Every reported failure has a red regression test |
| 1 | Define decoder types and invariants | Shared tests compile |
| 2 | Implement ASCII tokenizer | Grammar unit tests pass |
| 3 | Implement Standard/Hsu edge generation | Strict-reading tests pass |
| 4 | Implement DAG and top-K search | Coverage and ranking tests pass |
| 5 | Implement rendering and deduplication | Complete mixed-candidate tests pass |
| 6 | Integrate with `ImeEngine` | Existing and new engine tests pass |
| 7 | Remove prototype heuristics | No `english_locked` or punctuation lock rules remain |
| 8 | Add exhaustive, fuzz, and performance tests | No crashes, input loss, or performance regression |
| 9 | Run full verification and manual smoke tests | Build, ctest, and real fcitx5 scenarios pass |

## Performance Bounds

- Decoding must not expand candidate combinations exponentially.
- Keep only the top K paths at each input position. Start with `K = 8`.
- Expand only enough character candidates to fill the configured candidate
  pages.
- Use input revisions so unchanged prefixes can be reused.
- Target less than 1 ms for a 256-character pending token on a normal desktop.
- Very long input must degrade to an exact raw ASCII path rather than truncate
  or lose data.

## User Learning

Implement learning only after the deterministic decoder is stable:

- Record selected segmentation patterns locally.
- Apply only a small ranking boost.
- Never record input from `PasswordOrSensitive` contexts.
- Do not persist raw Email addresses, URLs, or other sensitive strings.
- Provide a way to clear learned data.
- Learning may reorder paths but may not remove the raw fallback.

## Expected File Changes

| File | Change |
| --- | --- |
| `shared/input/pending_token.hpp` | Reduce to raw input state |
| `shared/input/mixed_input_decoder.*` | Add core DAG decoder |
| `shared/input/ascii_tokenizer.*` | Add ASCII grammar |
| `shared/engine/fallback_engine.*` | Expose Latin score and reading lookup |
| `shared/buffer/composition_buffer.*` | Retain strict replay and enforce invariants |
| `fcitx5/ime_engine.*` | Convert into a decoder adapter |
| `fcitx5/input_context_property.hpp` | Store pending revision and decode result |
| `tests/mixed_input_decoder_tests.cpp` | Add pure decoder tests |
| `tests/ascii_tokenizer_tests.cpp` | Add tokenizer tests |
| `tests/engine_smart_regression_tests.cpp` | Retain and expand real bug scenarios |
| `tests/CMakeLists.txt` | Register new tests |

## Acceptance Criteria

- Typing `gmail.com` immediately followed by 打 or 中 no longer leaves raw
  Bopomofo keys in the result.
- Every valid Standard and Hsu syllable has a mixed path after an English
  prefix.
- Email, URL, path, code, and number input is never silently rewritten.
- Every ambiguous input retains an exact raw ASCII candidate.
- There is no key loss, duplicate commit, early commit, or cross-context state
  leak.
- Disabling SmartEnglish preserves the established non-SmartEnglish behavior.
- Unit, metadata, and engine tests all pass.
- `git diff --check`, the complete build, and CTest pass.
- Manual Standard and Hsu tests pass in chat, terminal, browser, Email, URL,
  and code-entry scenarios.

## Verification Commands

```bash
cmake --build build/fcitx5 --target llavon_ime_engine_tests -j$(nproc)

IME_FCITX5_TABLE_PATH=ime-unix-service/ime-core/table/bopomofo_char.json \
  ./build/fcitx5/tests/llavon_ime_engine_tests

cmake --build build/fcitx5 -j$(nproc)

ctest --test-dir build/fcitx5 --output-on-failure

git diff --check
```

## Research References

- [Rime input engine, segmentors, and translators](https://github.com/rime/home/wiki/RimeWithSchemata)
- [Microsoft New Phonetic Auto Input Switch](https://documentation.help/Microsoft-IME/IMEAutoInputSW.htm)
- [Oh-my-rime Chinese-English mixed input](https://www.mintimate.cc/en/demo/cnEnInput.html)
- [Mixed-language input sequence parsing and candidates](https://patents.google.com/patent/CN101943952B/en)
- [Gboard language models and out-of-vocabulary handling](https://research.google/blog/improving-gboard-language-models-via-private-federated-analytics/)

The central design principle is to model mixed input as a multi-segment,
multi-path decoding problem with ranking and a lossless fallback, rather than
continuing to add special-case language rules.
