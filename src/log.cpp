#include "log.h"

#include <cstdio>
#include <string>

#ifdef HAVE_SPDLOG
#include <memory>

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#else
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#endif

namespace {

// Minimal JSON string escaping. Keys are arbitrary client input (the protocol
// only forbids '|', not quotes/backslashes/control bytes), so anything that
// would break the JSON line must be escaped.
std::string jesc(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string g_node_id = "unknown";

// Builds the inner field set for an operation event (no surrounding braces — the
// envelope supplies them).
std::string opFields(const std::string &operation, const std::string &key,
                     const std::string &outcome) {
    return "\"operation\":\"" + jesc(operation) + "\",\"key\":\"" + jesc(key) +
           "\",\"outcome\":\"" + jesc(outcome) + "\"";
}
std::string msgFields(const std::string &message) {
    return "\"msg\":\"" + jesc(message) + "\"";
}

#ifdef HAVE_SPDLOG
std::shared_ptr<spdlog::logger> g_logger;

// spdlog owns ts/level/node_id via the pattern; the message (%v) carries the
// per-event fields. Note: the message is always passed as an *argument*
// ("{}", fields), never as the format string, so a key containing '{' or '}'
// can never be misread as a fmt placeholder.
void emit(const std::string &level, const std::string &fields) {
    if (!g_logger) return;
    if (level == "error") {
        g_logger->error("{}", fields);
    } else if (level == "warn") {
        g_logger->warn("{}", fields);
    } else {
        g_logger->info("{}", fields);
    }
}
#else
std::mutex g_mtx;

std::string nowIso() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    auto ms = duration_cast<milliseconds>(now - secs).count();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lldZ", buf, static_cast<long long>(ms));
    return out;
}

void emit(const std::string &level, const std::string &fields) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::cout << "{\"ts\":\"" << nowIso() << "\",\"service\":\"node\",\"level\":\"" << level
              << "\",\"node_id\":\"" << jesc(g_node_id) << "\"," << fields << "}\n";
}
#endif

}  // namespace

namespace jlog {

void init(const std::string &node_id) {
    g_node_id = node_id;
#ifdef HAVE_SPDLOG
    g_logger = spdlog::stdout_logger_mt("node");
    g_logger->set_pattern("{\"ts\":\"%Y-%m-%dT%H:%M:%S.%eZ\",\"service\":\"node\",\"level\":\"%l"
                          "\",\"node_id\":\"" +
                          jesc(node_id) + "\",%v}");
    g_logger->flush_on(spdlog::level::info);
#endif
}

void op(const std::string &level, const std::string &operation, const std::string &key,
        const std::string &outcome) {
    emit(level, opFields(operation, key, outcome));
}

void msg(const std::string &level, const std::string &message) { emit(level, msgFields(message)); }

}  // namespace jlog
