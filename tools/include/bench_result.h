// ============================================================================
// bench_result.h - unified structured result contract for all bench_* tools.
// ----------------------------------------------------------------------------
// Every benchmark tool writes one result file per run. The file is the only
// machine-readable artifact consumed by the baseline store and the comparison
// harness; the human-readable stdout tables stay exactly as they were.
//
// Zero third-party dependencies: serialization goes through the in-tree JSON
// implementation. This header is tool-side only (tools/include), it is NOT part
// of the public aurora API and does not enter the umbrella header.
//
// Result lifecycle:
//   make_meta(tool, scene) -> BenchResult
//     .metric(...) / .flag(...) / .scope(...) / .trust(false, reason)
//     OR .skip("backend-unavailable") when no sample could be taken at all
//   finalize(result, tool, scene) -> true only when the file was written.
//
// A skipped run still writes a file, with data_status = "data-missing". A
// missing file and a skipped file are different things: the comparison harness
// must treat both as a failure, never as a pass.
// ============================================================================
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/core/json.h"

namespace aurora::bench {

// Bump only when a field is removed or renamed; additive fields keep the version.
inline constexpr int AURORA_BENCH_RESULT_SCHEMA = 1;

/// @brief Everything that identifies where one sample came from.
struct BenchMeta {
    std::string tool;  ///< producing executable name
    std::string scene;  ///< deterministic scenario name
    std::string testbed;  ///< machine/build bucket; cross-bucket comparison is forbidden
    std::string backend;  ///< compiled window backend
    std::string build_type;  ///< release / debug
    std::string compiler;  ///< compiler family
    std::string commit;  ///< source revision
    std::string build_fingerprint;  ///< binary + configuration fingerprint
    int warmup_frames = 0;  ///< frames run before sampling
    int discarded_frames = 0;  ///< frames explicitly dropped from the sample
    int sampled_frames = 0;  ///< frames actually counted
    int repetition = 0;  ///< repetition index inside one interleaved campaign
    std::uint64_t interleave_seed = 0;  ///< explicit seed that decided the A/B order
    std::string sampling = "steady-clock";  ///< clock domain and aggregation rule
};

/// @brief Outcome of one run. PascalCase values are the repository convention.
enum class ResultStatus : std::uint8_t {
    Ok,  ///< all required readings present and trustworthy
    Untrusted,  ///< readings exist but a self-check failed
    Skipped,  ///< no readings at all (backend absent, scene not compiled)
};

[[nodiscard]] inline auto env_or(const char *name, const char *fallback) -> std::string {
    const char *value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string{value} : std::string{fallback};
}

[[nodiscard]] inline auto env_uint(const char *name, std::uint64_t fallback) -> std::uint64_t {
    const std::string text = env_or(name, "");
    if (text.empty()) {
        return fallback;
    }
    std::uint64_t parsed = fallback;
    const char *const begin = text.data();
    const char *const end = std::next(begin, static_cast<std::ptrdiff_t>(text.size()));
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
    return (ec == std::errc{} && ptr == end) ? parsed : fallback;
}

[[nodiscard]] inline auto compiler_id() -> std::string {
#ifdef __clang__
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#elif defined(_MSC_VER)
    return "msvc";
#else
    return "unknown";
#endif
}

[[nodiscard]] inline auto backend_id() -> std::string {
#ifdef AURORA_BACKEND_WIN32
    return "win32";
#elif defined(AURORA_BACKEND_X11)
    return "x11";
#elif defined(AURORA_BACKEND_WAYLAND)
    return "wayland";
#elif defined(AURORA_BACKEND_HEADLESS)
    return "headless";
#else
    return "unknown";
#endif
}

[[nodiscard]] inline auto build_type_id() -> std::string {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

/// @brief Fill the metadata from build-time macros plus explicit CI variables.
/// @param tool producing executable name.
/// @param scene deterministic scenario name.
/// @return metadata with every field set; "unknown" is an explicit value, never an omission.
[[nodiscard]] inline auto make_meta(const std::string &tool, const std::string &scene) -> BenchMeta {
    BenchMeta meta;
    meta.tool = tool;
    meta.scene = scene;
    meta.testbed = env_or("AURORA_BENCH_TESTBED", "unknown");
    meta.backend = backend_id();
    meta.build_type = build_type_id();
    meta.compiler = compiler_id();
    meta.commit = env_or("AURORA_BENCH_COMMIT", "unknown");
    meta.build_fingerprint = env_or("AURORA_BENCH_BUILD_FINGERPRINT", "unknown");
    meta.repetition = static_cast<int>(env_uint("AURORA_BENCH_REPETITION", 0));
    meta.interleave_seed = env_uint("AURORA_BENCH_INTERLEAVE_SEED", 0);
    return meta;
}

/// @brief One structured result file.
/// @note Thread: tool-side, single-threaded. Not part of the library surface.
class BenchResult {
  public:
    /// @brief Construct from fully identified metadata.
    /// @param meta run metadata.
    explicit BenchResult(BenchMeta meta) : meta_(std::move(meta)) {}

    /// @brief Set the tool/scene identity on a process-wide result.
    /// @param tool producing executable name.
    /// @param scene deterministic scenario name.
    auto identify(const std::string &tool, const std::string &scene) -> void {
        meta_.tool = tool;
        meta_.scene = scene;
    }

    /// @brief Set the frame accounting shared by every metric in this run.
    /// @param warmup frames run before sampling.
    /// @param discarded frames explicitly dropped from the sample.
    /// @param sampled frames actually counted.
    auto frame_counts(int warmup, int discarded, int sampled) -> void {
        meta_.warmup_frames = warmup;
        meta_.discarded_frames = discarded;
        meta_.sampled_frames = sampled;
    }

    /// @brief Add a numeric reading. Duplicate names overwrite, by design.
    /// @param name stable metric key (use the same key across runs).
    /// @param value measurement.
    /// @param unit unit of the measurement.
    auto metric(const std::string &name, double value, const std::string &unit = "ms") -> void {
        metrics_.push_back(Metric{.name = name, .value = value, .unit = unit});
    }

    /// @brief Add a boolean self-check result.
    /// @param name stable flag key.
    /// @param value flag value.
    auto flag(const std::string &name, bool value) -> void { flags_.push_back(Flag{.name = name, .value = value}); }

    /// @brief Add an aggregated named scope.
    /// @param name logical region name; keep it stable across versions.
    /// @param total_ms accumulated milliseconds.
    /// @param calls number of scope entries.
    auto scope(const std::string &name, double total_ms, std::uint64_t calls) -> void {
        scopes_.push_back(Scope{.name = name, .total_ms = total_ms, .calls = calls});
    }

    /// @brief Record a failed self-check; the run stays readable but cannot pass.
    /// @param trustworthy result of the tool's own self-validation.
    /// @param reason why the reading may not be used as a baseline.
    auto trust(bool trustworthy, const std::string &reason) -> void {
        trustworthy_ = trustworthy;
        trust_reason_ = reason;
        if (!trustworthy) {
            status_ = ResultStatus::Untrusted;
        }
    }

    /// @brief Mark the run as having no readings at all.
    /// @param reason stable code such as "backend-unavailable".
    auto skip(const std::string &reason) -> void {
        status_ = ResultStatus::Skipped;
        reason_ = reason;
        trustworthy_ = false;
        trust_reason_ = reason;
    }

    /// @brief Materialize the contract as a JSON value.
    /// @return full result document; callers may serialize or inspect it directly.
    [[nodiscard]] auto to_value() const -> aurora::json::Value {
        auto root = aurora::json::Value::object();
        root.set("schema_version", aurora::json::Value{AURORA_BENCH_RESULT_SCHEMA});
        root.set("status", aurora::json::Value{status_text()});
        root.set("data_status", aurora::json::Value{data_status_text()});
        root.set("reason", aurora::json::Value{reason_});
        root.set("meta", meta_value());

        auto metrics = aurora::json::Value::object();
        for (const auto &entry : metrics_) {
            auto item = aurora::json::Value::object();
            item.set("value", aurora::json::Value{entry.value});
            item.set("unit", aurora::json::Value{entry.unit});
            metrics.set(entry.name, std::move(item));
        }
        root.set("metrics", std::move(metrics));

        auto flags = aurora::json::Value::object();
        for (const auto &entry : flags_) {
            flags.set(entry.name, aurora::json::Value{entry.value});
        }
        root.set("flags", std::move(flags));

        auto scopes = aurora::json::Value::array();
        for (const auto &entry : scopes_) {
            auto item = aurora::json::Value::object();
            item.set("name", aurora::json::Value{entry.name});
            item.set("total_ms", aurora::json::Value{entry.total_ms});
            item.set("calls", aurora::json::Value{static_cast<std::int64_t>(entry.calls)});
            scopes.push_back(std::move(item));
        }
        root.set("scopes", std::move(scopes));

        auto trust = aurora::json::Value::object();
        trust.set("trustworthy", aurora::json::Value{trustworthy_});
        trust.set("reason", aurora::json::Value{trust_reason_});
        root.set("trust", std::move(trust));
        return root;
    }

    /// @brief Serialize the contract with stable indentation.
    /// @return empty string when a reading is not serializable (NaN / Inf).
    [[nodiscard]] auto to_json() const -> std::string {
        const auto dumped = aurora::json::dump(to_value(), aurora::json::DumpOptions{.indent = 2});
        return dumped ? dumped.value() : std::string{};
    }

    /// @brief Write the result file, creating the parent directory when needed.
    /// @param path destination file.
    /// @return true only when the complete document reached disk.
    [[nodiscard]] auto write(const std::filesystem::path &path) const -> bool {
        const std::string text = to_json();
        if (text.empty()) {
            return false;
        }
        std::error_code ec;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return false;
            }
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text;
        out.close();
        return out.good();
    }

  private:
    struct Metric {
        std::string name;
        double value = 0.0;
        std::string unit;
    };
    struct Flag {
        std::string name;
        bool value = false;
    };
    struct Scope {
        std::string name;
        double total_ms = 0.0;
        std::uint64_t calls = 0;
    };

    [[nodiscard]] auto status_text() const -> std::string {
        switch (status_) {
            case ResultStatus::Ok:
                return "ok";
            case ResultStatus::Untrusted:
                return "untrusted";
            case ResultStatus::Skipped:
                return "skipped";
        }
        return "unknown";
    }

    [[nodiscard]] auto data_status_text() const -> std::string {
        return status_ == ResultStatus::Ok ? "complete" : "data-missing";
    }

    [[nodiscard]] auto meta_value() const -> aurora::json::Value {
        auto meta = aurora::json::Value::object();
        meta.set("tool", aurora::json::Value{meta_.tool});
        meta.set("scene", aurora::json::Value{meta_.scene});
        meta.set("testbed", aurora::json::Value{meta_.testbed});
        meta.set("backend", aurora::json::Value{meta_.backend});
        meta.set("build_type", aurora::json::Value{meta_.build_type});
        meta.set("compiler", aurora::json::Value{meta_.compiler});
        meta.set("commit", aurora::json::Value{meta_.commit});
        meta.set("build_fingerprint", aurora::json::Value{meta_.build_fingerprint});
        meta.set("warmup_frames", aurora::json::Value{meta_.warmup_frames});
        meta.set("discarded_frames", aurora::json::Value{meta_.discarded_frames});
        meta.set("sampled_frames", aurora::json::Value{meta_.sampled_frames});
        meta.set("repetition", aurora::json::Value{meta_.repetition});
        meta.set("interleave_seed", aurora::json::Value{static_cast<std::int64_t>(meta_.interleave_seed)});
        meta.set("sampling", aurora::json::Value{meta_.sampling});
        return meta;
    }

    BenchMeta meta_;
    std::vector<Metric> metrics_;
    std::vector<Flag> flags_;
    std::vector<Scope> scopes_;
    ResultStatus status_ = ResultStatus::Ok;
    std::string reason_;
    std::string trust_reason_;
    bool trustworthy_ = true;
};

/// @brief Default destination: AURORA_BENCH_RESULT_DIR, else ./bench-results/.
/// @param tool producing executable name.
/// @param scene deterministic scenario name (may be empty).
/// @return path of the single result file for this run.
[[nodiscard]] inline auto result_path(const std::string &tool, const std::string &scene) -> std::filesystem::path {
    const std::string dir = env_or("AURORA_BENCH_RESULT_DIR", "bench-results");
    std::string name = tool;
    if (!scene.empty() && scene != "default") {
        name += "." + scene;
    }
    name += ".json";
    return std::filesystem::path{dir} / name;
}

/// @brief Persist one result. Call this exactly once, at the end of every tool.
/// @param result fully populated result, including skip() when there is no sample.
/// @param tool producing executable name.
/// @param scene deterministic scenario name (may be empty).
/// @return true only when the file was written.
[[nodiscard]] inline auto finalize(const BenchResult &result, const std::string &tool, const std::string &scene)
    -> bool {
    return result.write(result_path(tool, scene));
}

/// @brief Process-wide result. Tool helpers (bench_row / report_*) feed it directly,
/// so a tool only needs identify() once and finalize_process() at the very end.
/// @return the single result object owned by this process.
[[nodiscard]] inline auto process_result() -> BenchResult & {
    static std::optional<BenchResult> instance;  // NOLINT(bugprone-dynamic-static-initializers)
    if (!instance.has_value()) {
        instance.emplace(make_meta("unknown-tool", "default"));
    }
    return *instance;
}

/// @brief Persist the process-wide result.
/// @param tool producing executable name.
/// @param scene deterministic scenario name.
/// @return true only when the file was written.
[[nodiscard]] inline auto finalize_process(const std::string &tool, const std::string &scene) -> bool {
    return finalize(process_result(), tool, scene);
}

}  // namespace aurora::bench
