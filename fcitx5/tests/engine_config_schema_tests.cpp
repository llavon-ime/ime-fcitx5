#include "engine_harness.hpp"

#include <fcitx-config/rawconfig.h>
#include <fcitx/addonmanager.h>

#include <string>

#include "config/config_schema.hpp"
#include "fcitx5/ime_config.hpp"

using namespace llavon::ime::test;

// The addon configuration is created from the shared engine schema, so every
// engine field must appear in the description the config UIs render, with the
// widget type the schema asks for. Adding a field to the engine schema must
// not require any change here or in the addon.
void engine_test_config_schema(fcitx::Instance* instance) {
    instance->eventDispatcher().schedule([instance]() {
        auto* addon = instance->addonManager().addon("llavon-ime");
        FCITX_ASSERT(addon != nullptr);
        const auto* config = static_cast<const llavon::ime::ImeFcitxConfig*>(addon->getConfig());
        FCITX_ASSERT(config != nullptr);

        fcitx::RawConfig description;
        config->dumpDescription(description);

        const auto description_path = [](const std::string& ini_key, const std::string& leaf) {
            return std::string("ImeFcitxConfig/") + ini_key + "/" + leaf;
        };

        for (const auto& field : llavon::ime::config_fields()) {
            const auto* type = description.valueByPath(description_path(field.ini_key, "Type"));
            FCITX_ASSERT(type != nullptr);
            switch (field.kind) {
                case llavon::ime::ConfigValueKind::Boolean:
                    FCITX_ASSERT(*type == "Boolean");
                    break;
                case llavon::ime::ConfigValueKind::Integer: {
                    FCITX_ASSERT(*type == "Integer");
                    FCITX_ASSERT(description.valueByPath(description_path(field.ini_key, "IntMin")) != nullptr);
                    FCITX_ASSERT(description.valueByPath(description_path(field.ini_key, "IntMax")) != nullptr);
                    break;
                }
                case llavon::ime::ConfigValueKind::Text:
                    FCITX_ASSERT(*type == "String");
                    break;
                case llavon::ime::ConfigValueKind::Choice: {
                    FCITX_ASSERT(*type == "String");
                    const auto* is_enum = description.valueByPath(description_path(field.ini_key, "IsEnum"));
                    FCITX_ASSERT(is_enum != nullptr && *is_enum == "True");
                    for (size_t i = 0; i < field.choices.size(); ++i) {
                        const auto* choice = description.valueByPath(
                            description_path(field.ini_key, "Enum/" + std::to_string(i)));
                        FCITX_ASSERT(choice != nullptr && *choice == field.choices[i].label);
                    }
                    break;
                }
            }
        }

        // Values written by a config UI are stored under the INI keys; the
        // engine schema maps the labels back to canonical engine values.
        fcitx::RawConfig edited;
        edited.setValueByPath("CandidatePageSize", "7");
        edited.setValueByPath("CandidateLayout", "垂直");
        edited.setValueByPath("SmartEnglish", "True");
        edited.setValueByPath("ShiftLetterKeys", "直接放入組字區");
        auto* mutable_config = const_cast<llavon::ime::ImeFcitxConfig*>(config);
        mutable_config->load(edited, true);

        fcitx::RawConfig saved;
        mutable_config->save(saved);
        FCITX_ASSERT(saved.valueByPath("CandidatePageSize") != nullptr);
        FCITX_ASSERT(*saved.valueByPath("CandidatePageSize") == "7");
        FCITX_ASSERT(saved.valueByPath("CandidateLayout") != nullptr);
        FCITX_ASSERT(*saved.valueByPath("CandidateLayout") == "垂直");
        FCITX_ASSERT(saved.valueByPath("SmartEnglish") != nullptr);
        FCITX_ASSERT(*saved.valueByPath("SmartEnglish") == "True");

        for (const auto& field : llavon::ime::config_fields()) {
            if (field.kind != llavon::ime::ConfigValueKind::Choice) continue;
            for (const auto& choice : field.choices) {
                const auto canonical = llavon::ime::canonical_choice(field, choice.label);
                FCITX_ASSERT(canonical.has_value() && *canonical == choice.value);
            }
        }
    });
}

void engine_test_config_schema_tests(fcitx::Instance* instance) {
    engine_test_config_schema(instance);
}
