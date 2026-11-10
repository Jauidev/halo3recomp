// maliprof: samples Arm Mali GPU hardware counters (libGPUCounters / HWCPipe) from adb shell while the
// game runs. Counters are global to the GPU, so the sampler does not have to live in the game process.
// Usage: maliprof [seconds=10] [interval_ms=1000]
// Prints one line per counter per interval (raw counters are deltas over the interval) and a summary
// with the averages at the end.
#include <hwcpipe/counter_database.hpp>
#include <hwcpipe/gpu.hpp>
#include <hwcpipe/sampler.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

static const hwcpipe_counter kCounters[] = {
    MaliGPUActiveCy,      MaliGPUIRQActiveCy,    MaliFragQueueActiveCy, MaliVertQueueActiveCy,
    MaliCompQueueActiveCy, MaliTilerActiveCy,    MaliCoreActiveCy,      MaliFragActiveCy,
    MaliNonFragActiveCy,  MaliFragQueueJob,      MaliVertQueueJob,      MaliCompQueueJob,
    MaliFragQueueTask,    MaliGPUPix,            MaliFragTile,          MaliFragTileKill,
    MaliFragOverdraw,     MaliGeomTotalPrim,     MaliGeomVisiblePrim,   MaliExtBusRdBy,
    MaliExtBusWrBy,       MaliExtBusRdStallCy,   MaliALUUtil,           MaliTexUtil,
    MaliLSUtil,           MaliVarUtil,           MaliCoreUtil,          MaliFragUtil,
    MaliNonFragUtil,      MaliTilerUtil,         MaliTexSample,         MaliGPUCyPerPix,
    MaliFragShadRate,     MaliEngFMAPipeUtil,    MaliL2CacheFlush,      MaliFragRastPrim,
    MaliFragEZSKillRate,  MaliFragLZSTestRate,   MaliSCBusTileWrBy,     MaliTilerWrBy,
    MaliCSFMCUActiveCy,   MaliFragQueuedCy,
};

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int interval_ms = argc > 2 ? atoi(argv[2]) : 1000;

    auto gpu = hwcpipe::gpu(0);
    if (!gpu) {
        printf("no Mali GPU at /dev/mali0\n");
        return 1;
    }
    printf("cores %u, bus width %u\n", (unsigned)gpu.num_shader_cores(), (unsigned)gpu.bus_width());

    hwcpipe::counter_database db;
    auto config = hwcpipe::sampler_config(gpu);
    std::vector<hwcpipe_counter> used;
    std::vector<std::string> names;
    for (hwcpipe_counter c : kCounters) {
        hwcpipe::counter_metadata meta;
        if (db.describe_counter(c, meta)) continue;
        if (config.add_counter(c)) {
            printf("unsupported: %s\n", meta.name);
            continue;
        }
        used.push_back(c);
        names.push_back(meta.name);
    }

    auto sampler = hwcpipe::sampler<>(config);
    if (auto ec = sampler.start_sampling()) {
        printf("start_sampling: %s\n", ec.message().c_str());
        return 1;
    }
    std::vector<double> sum(used.size(), 0.0);
    int n = 0;
    for (int t = 0; t * interval_ms < seconds * 1000; ++t) {
        usleep(interval_ms * 1000);
        if (auto ec = sampler.sample_now()) {
            printf("sample_now: %s\n", ec.message().c_str());
            continue;
        }
        printf("--- sample %d\n", t);
        for (size_t i = 0; i < used.size(); ++i) {
            hwcpipe::counter_sample s;
            if (sampler.get_counter_value(used[i], s)) continue;
            double v = s.type == hwcpipe::counter_sample::type::uint64 ? double(s.value.uint64) : s.value.float64;
            sum[i] += v;
            printf("%-24s %.4g\n", names[i].c_str(), v);
        }
        ++n;
    }
    sampler.stop_sampling();
    printf("=== average over %d samples of %d ms\n", n, interval_ms);
    for (size_t i = 0; i < used.size() && n; ++i) printf("%-24s %.4g\n", names[i].c_str(), sum[i] / n);
    return 0;
}
