// Exercise the real port macro with a deterministic 100 MHz clock. This also
// checks that including the header in a host TU needs no MACA builtin or SDK.
#include <cstdio>

static unsigned long long ticks = 0x100000001ull;
static unsigned reads = 0;
static unsigned long long read_test_realtime() {
    ++reads;
    const auto value = ticks;
    ticks += 100;
    return value;
}

#define __builtin_mxc_get_realtime() read_test_realtime()
#include <tutti/gpu_vendor/maca_primitives.h>

int main() {
    const auto before = TUTTI_GLOBALTIMER_NS();
    const auto after = TUTTI_GLOBALTIMER_NS();
    if (before != 0xa0000000aull || after - before != 1000ull || reads != 2) {
        std::puts("MACA timestamps must be 64-bit nanoseconds, one clock read each");
        return 1;
    }
    // The upstream consumer divides by 1000 to report microseconds.
    if ((after - before) / 1000.0 != 1.0 ||
        before / TUTTI_MACA_REALTIME_NS_PER_TICK != 0x100000001ull) {
        std::puts("MACA timestamp units do not round-trip to realtime ticks");
        return 1;
    }
    std::puts("MACA nanosecond clock contract passed");
}
