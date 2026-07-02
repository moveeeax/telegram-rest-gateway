#include "util/metrics.hpp"

namespace tgw::metrics {

Counters& Counters::instance() {
    static Counters counters;
    return counters;
}

}  // namespace tgw::metrics
