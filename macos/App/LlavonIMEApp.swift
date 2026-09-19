import Cocoa
import InputMethodKit

@main
struct LlavonIMEApp {
    // NSApplication.delegate is not retaining; keep the delegate alive for the
    // whole process.
    private static let delegate = LlavonAppDelegate()

    static func main() {
        let application = NSApplication.shared
        application.delegate = delegate
        application.run()
    }
}

final class LlavonAppDelegate: NSObject, NSApplicationDelegate {
    private var server: IMKServer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        EngineBridge.shared.startResolved()

        let identifier = Bundle.main.bundleIdentifier ?? "com.llavon.ime"
        let connectionName = Bundle.main.infoDictionary?["InputMethodConnectionName"] as? String
            ?? "\(identifier)_Connection"
        server = IMKServer(name: connectionName, bundleIdentifier: identifier)
        NSLog("llavon-ime: input method server started on \(connectionName)")
    }

    func applicationWillTerminate(_ notification: Notification) {
        server = nil
        EngineBridge.shared.stop()
    }
}
