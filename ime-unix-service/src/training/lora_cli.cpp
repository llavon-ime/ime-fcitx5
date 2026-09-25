#include "commit_store.hpp"
#include "numeric_dataset.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>


namespace fs = std::filesystem;

namespace {

using Options = std::map<std::string, std::string>;

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (!name.starts_with("--") || i + 1 >= argc || !options.emplace(name, argv[i + 1]).second)
            throw std::invalid_argument("expected unique --option value pairs");
        ++i;
    }
    return options;
}

std::string require(const Options& options, const char* key) {
    const auto found = options.find(key);
    if (found == options.end() || found->second.empty()) throw std::invalid_argument(std::string("missing ") + key);
    return found->second;
}

std::string optional(const Options& options, const char* key, const char* fallback) {
    const auto found = options.find(key);
    return found == options.end() ? fallback : found->second;
}

int integer_option(const Options& options, const char* key, int fallback, int minimum, int maximum) {
    const auto text = optional(options, key, "");
    if (text.empty()) return fallback;
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return value;
}

double real_option(const Options& options, const char* key, double fallback, double minimum, double maximum) {
    const auto text = optional(options, key, "");
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return value;
}

// Passwords never travel in argv, where any local process could read them.
// They come from an environment variable, a private file, or the terminal.
std::string password_option(const Options& options) {
    if (options.contains("--password-env")) {
        const char* value = std::getenv(require(options, "--password-env").c_str());
        if (value == nullptr || *value == '\0') throw std::runtime_error("password environment variable is not set");
        return value;
    }
    if (options.contains("--password-file")) {
        std::ifstream input(require(options, "--password-file"));
        if (!input) throw std::runtime_error("cannot read password file");
        std::string line;
        std::getline(input, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) throw std::runtime_error("password file is empty");
        return line;
    }
    if (options.contains("--password-fd")) {
        const auto text = require(options, "--password-fd");
        int fd = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), fd);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || fd < 3)
            throw std::invalid_argument("invalid --password-fd");
        std::string line;
        char ch = 0;
        while (true) {
            const auto count = ::read(fd, &ch, 1);
            if (count == 1) { if (ch == '\n') break; line.push_back(ch); continue; }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        ::close(fd);
        if (line.empty()) throw std::runtime_error("password was not provided");
        return line;
    }
    if (::isatty(STDIN_FILENO)) {
        std::cerr << "password: " << std::flush;
        termios original{};
        const bool hidden = ::tcgetattr(STDIN_FILENO, &original) == 0;
        if (hidden) {
            termios masked = original;
            masked.c_lflag &= ~ECHO;
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &masked);
        }
        std::string line;
        std::getline(std::cin, line);
        if (hidden) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
        if (line.empty()) throw std::runtime_error("password required");
        return line;
    }
    throw std::runtime_error("password required; pass --password-env or --password-file");
}

// A locked cipher still serves legacy plaintext rows; a configured password
// always demands a matching one.
void unlock(ime::unix_service::CommitCipher& cipher, sqlite3* db, const Options& options) {
    if (ime::unix_service::read_commit_protection(db).configured) cipher.unlock(db, password_option(options));
}

std::string as_argument(double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

bool has_pinned_trainer_stamp(const fs::path& executable) {
    if (!fs::is_regular_file(executable)) return false;
    try {
        return nlohmann::json::parse(std::ifstream(executable.parent_path() / "trainer-release.json")).at("commit")
               == LLAVON_IME_LORA_PINNED_COMMIT;
    } catch (...) { return false; }
}

// The trainer archive is a directory of the executable plus TorchSharp's
// native libraries. Installing only the executable leaves TorchSharp failing
// at initialization, so every candidate must carry them.
bool has_trainer_libraries(const fs::path& directory, int depth = 0) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".so") || name.find(".so.") != std::string::npos || name.ends_with(".dylib")) return true;
        if (depth < 3 && entry.is_directory() && has_trainer_libraries(entry.path(), depth + 1)) return true;
    }
    return false;
}

bool trainer_usable(const fs::path& executable) {
    return fs::is_regular_file(executable) && has_pinned_trainer_stamp(executable) &&
           has_trainer_libraries(executable.parent_path());
}

// The release pinned by the submodule is installed either by the GUI/CLI into
// the per-user state directory or by the install scripts into the system
// directory. Prefer whichever carries the matching release stamp, then fall
// back to the per-user path so its update flow stays the default.
fs::path installed_trainer(const fs::path& db_path) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH"); override && *override)
        return fs::absolute(override);
    const fs::path managed = db_path.parent_path() / "tools" / "lora" / "llavon-lora";
    const fs::path system = LLAVON_IME_INSTALLED_LORA_TRAINER_PATH;
    if (trainer_usable(managed)) return managed;
    if (trainer_usable(system)) return system;
    return fs::is_regular_file(managed) ? managed : system;
}

