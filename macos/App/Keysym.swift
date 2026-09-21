import Cocoa

// AppKit adapter for the platform-independent KeyTranslation.
enum Keysym {
    static func symbol(for event: NSEvent) -> UInt32 {
        KeyTranslation.symbol(keyCode: event.keyCode,
                              charactersIgnoringModifiers: event.charactersIgnoringModifiers)
    }

    static func modifiers(for event: NSEvent) -> KeyModifiers {
        let flags = event.modifierFlags
        var modifiers = KeyModifiers()
        modifiers.shift = flags.contains(.shift)
        modifiers.capsLock = flags.contains(.capsLock)
        modifiers.control = flags.contains(.control)
        modifiers.option = flags.contains(.option)
        modifiers.command = flags.contains(.command)
        return modifiers
    }
}
