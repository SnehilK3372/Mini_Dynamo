#include "versioned_value.h"

#include "base64.h"

using namespace std;

string VersionedValue::serialize() const {
    // base64(data) contains only [A-Za-z0-9+/=]; clock token contains only
    // [A-Za-z0-9:,] — neither can contain '|', so one '|' cleanly separates them.
    return base64::encode(data) + "|" + clock.serialize();
}

VersionedValue VersionedValue::deserialize(const string &stored) {
    VersionedValue vv;
    auto bar = stored.find('|');
    if (bar == string::npos) {
        // Tolerate a bare (pre-versioning) value: treat the whole thing as
        // base64 data with an empty clock. Defensive; production always writes
        // the two-field form.
        vv.data = base64::decode(stored);
        return vv;
    }
    vv.data = base64::decode(stored.substr(0, bar));
    vv.clock = VectorClock::parse(stored.substr(bar + 1));
    return vv;
}
