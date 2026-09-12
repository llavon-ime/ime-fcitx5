#include "atspi/accessibility_context.hpp"

#include "text/utf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifdef IME_FCITX5_HAVE_ATSPI
#include <atspi/atspi.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#endif

namespace ime::fcitx5 {

class AccessibilityContextProvider::Impl {
public:
    explicit Impl(size_t max_code_units) : max_code_units_(max_code_units) {}

    void publish(std::u16string text, bool usable) {
        std::lock_guard lock(mutex_);
        sample_.text = std::move(text);
        sample_.usable = usable;
        sample_.sequence = ++next_sequence_;
        has_sample_ = true;
    }

    std::optional<AccessibilityContextSample> latest() const {
        std::lock_guard lock(mutex_);
        if (!has_sample_) return std::nullopt;
        return sample_;
    }

    std::uint64_t sequence() const {
        std::lock_guard lock(mutex_);
        return sample_.sequence;
    }

    bool running() const noexcept { return running_.load(); }

    bool start() {
        if (running_.load()) return true;
        stop();
        if (const char* disabled = std::getenv("IME_FCITX5_DISABLE_ATSPI"); disabled != nullptr && disabled[0] != '\0') {
            return false;
        }
        if (const char* file = std::getenv("IME_FCITX5_ATSPI_SAMPLE_FILE"); file != nullptr && file[0] != '\0') {
            sample_file_ = file;
            running_.store(true);
            refresh_file();
            return true;
        }
#ifdef IME_FCITX5_HAVE_ATSPI
        return start_backend();
#else
        return false;
#endif
    }

    void stop() {
        running_.store(false);
#ifdef IME_FCITX5_HAVE_ATSPI
        if (backend_thread_.joinable()) {
            if (loop_ != nullptr) g_main_loop_quit(loop_);
            backend_thread_.join();
        }
#endif
    }

    void set_active(bool active) {
        if (active_.exchange(active) == active) return;
        if (!active) publish(std::u16string(), false);
    }

    bool active() const noexcept { return active_.load(); }

    void refresh() {
        if (!running_.load() || !active_.load()) return;
        if (!sample_file_.empty()) {
            refresh_file();
            return;
        }
#ifdef IME_FCITX5_HAVE_ATSPI
        queue_idle();
#endif
    }

private:
    void refresh_file() {
        std::ifstream input(sample_file_, std::ios::binary);
        if (!input) {
            publish(std::u16string(), false);
            return;
        }
        const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        try {
            publish(utf8_prefix_tail(raw, raw.size(), max_code_units_), true);
        } catch (const std::exception&) {
            publish(std::u16string(), false);
        }
    }

    std::size_t max_code_units_;
    std::string sample_file_;
    mutable std::mutex mutex_;
    AccessibilityContextSample sample_;
    bool has_sample_ = false;
    std::uint64_t next_sequence_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};

#ifdef IME_FCITX5_HAVE_ATSPI
    static inline Impl* active_instance_ = nullptr;

