#include "versioned_value.h"

#include "base64.h"

using namespace std;

string VersionedValue::serialize() const {
    // base64(data) contains only [A-Za-z0-9+/=]; clock token contains only
    // [A-Za-z0-9:,] — neither can contain '|', so the '|' splits stay clean.
    // A tombstone adds a third field "D"; live values omit it (staying
    // byte-identical to the pre-tombstone format).
    string s = base64::encode(data) + "|" + clock.serialize();
    if (deleted) s += "|D";
    return s;
}

VersionedValue VersionedValue::deserialize(const string &stored) {
    VersionedValue vv;
    auto bar = stored.find('|');
    if (bar == string::npos) {
        // Tolerate a bare (pre-versioning) value: treat the whole thing as
        // base64 data with an empty clock. Defensive; production always writes
        // the framed form.
        vv.data = base64::decode(stored);
        return vv;
    }
    vv.data = base64::decode(stored.substr(0, bar));

    // Everything after the first '|' is the clock, optionally followed by a
    // "|D" tombstone marker. Splitting on the *next* '|' keeps the clock token
    // intact (it never contains '|') and leaves the flag isolated.
    const string rest = stored.substr(bar + 1);
    auto bar2 = rest.find('|');
    if (bar2 == string::npos) {
        vv.clock = VectorClock::parse(rest);
    } else {
        vv.clock = VectorClock::parse(rest.substr(0, bar2));
        vv.deleted = (rest.substr(bar2 + 1) == "D");
    }
    return vv;
}