class Database {
public:
    explicit Database(const fs::path& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "cannot open training database";
            sqlite3_close(db_); throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db_, 3000);
        exec("PRAGMA foreign_keys=ON");
        ime::unix_service::initialize_commit_database(db_);
    }
    ~Database() { sqlite3_close(db_); }
    sqlite3* get() const { return db_; }
    void exec(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    }
private:
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    int next() {
        const auto status = sqlite3_step(stmt_);
        if (status != SQLITE_ROW && status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return status;
    }
    const char* text(int index) const { return reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index)); }
    sqlite3_stmt* get() const { return stmt_; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void run(const fs::path& program, const std::vector<std::string>& args) {
    if (!fs::is_regular_file(program)) throw std::runtime_error("trainer executable not found: " + program.string());
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot fork trainer");
    if (pid == 0) {
        std::vector<std::string> owned{program.string()};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for trainer");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("trainer failed (exit status " +
                                 std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
}

void run_tool(const char* program, const std::vector<std::string>& args, const fs::path& stdout_file = {}) {
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot fork external tool");
    if (pid == 0) {
        if (!stdout_file.empty()) {
            const int fd = ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) _exit(127);
            ::close(fd);
        }
        std::vector<std::string> owned{program};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execvp(program, argv.data());
        _exit(127);
    }
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for external tool");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error(std::string(program) + " failed");
}

std::string sha256_hex(const fs::path& file) {
    auto hash_file = file; hash_file += ".sha256";
    try {
#ifdef __APPLE__
        run_tool("shasum", {"-a", "256", file.string()}, hash_file);
#else
        run_tool("sha256sum", {file.string()}, hash_file);
#endif
        std::ifstream input(hash_file);
        std::string actual;
        input >> actual;
        fs::remove(hash_file);
        return actual;
    } catch (...) { fs::remove(hash_file); throw; }
}

bool sha256_matches(const fs::path& file, const std::string& expected) {
    return sha256_hex(file) == expected;
}

// Downloaded trainer archives are kept so repeat installs (packaging, CI, or
// another target directory) do not fetch the 158 MB release again. The archive
// is still verified against the manifest SHA-256 before it is reused.
fs::path trainer_cache_dir() {
    if (const char* override = std::getenv("LLAVON_IME_LORA_TRAINER_CACHE"); override && *override)
        return fs::absolute(override);
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return fs::path(xdg) / "llavon-ime" / "lora-trainer";
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return fs::path(home) / ".cache" / "llavon-ime" / "lora-trainer";
}

bool archived_trainer(const fs::path& archive, const std::string& hash, std::uint64_t size) {
    return fs::is_regular_file(archive) && fs::file_size(archive) == size && sha256_matches(archive, hash);
}

// The rolling `latest` manifest only tracks the newest trainer release, so a
// pinned submodule commit falls off it as soon as the trainer cuts a newer
// version. Look the release that actually declares the pinned commit up
// through the public release listing instead of waiting for a manifest that
// will never match. The immutable manifest is still the authority: the caller
// re-validates the version, commit, asset name, and SHA-256.
nlohmann::json trainer_release_for_commit(const fs::path& output, std::string_view expected_commit) {
    const auto listing = output / "releases.json.partial";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3",
                          "--header", "Accept: application/vnd.github+json",
                          "--header", "Cache-Control: no-cache",
                          "--output", listing.string(),
                          "https://api.github.com/repos/llavon-ime/lora-trainer/releases?per_page=50"});
        const auto releases = nlohmann::json::parse(std::ifstream(listing));
        fs::remove(listing);
        if (!releases.is_array()) return nlohmann::json::object();
        for (const auto& release : releases) {
            const auto target = release.value("target_commitish", "");
            if (target.empty() || std::string_view(target).rfind(expected_commit, 0) != 0) continue;
            const auto tag = release.value("tag_name", "");
            if (tag.rfind("v", 0) != 0 || tag.size() < 2 ||
                tag.find_first_not_of("0123456789.", 1) != std::string::npos) continue;
            return nlohmann::json{{"schema", 1}, {"trainerApi", 1}, {"version", tag.substr(1)},
                                  {"commit", std::string(expected_commit)}};
        }
    } catch (...) {
        // The listing is an optimisation for pinned commits that fell off the
        // rolling manifest; a rate limit or network error still produces the
        // caller's pinned-commit diagnosis.
    }
    fs::remove(listing);
    return nlohmann::json::object();
}