    static bool connect_unix(const std::string& path) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return false;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::close(fd);
        return rc == 0;
    }

    static std::string reachable_bus_socket() {
        const char* address = std::getenv("AT_SPI_BUS_ADDRESS");
        if (address == nullptr) {
            const std::string dir =
                "/run/user/" + std::to_string(static_cast<long long>(::getuid())) + "/at-spi";
            std::error_code ec;
            std::filesystem::directory_iterator it(dir, ec);
            if (ec) return {};
            for (const auto& entry : it) {
                const auto candidate = entry.path().string();
                if (connect_unix(candidate)) return candidate;
            }
            return {};
        }
        std::string path(address);
        if (path.rfind("unix:path=", 0) == 0) path = path.substr(10);
        return connect_unix(path) ? path : std::string();
    }

    static bool is_password_text(AtspiAccessible* object) {
        GError* error = nullptr;
        const AtspiRole role = atspi_accessible_get_role(object, &error);
        g_clear_error(&error);
        return role == ATSPI_ROLE_PASSWORD_TEXT;
    }

    static bool has_text_interface(AtspiAccessible* object) {
        AtspiText* text = atspi_accessible_get_text_iface(object);
        if (text == nullptr) return false;
        g_object_unref(text);
        return true;
    }

    AtspiAccessible* find_focused_text(AtspiAccessible* root, int depth, int* visits) {
        if (root == nullptr || depth > kMaxTreeDepth || *visits >= kMaxTreeVisits) return nullptr;
        ++*visits;

        AtspiStateSet* states = atspi_accessible_get_state_set(root);
        const bool focused = states != nullptr && atspi_state_set_contains(states, ATSPI_STATE_FOCUSED);
        if (states != nullptr) g_object_unref(states);
        if (focused && has_text_interface(root) && !is_password_text(root)) return g_object_ref(root);

        const int children = atspi_accessible_get_child_count(root, nullptr);
        for (int i = 0; i < children; ++i) {
            GError* error = nullptr;
            AtspiAccessible* child = atspi_accessible_get_child_at_index(root, i, &error);
            g_clear_error(&error);
            if (child == nullptr) continue;
            AtspiAccessible* found = find_focused_text(child, depth + 1, visits);
            g_object_unref(child);
            if (found != nullptr) return found;
        }
        return nullptr;
    }

    AtspiAccessible* focused_text_object() {
        int visits = 0;
        const int count = atspi_get_desktop_count();
        for (int i = 0; i < count; ++i) {
            AtspiAccessible* desktop = atspi_get_desktop(i);
            if (desktop == nullptr) continue;
            if (AtspiAccessible* found = find_focused_text(desktop, 0, &visits)) return found;
        }
        return nullptr;
    }

    void publish_from_object(AtspiAccessible* object) {
        if (object == nullptr || is_password_text(object)) {
            publish(std::u16string(), false);
            return;
        }
        AtspiText* text = atspi_accessible_get_text_iface(object);
        if (text == nullptr) {
            publish(std::u16string(), false);
            return;
        }

        GError* error = nullptr;
        const gint caret = atspi_text_get_caret_offset(text, &error);
        if (error != nullptr || caret < 0) {
            g_clear_error(&error);
            g_object_unref(text);
            publish(std::u16string(), false);
            return;
        }

        const gint window =
            static_cast<gint>(std::min<size_t>(max_code_units_, static_cast<size_t>(kMaxRequestCharacters)));
        const gint start = caret > window ? caret - window : 0;
        gchar* raw = atspi_text_get_text(text, start, caret, &error);
        std::u16string sample;
        bool usable = false;
        if (error == nullptr && raw != nullptr) {
            try {
                sample = utf8_prefix_tail(raw, static_cast<size_t>(caret - start), max_code_units_);
                usable = true;
            } catch (const std::exception&) {
                usable = false;
            }
        }
        if (raw != nullptr) g_free(raw);
        g_clear_error(&error);
        g_object_unref(text);
        publish(std::move(sample), usable);
    }

    void queue_idle() {
        GMainContext* context = context_;
        if (context == nullptr || idle_source_.load() != 0) return;
        GSource* source = g_idle_source_new();
        g_source_set_callback(source, &Impl::on_idle, this, nullptr);
        idle_source_.store(g_source_attach(source, context));
        g_source_unref(source);
    }

    static gboolean on_idle(gpointer data) {
        auto* self = static_cast<Impl*>(data);
        self->idle_source_.store(0);
        if (!self->active_.load()) {
            if (self->pending_source_ != nullptr) {
                g_object_unref(self->pending_source_);
                self->pending_source_ = nullptr;
            }
            return G_SOURCE_REMOVE;
        }
        if (self->pending_source_ != nullptr) {
            AtspiAccessible* source = self->pending_source_;
            self->pending_source_ = nullptr;
            self->publish_from_object(source);
            g_object_unref(source);
        } else {
            AtspiAccessible* focused = self->focused_text_object();
            self->publish_from_object(focused);
            if (focused != nullptr) g_object_unref(focused);
        }
        return G_SOURCE_REMOVE;
    }

    static void on_event(const AtspiEvent* event) {
        Impl* self = active_instance_;
        if (self == nullptr || event == nullptr || event->type == nullptr) return;
        if (!self->active_.load()) return;
        const std::string type(event->type);
        if (type != "object:state-changed:focused" && type != "object:text-caret-moved" &&
            type.find("object:text-changed") != 0) {
            return;
        }
        // A focus loss reports the widget being left behind; sampling it would
        // publish the previous field's text with a fresh sequence right after
        // the engine switched contexts.
        if (type == "object:state-changed:focused" && event->detail1 == 0) {
            if (event->source != nullptr && has_text_interface(event->source)) {
                if (self->pending_source_ != nullptr) {
                    g_object_unref(self->pending_source_);
                    self->pending_source_ = nullptr;
                }
                self->publish(std::u16string(), false);
            }
            return;
        }
        if (event->source != nullptr && has_text_interface(event->source)) {
            if (is_password_text(event->source)) {
                self->publish(std::u16string(), false);
                return;
            }
            if (self->pending_source_ != nullptr) g_object_unref(self->pending_source_);
            self->pending_source_ = static_cast<AtspiAccessible*>(g_object_ref(event->source));
        }
        self->queue_idle();
    }

    void run() {
        context_ = g_main_context_new();
        g_main_context_push_thread_default(context_);
        bool ok = false;
        AtspiEventListener* listener = nullptr;
        do {
            if (reachable_bus_socket().empty()) break;
            atspi_init();
            if (!atspi_is_initialized()) break;
            atspi_set_timeout(1000, 1000);

            GError* error = nullptr;
            listener = atspi_event_listener_new_simple(&Impl::on_event, nullptr);
            if (!atspi_event_listener_register(listener, "object:state-changed:focused", &error) ||
                !atspi_event_listener_register(listener, "object:text-caret-moved", &error) ||
                !atspi_event_listener_register(listener, "object:text-changed", &error)) {
                g_clear_error(&error);
                break;
            }
            loop_ = g_main_loop_new(context_, FALSE);
            ok = loop_ != nullptr;
        } while (false);

        {
            std::lock_guard lock(ready_mutex_);
            ready_ = true;
            ready_ok_ = ok;
        }
        ready_cv_.notify_all();
        if (!ok) {
            if (listener != nullptr) g_object_unref(listener);
            g_main_context_pop_thread_default(context_);
            g_main_context_unref(context_);
            context_ = nullptr;
            running_.store(false);
            return;
        }

        active_instance_ = this;
        queue_idle();
        g_main_loop_run(loop_);
        active_instance_ = nullptr;
        if (pending_source_ != nullptr) {
            g_object_unref(pending_source_);
            pending_source_ = nullptr;
        }
        g_object_unref(listener);
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        atspi_exit();
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
        context_ = nullptr;
        running_.store(false);
    }

    bool start_backend() {
        {
            std::lock_guard lock(ready_mutex_);
            ready_ = false;
            ready_ok_ = false;
        }
        running_.store(true);
        backend_thread_ = std::thread([this]() { run(); });

        std::unique_lock lock(ready_mutex_);
        if (!ready_cv_.wait_for(lock, std::chrono::seconds(5), [this]() { return ready_; })) {
            running_.store(false);
        }
        return ready_ok_;
    }

    static constexpr int kMaxTreeDepth = 32;
    static constexpr int kMaxTreeVisits = 20000;
    static constexpr gint kMaxRequestCharacters = 8192;

    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    std::thread backend_thread_;
    std::atomic<guint> idle_source_{0};
    AtspiAccessible* pending_source_ = nullptr;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool ready_ = false;
    bool ready_ok_ = false;
#endif
};

AccessibilityContextProvider::AccessibilityContextProvider(size_t max_code_units)
    : impl_(std::make_unique<Impl>(max_code_units)) {}

AccessibilityContextProvider::~AccessibilityContextProvider() { stop(); }

bool AccessibilityContextProvider::start() { return impl_->start(); }

void AccessibilityContextProvider::stop() { impl_->stop(); }

bool AccessibilityContextProvider::running() const noexcept { return impl_->running(); }

void AccessibilityContextProvider::set_active(bool active) { impl_->set_active(active); }

bool AccessibilityContextProvider::active() const noexcept { return impl_->active(); }

void AccessibilityContextProvider::refresh() { impl_->refresh(); }

void AccessibilityContextProvider::publish(std::u16string text, bool usable) {
    impl_->publish(std::move(text), usable);
}

std::optional<AccessibilityContextSample> AccessibilityContextProvider::latest() const { return impl_->latest(); }

std::uint64_t AccessibilityContextProvider::sequence() const { return impl_->sequence(); }

}  // namespace ime::fcitx5
