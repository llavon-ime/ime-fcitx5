#include "fcitx5/ime_config.hpp"

namespace llavon::ime {

SchemaOptions::SchemaOptions(fcitx::Configuration* parent) {
    for (const auto& field : config_fields()) {
        const auto default_value = default_config_field_value(field);
        switch (field.kind) {
            case ConfigValueKind::Boolean: {
                auto option = std::make_unique<fcitx::Option<bool>>(parent, field.ini_key, field.label,
                                                                     default_value.boolean);
                auto* raw = option.get();
                readers_[field.key] = [raw] { return ConfigValue{.boolean = raw->value()}; };
                writers_[field.key] = [raw](const ConfigValue& value) { return raw->setValue(value.boolean); };
                options_.push_back(std::move(option));
                break;
            }
            case ConfigValueKind::Integer: {
                auto option = std::make_unique<fcitx::Option<int, fcitx::IntConstrain>>(
                    parent, field.ini_key, field.label, default_value.integer,
                    fcitx::IntConstrain(field.minimum, field.maximum));
                auto* raw = option.get();
                readers_[field.key] = [raw] { return ConfigValue{.integer = raw->value()}; };
                writers_[field.key] = [raw](const ConfigValue& value) { return raw->setValue(value.integer); };
                options_.push_back(std::move(option));
                break;
            }
            case ConfigValueKind::Text: {
                auto option = std::make_unique<fcitx::Option<std::string>>(parent, field.ini_key, field.label,
                                                                            default_value.text);
                auto* raw = option.get();
                readers_[field.key] = [raw] { return ConfigValue{.text = raw->value()}; };
                writers_[field.key] = [raw](const ConfigValue& value) { return raw->setValue(value.text); };
                options_.push_back(std::move(option));
                break;
            }
            case ConfigValueKind::Choice: {
                std::vector<std::string> labels;
                labels.reserve(field.choices.size());
                for (const auto& choice : field.choices) labels.push_back(choice.label);
                auto option = std::make_unique<ChoiceOption>(parent, field.ini_key, field.label,
                                                             choice_label(field, default_value.text),
                                                             fcitx::NoConstrain<std::string>(),
                                                             fcitx::DefaultMarshaller<std::string>(),
                                                             ChoiceAnnotation(std::move(labels)));
                auto* raw = option.get();
                readers_[field.key] = [raw] { return ConfigValue{.text = raw->value()}; };
                writers_[field.key] = [raw](const ConfigValue& value) { return raw->setValue(value.text); };
                options_.push_back(std::move(option));
                break;
            }
        }
    }
}

ConfigValue SchemaOptions::read(const ConfigField& field) const {
    const auto it = readers_.find(field.key);
    if (it == readers_.end()) return default_config_field_value(field);
    return it->second();
}

bool SchemaOptions::write(const ConfigField& field, const ConfigValue& value) {
    const auto it = writers_.find(field.key);
    if (it == writers_.end()) return false;
    return it->second(value);
}

Config to_shared_config(const ImeFcitxConfig& config) {
    Config shared = default_config();
    for (const auto& field : config_fields()) {
        auto value = config.fields.read(field);
        if (field.kind == ConfigValueKind::Choice) {
            // The option holds the INI label; the engine stores canonical values.
            const auto canonical = canonical_choice(field, value.text);
            if (!canonical) continue;
            value.text = *canonical;
        }
        (void)set_config_field_value(shared, field, value);
    }
    return shared;
}

void apply_shared_config(ImeFcitxConfig& target, const Config& source) {
    (void)target.version.setValue(DisplayVersion::Current);
    for (const auto& field : config_fields()) {
        auto value = config_field_value(source, field);
        if (field.kind == ConfigValueKind::Choice) value.text = choice_label(field, value.text);
        (void)target.fields.write(field, value);
    }
}

}  // namespace llavon::ime
