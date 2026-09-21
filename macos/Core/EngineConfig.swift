import Foundation

// Schema-driven settings. The engine owns the field list and exports it through
// lv_config_schema_json, so adding a config option in the engine schema
// (engine/src/config/config_schema.cpp) makes it appear in the native settings
// window without touching any Swift code.

struct ConfigChoice: Equatable {
    let value: String
    let label: String
}

struct ConfigField: Equatable {
    enum Kind: String {
        case boolean
        case integer
        case text
        case choice
    }

    let key: String
    let iniKey: String
    let label: String
    let group: String
    let kind: Kind
    let minimum: Int?
    let maximum: Int?
    let choices: [ConfigChoice]
    let defaultValue: ConfigValue?
}

enum ConfigValue: Equatable {
    case boolean(Bool)
    case integer(Int)
    case text(String)

    var boolValue: Bool? {
        if case .boolean(let value) = self { return value }
        return nil
    }

    var intValue: Int? {
        if case .integer(let value) = self { return value }
        return nil
    }

    var stringValue: String? {
        if case .text(let value) = self { return value }
        return nil
    }
}

struct ConfigSchema: Equatable {
    let fields: [ConfigField]

    static func decode(_ json: String) -> ConfigSchema? {
        guard let data = json.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let rawFields = object["fields"] as? [[String: Any]] else {
            return nil
        }

        var fields: [ConfigField] = []
        for raw in rawFields {
            guard let key = raw["key"] as? String,
                  let label = raw["label"] as? String,
                  let group = raw["group"] as? String,
                  let kind = (raw["kind"] as? String).flatMap(ConfigField.Kind.init) else {
                continue
            }

            var choices: [ConfigChoice] = []
            for rawChoice in raw["choices"] as? [[String: Any]] ?? [] {
                if let value = rawChoice["value"] as? String, let choiceLabel = rawChoice["label"] as? String {
                    choices.append(ConfigChoice(value: value, label: choiceLabel))
                }
            }

            var defaultValue: ConfigValue?
            switch kind {
            case .boolean:
                if let value = raw["default"] as? Bool { defaultValue = .boolean(value) }
            case .integer:
                if let value = raw["default"] as? Int { defaultValue = .integer(value) }
            case .text, .choice:
                if let value = raw["default"] as? String { defaultValue = .text(value) }
            }

            fields.append(ConfigField(key: key,
                                      iniKey: raw["ini_key"] as? String ?? key,
                                      label: label,
                                      group: group,
                                      kind: kind,
                                      minimum: raw["minimum"] as? Int,
                                      maximum: raw["maximum"] as? Int,
                                      choices: choices,
                                      defaultValue: defaultValue))
        }
        return ConfigSchema(fields: fields)
    }
}

// Current config values keyed by field key, paired with the schema that
// describes them.
struct EngineConfig {
    let schema: ConfigSchema
    private(set) var values: [String: ConfigValue]

    static func decode(schema: ConfigSchema, json: Data) -> EngineConfig? {
        guard let object = try? JSONSerialization.jsonObject(with: json) as? [String: Any] else {
            return nil
        }
        var values: [String: ConfigValue] = [:]
        for field in schema.fields {
            guard let raw = object[field.key] else { continue }
            switch field.kind {
            case .boolean:
                if let value = raw as? Bool { values[field.key] = .boolean(value) }
            case .integer:
                if let value = raw as? Int { values[field.key] = .integer(value) }
            case .text, .choice:
                if let value = raw as? String { values[field.key] = .text(value) }
            }
        }
        return EngineConfig(schema: schema, values: values)
    }

    func value(_ key: String) -> ConfigValue? {
        values[key]
    }

    mutating func set(_ key: String, _ value: ConfigValue) {
        values[key] = value
    }

    func jsonData() -> Data? {
        var object: [String: Any] = [:]
        for field in schema.fields {
            switch values[field.key] {
            case .boolean(let value):
                object[field.key] = value
            case .integer(let value):
                object[field.key] = value
            case .text(let value):
                object[field.key] = value
            case nil:
                break
            }
        }
        return try? JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
    }

    // Returns the config JSON with `model_path` filled in when it is missing or
    // empty. The fcitx5 addon keeps the effective model path in its config too
    // (falling back to the installed model), so both frontends show and save
    // the same value.
    static func fillingModelPath(_ json: String?, with path: String?) -> String? {
        guard let path, !path.isEmpty else { return json }
        var object: [String: Any] = [:]
        if let json, let data = json.data(using: .utf8),
           let parsed = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            object = parsed
        }
        if let existing = object["model_path"] as? String, !existing.isEmpty { return json }
        object["model_path"] = path
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
              let filled = String(data: data, encoding: .utf8) else {
            return json
        }
        return filled
    }
}
