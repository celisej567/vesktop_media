#include "transport/region_tester.h"

#include <algorithm>
#include <map>
#include <thread>

#include "common/log.h"
#include "common/util.h"
#include "transport/udp_socket.h"

namespace vv {

void RankRtcRegions(std::vector<RegionEndpoints> regions, std::function<void(std::vector<std::string>)> done)
{
    std::thread([regions = std::move(regions), done = std::move(done)] {
        struct Probe {
            size_t region;
            SocketAddress addr;
            int64_t sentMs = 0;
            int64_t rttMs = -1;
        };
        std::vector<Probe> probes;
        for (size_t r = 0; r < regions.size(); r++)
            for (auto& ip : regions[r].ips) {
                auto colon = ip.rfind(':');
                if (colon == std::string::npos) continue;
                auto addr = SocketAddress::Resolve(ip.substr(0, colon), std::atoi(ip.c_str() + colon + 1));
                if (addr) probes.push_back({r, *addr});
            }

        UdpSocket v4, v6;
        v4.Open(AF_INET);
        v6.Open(AF_INET6);
        for (uint32_t seq = 0; seq < probes.size(); seq++) {
            uint8_t pkt[8] = {0x13, 0x37, 0xca, 0xfe};
            WriteU32LE(pkt + 4, seq);
            auto& sock = probes[seq].addr.storage.ss_family == AF_INET6 ? v6 : v4;
            probes[seq].sentMs = NowMs();
            sock.SendTo(pkt, sizeof(pkt), probes[seq].addr);
        }

        int64_t deadline = NowMs() + 5000;
        size_t answered = 0;
        while (answered < probes.size() && NowMs() < deadline) {
            if (!WaitReadable({&v4, &v6}, static_cast<int>(std::max<int64_t>(1, deadline - NowMs())))) continue;
            for (auto* sock : {&v4, &v6}) {
                uint8_t buf[64];
                SocketAddress from;
                int64_t n;
                while ((n = sock->RecvFrom(buf, sizeof(buf), from)) >= 8) {
                    // the answer to 13 37 ca fe is 13 37 f0 0d (an echo from test servers)
                    if (ReadU32(buf) != 0x1337f00d && ReadU32(buf) != 0x1337cafe) continue;
                    uint32_t seq = ReadU32LE(buf + 4);
                    if (seq < probes.size() && probes[seq].rttMs < 0) {
                        probes[seq].rttMs = NowMs() - probes[seq].sentMs;
                        answered++;
                    }
                }
            }
        }

        std::vector<std::string> ranked;
        if (answered == probes.size() && !probes.empty()) {
            std::map<size_t, std::pair<int64_t, int>> sums;
            for (auto& p : probes) sums[p.region].first += p.rttMs, sums[p.region].second++;
            std::vector<std::pair<double, size_t>> order;
            for (auto& [r, s] : sums) order.push_back({static_cast<double>(s.first) / s.second, r});
            std::sort(order.begin(), order.end());
            for (auto& [avg, r] : order) ranked.push_back(regions[r].region);
        }
        VV_LOGI("region", "rankRtcRegions: %zu/%zu answered, %zu ranked", answered, probes.size(), ranked.size());
        done(std::move(ranked));
    }).detach();
}

} // namespace vv
