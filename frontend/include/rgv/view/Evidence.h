// How trustworthy a rendered thing is.
//
// A node reports its own freshness; an impact result reports the freshness of the PATH
// that reached it, which can be worse. The weaker of the two wins -- a conclusion is
// only as fresh as its worst hop, and presenting stale evidence as current is what
// NFR-04 forbids outright.
//
// Shared rather than duplicated because two systems need it: one counts how many
// things are uncertain, the other draws them dashed. If they computed it separately
// they could disagree about the same node.
#pragma once

#include "rgv/contract/Types.h"

namespace rgv::view {

constexpr int freshness_rank(Freshness f) {
    switch (f) {
        case Freshness::Current: return 0;
        case Freshness::Pending: return 1;
        case Freshness::Stale:   return 2;
        case Freshness::Invalid: return 3;
    }
    return 0;
}

constexpr Freshness worse_of(Freshness a, Freshness b) {
    return freshness_rank(a) >= freshness_rank(b) ? a : b;
}

} // namespace rgv::view