void check_model(const fs::path& output) {
    fs::create_directories(output);
    const auto metadata = output / "metadata.partial";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", metadata.string(),
                          "https://huggingface.co/api/models/tony65535/llavon-ime-llama-250m/revision/main"});
        const auto remote = nlohmann::json::parse(std::ifstream(metadata)).at("sha").get<std::string>();
        if (remote.size() != 40 || remote.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid Hugging Face model revision");
        std::string local;
        std::ifstream current(output / "current.revision");
        current >> local;
        const bool complete = local.size() == 40 && fs::is_regular_file(output / local / "config.json") &&
            fs::is_regular_file(output / local / "ime_vocab.json") &&
            fs::is_regular_file(output / local / "model.safetensors");
        std::cout << "revision=" << remote << " installed=" << (complete ? local : "none")
                  << " update-available=" << (!complete || local != remote ? "true" : "false") << '\n';
    } catch (...) { fs::remove(metadata); throw; }
    fs::remove(metadata);
}

void install_trainer(const fs::path& output) {
    // Every platform declares the name so the function still compiles where no
    // release exists (macOS x86_64); the check below reports those.
    constexpr std::string_view platform =
#if defined(__linux__) && defined(__x86_64__)
        "linux-x64-cpu";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        "osx-arm64-cpu";
#else
        "";
#endif
    if (platform.empty())
        throw std::runtime_error("LoRA Trainer release is unavailable for this platform");
    fs::create_directories(output);
    const auto manifest_file = output / "release.json.partial";
    const auto pinned_manifest_file = output / "pinned-release.json.partial";
    const auto archive = output / "trainer.tar.gz.partial";
    const auto staging = output / "trainer.staging";
    fs::remove_all(staging);
    try {
        constexpr std::string_view expected_commit = LLAVON_IME_LORA_PINNED_COMMIT;
        // An intact installation of the pinned release is left alone: the
        // stamp records the executable SHA-256, so a repeat install neither
        // downloads nor re-extracts anything. A stamp written before hashes
        // were recorded is upgraded in place instead of re-downloading.
        try {
            const auto stamp_path = output / "trainer-release.json";
            const auto stamp = nlohmann::json::parse(std::ifstream(stamp_path));
            const auto installed = output / "llavon-lora";
            if (stamp.at("commit") == expected_commit && fs::is_regular_file(installed) && has_trainer_libraries(output)) {
                const auto hash = sha256_hex(installed);
                const auto recorded = stamp.contains("sha256") ? stamp.at("sha256").get<std::string>() : std::string{};
                if (recorded.empty() || recorded == hash) {
                    if (recorded.empty()) {
                        std::ofstream file(stamp_path, std::ios::trunc);
                        file << nlohmann::json{{"version", stamp.at("version")}, {"commit", stamp.at("commit")},
                                               {"sha256", hash}}.dump() << '\n';
                    }
                    std::cout << "trainer=" << installed << " version=" << stamp.at("version").get<std::string>()
                              << " already-installed=true\n";
                    return;
                }
            }
        } catch (...) {}
        const std::string rolling_url = "https://github.com/llavon-ime/lora-trainer/releases/download/latest/latest.json";
        int attempts = 30;
        if (const char* setting = std::getenv("LLAVON_IME_LORA_RELEASE_ATTEMPTS")) {
            const std::string value(setting);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), attempts);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || attempts < 1 || attempts > 30)
                throw std::invalid_argument("invalid release retry count");
        }
        nlohmann::json candidate;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            try {
                run_tool("curl", {"--fail", "--location", "--retry", "3", "--header", "Cache-Control: no-cache",
                                  "--output", manifest_file.string(), rolling_url + "?commit=" +
                                  std::string(expected_commit) + "&attempt=" + std::to_string(attempt)});
                candidate = nlohmann::json::parse(std::ifstream(manifest_file));
                if (candidate.value("commit", "") == expected_commit) break;
                std::cerr << "Waiting for LoRA Trainer release of pinned commit " << expected_commit << '\n';
            } catch (const std::exception& error) {
                std::cerr << "Waiting for LoRA Trainer release: " << error.what() << '\n';
            }
            // The pinned commit falls off the rolling manifest as soon as the
            // trainer cuts a newer release, so consult the release listing
            // before sleeping on a manifest that may never match again.
            if (attempt == 1 || attempt % 5 == 0) {
                candidate = trainer_release_for_commit(output, expected_commit);
                if (candidate.value("commit", "") == expected_commit) break;
            }
            if (attempt < attempts) std::this_thread::sleep_for(std::chrono::seconds(30));
        }
        if (!candidate.is_object() || candidate.value("commit", "") != expected_commit)
            candidate = trainer_release_for_commit(output, expected_commit);
        if (!candidate.is_object() || candidate.value("commit", "") != expected_commit)
            throw std::runtime_error("no LoRA Trainer release for pinned submodule commit " + std::string(expected_commit));
        if (candidate.at("schema") != 1 || candidate.at("trainerApi") != 1)
            throw std::runtime_error("unsupported trainer release manifest");
        const std::string version = candidate.at("version").get<std::string>();
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", pinned_manifest_file.string(),
                          "https://github.com/llavon-ime/lora-trainer/releases/download/v" + version + "/latest.json"});
        const auto manifest = nlohmann::json::parse(std::ifstream(pinned_manifest_file));
        if (manifest.at("schema") != 1 || manifest.at("trainerApi") != 1 ||
            manifest.at("commit") != expected_commit || manifest.at("version") != version)
            throw std::runtime_error("immutable trainer release differs from pinned submodule commit");
        if (version.empty() || version.find_first_not_of("0123456789.") != std::string::npos)
            throw std::runtime_error("invalid trainer release version");
        const auto& asset = manifest.at("assets").at(std::string(platform));
        const std::string name = asset.at("name").get<std::string>();
        const std::string url = asset.at("url").get<std::string>();
        const std::string hash = asset.at("sha256").get<std::string>();
        if (name != "llavon-lora-" + version + "-" + std::string(platform) + ".tar.gz" ||
            url != "https://github.com/llavon-ime/lora-trainer/releases/download/v" + version + "/" + name ||
            hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid trainer release asset");
        const auto cache = trainer_cache_dir();
        const auto cached = cache.empty() ? fs::path{} : cache / name;
        const std::uint64_t size = asset.at("size").get<std::uint64_t>();
        // Reuse a verified archive when one is cached; only a cache miss
        // downloads the release again.
        fs::path source_archive = archive;
        if (!cached.empty() && archived_trainer(cached, hash, size)) {
            source_archive = cached;
        } else {
            run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", archive.string(), url});
            if (!archived_trainer(archive, hash, size))
                throw std::runtime_error("trainer download failed checksum verification");
            if (!cached.empty()) {
                fs::create_directories(cache);
                auto partial = cached; partial += ".partial";
                fs::remove(partial);
                fs::copy_file(archive, partial, fs::copy_options::overwrite_existing);
                fs::rename(partial, cached);
                source_archive = cached;
            }
        }
        // The release is a directory: the executable plus the TorchSharp
        // native libraries it loads at startup. Extract and validate the whole
        // tree, then publish it in place of the previous installation.
        fs::create_directories(staging);
        run_tool("tar", {"-xzf", source_archive.string(), "-C", staging.string()});
        const auto binary = staging / "llavon-lora";
        if (!fs::is_regular_file(binary)) throw std::runtime_error("trainer executable is missing from release");
        if (!has_trainer_libraries(staging))
            throw std::runtime_error("trainer release is missing its native libraries");
        if (::chmod(binary.c_str(), 0755) != 0) throw std::runtime_error("cannot mark the trainer executable");
        const auto version_file = output / "trainer-version.json.partial";
        run_tool(binary.c_str(), {"--version", "--json"}, version_file);
        const auto actual = nlohmann::json::parse(std::ifstream(version_file));
        fs::remove(version_file);
        // The release manifest currently reports API 1; verify the actual CLI.
        if (actual.at("trainerApi").get<int>() != 2)
            throw std::runtime_error("trainer API is incompatible (expected 2)");
        { std::ofstream file(staging / "trainer-release.json", std::ios::trunc);
          file << nlohmann::json{{"version", version}, {"commit", expected_commit},
                                 {"sha256", sha256_hex(binary)}}.dump() << '\n';
          if (!file) throw std::runtime_error("cannot store trainer release information"); }
        std::vector<fs::path> obsolete;
        for (const auto& entry : fs::directory_iterator(output)) obsolete.push_back(entry.path());
        for (const auto& path : obsolete) {
            const auto name = path.filename();
            if (name == archive.filename() || name == manifest_file.filename() ||
                name == pinned_manifest_file.filename() || name == staging.filename()) continue;
            fs::remove_all(path);
        }
        for (const auto& entry : fs::directory_iterator(staging))
            fs::rename(entry.path(), output / entry.path().filename());
        fs::remove_all(staging);
        std::cout << "trainer=" << (output / "llavon-lora") << " version=" << version << '\n';
    } catch (...) { fs::remove_all(staging); fs::remove(archive); fs::remove(manifest_file);
                    fs::remove(pinned_manifest_file); throw; }
    fs::remove(archive);
    fs::remove(manifest_file);
    fs::remove(pinned_manifest_file);
}

