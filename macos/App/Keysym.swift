import Cocoa

// Maps AppKit key events onto the X11 keysyms and modifier bits the engine's
// InputKey routes on. Printable keys use the layout-aware character; the
// remaining table covers the physical keys the engine interprets.
enum Keysym {
    static func symbol(for event: NSEvent) -> UInt32 {
        switch event.keyCode {
        case 0x24: return 0xff0d // Return
        case 0x4c: return 0xff8d // Keypad Enter
        case 0x30: return 0xff09 // Tab
        case 0x31: return 0x20 // Space
        case 0x33: return 0xff08 // Delete (backspace)
        case 0x75: return 0xffff // Forward delete
        case 0x35: return 0xff1b // Escape
        case 0x39: return 0xffe5 // Caps Lock
        case 0x47: return 0xff0b // Clear
        case 0x72: return 0xff63 // Help / Insert
        case 0x73: return 0xff50 // Home
        case 0x77: return 0xff57 // End
        case 0x74: return 0xff55 // Page Up
        case 0x79: return 0xff56 // Page Down
        case 0x7b: return 0xff51 // Left
        case 0x7c: return 0xff53 // Right
        case 0x7d: return 0xff54 // Down
        case 0x7e: return 0xff52 // Up
        // Keypad
        case 0x41: return 0xffae // KP_Decimal
        case 0x43: return 0xffaa // KP_Multiply
        case 0x45: return 0xffab // KP_Add
        case 0x4b: return 0xffaf // KP_Divide
        case 0x4e: return 0xffad // KP_Subtract
        case 0x51: return 0xffbd // KP_Equal
        case 0x52: return 0xffb0 // KP_0
        case 0x53: return 0xffb1 // KP_1
        case 0x54: return 0xffb2 // KP_2
        case 0x55: return 0xffb3 // KP_3
        case 0x56: return 0xffb4 // KP_4
        case 0x57: return 0xffb5 // KP_5
        case 0x58: return 0xffb6 // KP_6
        case 0x59: return 0xffb7 // KP_7
        case 0x5b: return 0xffb8 // KP_8
        case 0x5c: return 0xffb9 // KP_9
        default: break
        }
        guard let scalar = event.charactersIgnoringModifiers?.unicodeScalars.first else { return 0 }
        return scalar.value
    }

    // X11 modifier bits, matching InputKeyState.
    static func states(for event: NSEvent) -> UInt32 {
        let flags = event.modifierFlags
        var states: UInt32 = 0
        if flags.contains(.shift) { states |= 1 << 0 } // Shift
        if flags.contains(.capsLock) { states |= 1 << 1 } // CapsLock
        if flags.contains(.control) { states |= 1 << 2 } // Control
        if flags.contains(.option) { states |= 1 << 3 } // Mod1 / Alt
        if flags.contains(.command) { states |= 1 << 6 } // Super
        return states
    }
}
