#pragma once

#include <string>

// Structured JSON logging for the node. Every line is a single JSON object on
// stdout (Docker captures it) with a fixed envelope — ts, service, level,
// node_id — plus either an operation triple (operation/key/outcome) for request
// events or a free-form msg for lifecycle events. Backed by spdlog when it's
// available at build time (HAVE_SPDLOG), and by a small mutex-guarded stdout
// writer otherwise, so the node still builds in the minimal test image. Both
// backends emit byte-identical output.
namespace jlog {

// Set the constant node_id carried on every line. Call once at startup before
// any other jlog call.
void init(const std::string &node_id);

// A request/operation event: e.g. op("info", "put", "k1", "ok"). `key` may be
// empty where it isn't meaningful.
void op(const std::string &level, const std::string &operation, const std::string &key,
        const std::string &outcome);

// A lifecycle/free-form event: e.g. msg("info", "TCP server listening on 0.0.0.0:5001").
void msg(const std::string &level, const std::string &message);

}  // namespace jlog
