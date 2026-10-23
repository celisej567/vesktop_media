// rankRtcRegions: UDP latency test with the keepalive ping packet
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/js.h"

namespace vv {

struct RegionEndpoints {
    std::string region;
    std::vector<std::string> ips; // "host:port"
};

// Runs on its own thread; `done` gets the region names sorted by average RTT, or [] when any ping
// timed out
void RankRtcRegions(std::vector<RegionEndpoints> regions, std::function<void(std::vector<std::string>)> done);

} // namespace vv