void fetch_model(const fs::path& output) {
    const fs::path metadata = output / "metadata.partial";
    fs::create_directories(output);
    const std::string repository = "tony65535/llavon-ime-llama-250m";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", metadata.string(),
                          "https://huggingface.co/api/models/" + repository + "/revision/main?blobs=true"});
        std::ifstream stream(metadata);
        const auto manifest = nlohmann::json::parse(stream);
        const std::string revision = manifest.at("sha").get<std::string>();
        if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid Hugging Face model revision");
        const auto destination = output / revision;
        fs::create_directories(destination);
        const auto& siblings = manifest.at("siblings");
        for (const auto name : {"config.json", "ime_vocab.json", "model.safetensors"}) {
            const auto found = std::find_if(siblings.begin(), siblings.end(),
                [&](const nlohmann::json& sibling) { return sibling.value("rfilename", "") == name; });
            if (found == siblings.end()) throw std::runtime_error("missing checkpoint asset");
            const auto size = found->at("size").get<std::uint64_t>();
            const auto file = destination / name;
            const bool weights = std::string_view(name) == "model.safetensors";
            const std::string expected_hash = weights ? found->at("lfs").at("sha256").get<std::string>() : "";
            if (fs::is_regular_file(file) && fs::file_size(file) == size &&
                (!weights || sha256_matches(file, expected_hash))) continue;
            auto partial = file; partial += ".partial";
            run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", partial.string(),
                              "https://huggingface.co/" + repository + "/resolve/" + revision + "/" + name});
            if (!fs::is_regular_file(partial) || fs::file_size(partial) != size)
                throw std::runtime_error(std::string("incomplete checkpoint asset: ") + name);
            if (weights && !sha256_matches(partial, expected_hash))
                throw std::runtime_error("checkpoint SHA-256 mismatch");
            fs::rename(partial, file);
        }
        std::ofstream current(output / "current.revision.partial");
        current << revision << '\n'; current.close();
        fs::rename(output / "current.revision.partial", output / "current.revision");
        fs::remove(metadata);
        std::cout << "model-dir=" << destination << " revision=" << revision << '\n';
    } catch (...) { fs::remove(metadata); throw; }
}

