#pragma once

#include <optional>
#include <string>

#include "storage/storage_engine.h"
#include "versioned_value.h"

// What a replica does with a coordinator's REPLICATE / READ, independent of the
// wire. Extracted from Node's handlers so the *same* code serves both the real
// protocol edge (src/node.cpp) and the in-process test cluster
// (tests/support/in_process_cluster.h).
//
// The extraction is deliberate: node.cpp is POSIX-only (framing/sockets) and so
// cannot be linked by the portable test target. A test harness that
// re-implemented these rules would be asserting against a reimplementation
// rather than the shipped behaviour — exactly the failure mode that let the SWIM
// rejoin bug through a green suite. One code path, no drift.
namespace replica_ops {

// Apply a replicated version to local storage, never regressing: if the local
// copy strictly dominates the incoming one, this is a stale/duplicate replicate
// and the local copy is kept. Otherwise the incoming version is stored.
//
// Concurrent versions across *different* replicas are surfaced as siblings at
// read time; a single replica keeps one value (per-replica sibling storage is
// deferred with anti-entropy).
//
// Returns true if local storage was written, false if the local copy was kept.
// Either way the caller should acknowledge — our copy is at least as new.
bool applyReplicate(StorageEngine &storage, const std::string &key, const VersionedValue &incoming);

// Read this replica's version of `key`, or nullopt if absent.
std::optional<VersionedValue> readLocal(StorageEngine &storage, const std::string &key);

}  // namespace replica_ops
