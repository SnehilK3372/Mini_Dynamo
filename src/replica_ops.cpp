#include "replica_ops.h"

#include "vector_clock.h"

namespace replica_ops {

bool applyReplicate(StorageEngine &storage, const std::string &key,
                    const VersionedValue &incoming) {
    auto stored = storage.get(key);
    if (stored) {
        VersionedValue local = VersionedValue::deserialize(*stored);
        if (VectorClock::compare(local.clock, incoming.clock) ==
            VectorClock::Ordering::A_DOMINATES) {
            return false;  // ours strictly dominates — keep it, still ack
        }
    }
    storage.put(key, incoming.serialize());
    return true;
}

std::optional<VersionedValue> readLocal(StorageEngine &storage, const std::string &key) {
    auto stored = storage.get(key);
    if (!stored) return std::nullopt;
    return VersionedValue::deserialize(*stored);
}

}  // namespace replica_ops