void list(sqlite3* db, const Options& options) {
    ime::unix_service::CommitCipher cipher;
    unlock(cipher, db, options);
    for (const auto& record : ime::unix_service::read_commits(db, "pending", cipher, 0, 1000000)) {
        std::string readings;
        for (const auto& reading : record.readings) {
            if (!readings.empty()) readings += ' ';
            readings += reading;
        }
        std::cout << nlohmann::json{{"id", record.id}, {"committed_at", record.committed_at},
                                    {"context", record.context}, {"answer", record.answer},
                                    {"readings", readings}}.dump() << '\n';
    }
}

void change_state(Database& db, std::string_view action, const std::string& id) {
    // Deleting overwrites the freed pages so the typed text does not linger.
    if (action == "delete") db.exec("PRAGMA secure_delete=ON");
    const char* sql = action == "exclude" ? "UPDATE commits SET state='excluded' WHERE id=? AND state='pending'" :
                      action == "delete" ? "DELETE FROM commits WHERE id=? AND state='pending'" : nullptr;
    if (!sql) throw std::invalid_argument("unknown record action");
    Statement query(db.get(), sql);
    query.bind(1, id);
    (void)query.next();
    if (sqlite3_changes(db.get()) != 1)
        throw std::runtime_error(action == "delete" ? "record not found or not pending"
                                                    : "record not found or not eligible for exclusion");
}

// Removes the readable training dataset (and any partial file) as soon as a
// run no longer needs it. Only these two application-owned files are touched.
void discard_plaintext_dataset(const fs::path& dataset) {
    std::error_code error;
    fs::remove(dataset, error);
    auto partial = dataset; partial += ".partial";
    fs::remove(partial, error);
}

void ensure_run_history(Database& db) {    std::set<std::string> columns;
    Statement info(db.get(), "PRAGMA table_info(lora_runs)");
    while (info.next() == SQLITE_ROW) columns.insert(info.text(1));
    for (const auto& [name, definition] : std::vector<std::pair<std::string, std::string>>{
             {"rank", "INTEGER NOT NULL DEFAULT 8"}, {"alpha", "REAL NOT NULL DEFAULT 16"},
             {"dropout", "REAL NOT NULL DEFAULT 0"}, {"target_modules", "TEXT NOT NULL DEFAULT 'q_proj,v_proj'"},
             {"optimizer_steps", "INTEGER NOT NULL DEFAULT 0"}, {"parent_id", "INTEGER"},
             {"cumulative_record_count", "INTEGER NOT NULL DEFAULT 0"}}) {
        if (!columns.contains(name)) db.exec(("ALTER TABLE lora_runs ADD COLUMN " + name + " " + definition).c_str());
    }
}

