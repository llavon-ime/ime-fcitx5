#include "atspi/caret_prefix_sampler.hpp"

#include <atspi/atspi.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool connect_unix(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return rc == 0;
}

}  // namespace

// dbind's fatal g_error cannot be neutralized, so before calling libatspi we
// test whether the a11y bus socket is actually reachable. A stale socket left
// behind by a dead at-spi-registry would otherwise make libatspi abort the
// process. Returns the socket path when reachable, or an empty string.
std::string reachable_bus_socket() {
    const char* address = getenv("AT_SPI_BUS_ADDRESS");
    if (address == nullptr) {
        // libatspi reads the bus address from the org.a11y.Bus service; the
        // common fallback is the per-user launcher directory. Scan it for a
        // socket we can actually connect to (a stale socket left by a dead
        // registry is refused and must not crash libatspi).
        const std::string dir = "/run/user/" + std::to_string(static_cast<long long>(getuid())) + "/at-spi";
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            const auto candidate = entry.path().string();
            if (connect_unix(candidate)) return candidate;
        }
        return {};
    }
    std::string path(address);
    if (path.rfind("unix:path=", 0) == 0) path = path.substr(10);
    return connect_unix(path) ? path : std::string();
}

gint idle_source = 0;

void print_snapshot(AtspiAccessible* obj, const char* why) {
    AtspiText* text = atspi_accessible_get_text_iface(obj);
    if (text == nullptr) {
        std::printf("[%s] no text interface\n", why);
        return;
    }

    GError* error = nullptr;
    const gint caret = atspi_text_get_caret_offset(text, &error);
    if (error != nullptr) {
        std::printf("[%s] caret query failed: %s\n", why, error->message);
        g_error_free(error);
        return;
    }
    const gint length = atspi_text_get_character_count(text, &error);
    if (error != nullptr) {
        std::printf("[%s] length query failed: %s\n", why, error->message);
        g_error_free(error);
        return;
    }

    // Keep the sample bounded: only request up to the caret plus margin, and
    // cap the request so a huge document cannot stall the a11y bus.
    const gint request_end = caret < 0 ? 0 : caret;
    gchar* raw = atspi_text_get_text(text, 0, request_end, &error);
    if (error != nullptr) {
        std::printf("[%s] text query failed: %s\n", why, error->message);
        g_error_free(error);
        return;
    }
    std::printf("[%s] caret=%d length=%d\n", why, caret, length);

    ime::fcitx5::CaretPrefixSampler sampler(512);
    std::u16string utf16;
    if (raw != nullptr) {
        for (const char* p = raw; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            utf16.push_back(static_cast<char16_t>(c));
        }
        g_free(raw);
    }
    sampler.set_text(std::move(utf16), caret < 0 ? 0 : static_cast<size_t>(caret));

    if (!sampler.usable()) {
        std::printf("[%s] sample not usable (sampled=%d)\n", why, sampler.sampled() ? 1 : 0);
        return;
    }
    const std::u16string& prefix = sampler.text_before_caret();
    const size_t show = prefix.size() < 120 ? prefix.size() : 120;
    std::string ascii;
    for (size_t i = 0; i < show; ++i) {
        const char16_t ch = prefix[i];
        ascii.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    std::printf("[%s] prefix units=%zu window_start=%zu sample=\"%s%s\"\n", why, prefix.size(),
                sampler.window_start(), ascii.c_str(), show < prefix.size() ? "..." : "");
}

void snapshot_focused_object() {
    int count = atspi_get_desktop_count();
    for (int i = 0; i < count; ++i) {
        AtspiAccessible* desktop = atspi_get_desktop(i);
        if (desktop == nullptr) continue;
        const int children = atspi_accessible_get_child_count(desktop, nullptr);
        for (int c = 0; c < children; ++c) {
            GError* error = nullptr;
            AtspiAccessible* app = atspi_accessible_get_child_at_index(desktop, c, &error);
            if (app == nullptr || error != nullptr) {
                g_clear_error(&error);
                continue;
            }
            AtspiStateSet* states = atspi_accessible_get_state_set(app);
            if (atspi_state_set_contains(states, ATSPI_STATE_FOCUSED)) {
                print_snapshot(app, "focused-app");
                return;
            }
        }
    }
    std::printf("[poll] no focused application\n");
}

// The a11y bus is single-threaded; perform D-Bus queries from an idle callback
// after the event dispatch has unwound so we never block the event stream.
gboolean handle_snapshot(gpointer user_data) {
    (void)user_data;
    idle_source = 0;
    snapshot_focused_object();
    return G_SOURCE_REMOVE;
}

void queue_snapshot() {
    if (idle_source != 0) return;
    idle_source = g_idle_add(handle_snapshot, nullptr);
}

void on_event(const AtspiEvent* event, void* user_data) {
    (void)user_data;
    if (event == nullptr || event->type == nullptr) return;
    const std::string type(event->type);
    if (type == "object:state-changed:focused" || type == "object:text-caret-moved" ||
        type.find("object:text-changed") == 0) {
        std::printf("[event] %s\n", event->type);
        queue_snapshot();
    }
}

int main(int argc, char**) {
    if (argc > 1) {
        snapshot_focused_object();
        return 0;
    }

    // dbind treats a failed a11y-bus connection as a fatal g_error and aborts
    // the process, so verify the bus socket is reachable before touching
    // libatspi. This also covers stale sockets left by a dead registry.
    if (reachable_bus_socket().empty()) {
        std::printf("no live accessibility bus socket; start at-spi-bus-launcher (is the a11y stack running?)\n");
        return 2;
    }

    (void)atspi_init();

    if (!atspi_is_initialized()) {
        std::printf("no accessibility bus available (is at-spi-bus-launcher running?)\n");
        return 2;
    }
    atspi_set_timeout(3000, 3000);

    AtspiEventListener* listener =
        atspi_event_listener_new_simple([](const AtspiEvent* e) { on_event(e, nullptr); }, nullptr);
    GError* error = nullptr;
    if (!atspi_event_listener_register(listener, "object:state-changed:focused", &error) ||
        !atspi_event_listener_register(listener, "object:text-caret-moved", &error) ||
        !atspi_event_listener_register(listener, "object:text-changed", &error)) {
        std::printf("listener registration failed: %s\n", error ? error->message : "?");
        g_clear_error(&error);
        return 3;
    }
    std::printf("listening for focus / caret / text events; move your caret in an editor\n");

    snapshot_focused_object();
    atspi_event_main();
    atspi_exit();
    return 0;
}