void publish_run(Database& db, const ime::unix_service::NumericDataset& dataset,
                 const fs::path& adapter, const fs::path& model, const std::string& revision,
                 int rank, double alpha, double dropout, const std::string& modules) {
    int steps = 0;
    if (fs::is_regular_file(adapter / "training_state.json"))
        steps = nlohmann::json::parse(std::ifstream(adapter / "training_state.json")).at("step").get<int>();
    sqlite3_int64 previous_id = 0, cumulative = 0;
    {
        Statement previous(db.get(), "SELECT id,(SELECT COALESCE(SUM(record_count),0) FROM lora_runs) FROM lora_runs ORDER BY id DESC LIMIT 1");
        if (previous.next() == SQLITE_ROW) {
            previous_id = sqlite3_column_int64(previous.get(), 0);
            cumulative = sqlite3_column_int64(previous.get(), 1);
        }
    }
    db.exec("BEGIN IMMEDIATE");
    try {
        Statement update(db.get(), "UPDATE commits SET state='trained' WHERE id=? AND state='pending'");
        for (const auto& id : dataset.included_ids) {
            sqlite3_reset(update.get()); sqlite3_clear_bindings(update.get());
            update.bind(1, id);
            (void)update.next();
            if (sqlite3_changes(db.get()) != 1) throw std::runtime_error("training records changed during export");
        }
        Statement insert(db.get(), "INSERT INTO lora_runs(base_revision,adapter_path,model_path,record_count,rank,alpha,dropout,target_modules,optimizer_steps,parent_id,cumulative_record_count) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
        insert.bind(1, revision); insert.bind(2, adapter.string()); insert.bind(3, model.string());
        sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(dataset.included_ids.size()));
        sqlite3_bind_int(insert.get(), 5, rank);
        sqlite3_bind_double(insert.get(), 6, alpha);
        sqlite3_bind_double(insert.get(), 7, dropout);
        insert.bind(8, modules);
        sqlite3_bind_int(insert.get(), 9, steps);
        if (previous_id) sqlite3_bind_int64(insert.get(), 10, previous_id);
        else sqlite3_bind_null(insert.get(), 10);
        sqlite3_bind_int64(insert.get(), 11, cumulative + static_cast<sqlite3_int64>(dataset.included_ids.size()));
        (void)insert.next();
        db.exec("COMMIT");
    } catch (...) { sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

void train(Database& db, const Options& options, const fs::path& model_dir, const fs::path& tables,
            const fs::path& output, const fs::path& db_path, const ime::unix_service::CommitCipher& cipher) {
    if (options.contains("--trainer"))
        throw std::invalid_argument("--trainer is no longer supported; use the pinned trainer installer");
    const fs::path trainer = installed_trainer(db_path);
    const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH");
    if (!override || !*override) {
        if (!has_pinned_trainer_stamp(trainer))
            throw std::runtime_error("installed trainer does not match the pinned submodule commit; reinstall the pinned trainer");
        if (!has_trainer_libraries(trainer.parent_path()))
            throw std::runtime_error("installed trainer is missing its native libraries; reinstall the pinned trainer");
    }
    std::string revision = require(options, "--revision");
    fs::path training_model_dir = model_dir;
    if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (model_dir.filename().string().size() == 40 && model_dir.filename() != revision))
        throw std::invalid_argument("--revision must match the pinned checkpoint directory");
    const std::string device = optional(options, "--device", "auto");
    if (device != "cpu" && device != "cuda" && device != "mps" && device != "auto")
        throw std::invalid_argument("invalid --device");
    const std::string dtype = optional(options, "--dtype", "float32");
    if (dtype != "float32" && dtype != "bfloat16") throw std::invalid_argument("invalid --dtype");
    const std::string modules = optional(options, "--target-modules", "q_proj,v_proj");
    if (modules.empty() || modules.find_first_not_of("abcdefghijklmnopqrstuvwxyz_,") != std::string::npos)
        throw std::invalid_argument("invalid --target-modules");
    const int max_length = integer_option(options, "--max-seq-length", 384, 2, 384);
    const int rank = integer_option(options, "--rank", 8, 1, INT_MAX);
    const double alpha = real_option(options, "--alpha", 16, std::numeric_limits<double>::denorm_min(),
                                     std::numeric_limits<double>::max());
    const double dropout = real_option(options, "--dropout", 0, 0, 1);
    if (dropout >= 1) throw std::invalid_argument("invalid --dropout");
    const int batch = integer_option(options, "--batch-size", 1, 1, INT_MAX);
    const int accumulation = integer_option(options, "--gradient-accumulation", 1, 1, INT_MAX);
    const int epochs = integer_option(options, "--epochs", 5, 1, INT_MAX);
    const int max_steps = integer_option(options, "--max-steps", -1, -1, INT_MAX);
    if (max_steps == 0) throw std::invalid_argument("invalid --max-steps");
    const double learning_rate = real_option(options, "--learning-rate", 0.0001,
                                             std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max());
    const double weight_decay = real_option(options, "--weight-decay", 0, 0, std::numeric_limits<double>::max());
    const int warmup = integer_option(options, "--warmup-steps", 0, 0, INT_MAX);
    const double norm = real_option(options, "--max-grad-norm", 1, 0, std::numeric_limits<double>::max());
    const int save_every = integer_option(options, "--save-every", 0, 0, INT_MAX);
    const int seed = integer_option(options, "--seed", 42, INT_MIN, INT_MAX);
    const int shuffle = integer_option(options, "--shuffle", 1, 0, 1);
    ensure_run_history(db);
    {
        Statement previous(db.get(), "SELECT base_revision,rank,alpha,dropout,target_modules FROM lora_runs ORDER BY id DESC LIMIT 1");
        if (previous.next() == SQLITE_ROW) {
            if (rank != sqlite3_column_int(previous.get(), 1) || alpha != sqlite3_column_double(previous.get(), 2) ||
                dropout != sqlite3_column_double(previous.get(), 3) || modules != previous.text(4))
                throw std::runtime_error("parameters differ from previous adapter");
            revision = previous.text(0);
            training_model_dir = model_dir.parent_path() / revision;
            if (!fs::is_regular_file(training_model_dir / "config.json") ||
                !fs::is_regular_file(training_model_dir / "ime_vocab.json") ||
                !fs::is_regular_file(training_model_dir / "model.safetensors"))
                throw std::runtime_error("previous adapter's base checkpoint is missing");
        }
    }
    std::unordered_set<std::string> selected;
    if (options.contains("--selected-ids")) {
        const auto selection = nlohmann::json::parse(std::ifstream(require(options, "--selected-ids")));
        if (!selection.is_object() || !selection.contains("selected") || !selection.contains("reviewed") ||
            !selection["selected"].is_array() || !selection["reviewed"].is_array() ||
            selection["selected"].empty()) throw std::invalid_argument("at least one record must be selected");
        const auto read_ids = [](const nlohmann::json& ids) {
            std::unordered_set<std::string> result;
            for (const auto& id : ids) {
                if (!id.is_string()) throw std::invalid_argument("invalid training record ID");
                const auto value = id.get<std::string>();
                if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != std::string::npos ||
                    !result.insert(value).second) throw std::invalid_argument("invalid training record ID");
            }
            return result;
        };
        selected = read_ids(selection["selected"]);
        const auto reviewed = read_ids(selection["reviewed"]);
        for (const auto& id : selected) if (!reviewed.contains(id))
            throw std::invalid_argument("selected record was not reviewed");
        db.exec("BEGIN IMMEDIATE");
        try {
            Statement pending(db.get(), "SELECT id FROM commits WHERE state='pending'");
            std::set<std::string> available;
            while (pending.next() == SQLITE_ROW) available.insert(pending.text(0));
            Statement exclude(db.get(), "UPDATE commits SET state='excluded' WHERE id=? AND state='pending'");
            for (const auto& id : reviewed) {
                if (selected.contains(id) || !available.contains(id)) continue;
                exclude.bind(1, id); (void)exclude.next();
                sqlite3_reset(exclude.get()); sqlite3_clear_bindings(exclude.get());
            }
            db.exec("COMMIT");
        } catch (...) { sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    }
    if (fs::exists(output)) throw std::runtime_error("output directory already exists");
    fs::create_directories(output);
    const auto version_file = output / "trainer-version.json";
    run_tool(trainer.c_str(), {"--version", "--json"}, version_file);
    const auto trainer_version = nlohmann::json::parse(std::ifstream(version_file));
    if (trainer_version.at("trainerApi").get<int>() != 2)
        throw std::runtime_error("trainer API is incompatible (expected 2)");
    const auto dataset_path = output / "training.jsonl";
    const auto decryption = cipher.decryption();
    const auto dataset = ime::unix_service::write_numeric_dataset(db.get(), tables, training_model_dir / "config.json",
        dataset_path, max_length, options.contains("--selected-ids") ? &selected : nullptr, &decryption);
    std::cout << "trainable=" << dataset.included_ids.size() << " skipped=" << dataset.skipped << std::endl;
    const auto adapter = output / "adapter";
    const auto f16 = output / "personalized-f16.gguf";
    const auto gguf = output / "personalized-Q4_K_M.gguf";
    // The numeric dataset is readable text, so it only lives while this run
    // needs it; a killed process leaves at most the partial file, which the
    // next run and the manager remove.
    try {
        run(trainer, {"validate", "--train-data", dataset_path.string(), "--vocab-size",
                       std::to_string(dataset.vocab_size), "--max-seq-length", std::to_string(max_length)});
        std::vector<std::string> args{
            "train", "--model-config", (training_model_dir / "config.json").string(), "--model", training_model_dir.string(),
            "--train-data", dataset_path.string(), "--output-dir", adapter.string(),
            "--target-modules", modules, "--pad-token-id", std::to_string(dataset.pad_token_id),
            "--max-seq-length", std::to_string(max_length), "--rank", std::to_string(rank),
            "--alpha", as_argument(alpha), "--dropout", as_argument(dropout),
            "--batch-size", std::to_string(batch), "--gradient-accumulation", std::to_string(accumulation),
            "--epochs", std::to_string(epochs), "--max-steps", std::to_string(max_steps),
            "--learning-rate", as_argument(learning_rate), "--weight-decay", as_argument(weight_decay),
            "--warmup-steps", std::to_string(warmup), "--max-grad-norm", as_argument(norm),
            "--save-every", std::to_string(save_every), "--seed", std::to_string(seed),
            "--device", device, "--dtype", dtype
        };
        if (!shuffle) args.push_back("--no-shuffle");
        {
            Statement previous(db.get(), "SELECT base_revision,adapter_path FROM lora_runs ORDER BY id DESC LIMIT 1");
            if (previous.next() == SQLITE_ROW) {
                if (revision != previous.text(0)) throw std::runtime_error("previous adapter uses a different base revision");
                const fs::path path = previous.text(1);
                if (!fs::is_regular_file(path / "adapter_model.safetensors"))
                    throw std::runtime_error("previous adapter is missing");
                args.insert(args.end(), {"--resume-adapter", path.string()});
            }
        }
        run(trainer, args);
        run(trainer, {"export-gguf", "--model-config", (training_model_dir / "config.json").string(),
                      "--model", training_model_dir.string(), "--vocab-file", (training_model_dir / "ime_vocab.json").string(),
                      "--adapter", adapter.string(), "--outfile", f16.string(), "--outtype", "f16",
                       "--quantize", "Q4_K_M", "--quantized-outfile", gguf.string(), "--force"});
        if (!fs::is_regular_file(gguf) || fs::file_size(gguf) == 0)
            throw std::runtime_error("trainer did not produce a GGUF model");
        fs::remove(f16);
        discard_plaintext_dataset(dataset_path);
    } catch (...) {
        discard_plaintext_dataset(dataset_path);
        throw;
    }
    publish_run(db, dataset, adapter, gguf, revision, rank, alpha, dropout, modules);
    std::cout << "model=" << gguf << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ::umask(0077);  // Datasets and adapter outputs contain user typing.
        if (argc < 2) throw std::invalid_argument("usage: llavon-ime-lora check-model|fetch-model|install-trainer|protection-status|configure-password|set-recording|reset-conversation-data|list|exclude|delete|dataset|train [--option value ...]");
        const auto options = parse(argc, argv);
        if (std::string_view(argv[1]) == "install-trainer") {
            install_trainer(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        if (std::string_view(argv[1]) == "check-model") {
            check_model(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        if (std::string_view(argv[1]) == "fetch-model") {
            fetch_model(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        const auto db_path = optional(options, "--db", "");
        const auto database = db_path.empty() ? ime::unix_service::CommitStore::default_path() : fs::path(db_path);
        const std::string_view action = argv[1];
        if (action == "protection-status") {
            ime::unix_service::CommitStore store(database);
            const auto status = store.protection_status();
            std::cout << nlohmann::json{{"configured", status.configured}, {"enabled", status.enabled}}.dump() << '\n';
            return EXIT_SUCCESS;
        }
        if (action == "configure-password") {
            ime::unix_service::CommitStore store(database);
            store.configure_password(password_option(options));
            std::cout << "configured=1 enabled=1\n";
            return EXIT_SUCCESS;
        }
        if (action == "set-recording") {
            const auto enabled = require(options, "--enabled");
            if (enabled != "0" && enabled != "1") throw std::invalid_argument("invalid --enabled");
            ime::unix_service::CommitStore store(database);
            store.set_recording_enabled(enabled == "1");
            std::cout << "enabled=" << enabled << '\n';
            return EXIT_SUCCESS;
        }
        if (action == "reset-conversation-data") {
            ime::unix_service::CommitStore store(database);
            store.reset_conversation_data();
            std::cout << "cleared=1\n";
            return EXIT_SUCCESS;
        }
        Database db(database);
        if (action == "list") list(db.get(), options);
        else if (action == "exclude" || action == "delete") change_state(db, action, require(options, "--id"));
        else if (action == "dataset" || action == "train") {
            const fs::path model_dir = fs::absolute(require(options, "--model-dir"));
            const fs::path tables = fs::absolute(require(options, "--tables-dir"));
            ime::unix_service::CommitCipher cipher;
            unlock(cipher, db.get(), options);
            if (action == "train") train(db, options, model_dir, tables, fs::absolute(require(options, "--output-dir")),
                                          database, cipher);
            else {
                const auto decryption = cipher.decryption();
                const auto dataset = ime::unix_service::write_numeric_dataset(
                    db.get(), tables, model_dir / "config.json", require(options, "--output"), 384, nullptr,
                    &decryption);
                std::cout << "trainable=" << dataset.included_ids.size() << " skipped=" << dataset.skipped << '\n';
            }
        } else throw std::invalid_argument("unknown action");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "llavon-ime-lora: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
