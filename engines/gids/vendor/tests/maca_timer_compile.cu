#include <tutti/cuda_like.h>
#include <tutti/gpu_vendor/maca_primitives.h>
// Compile the real device builtin; the host contract test checks its units.
__global__ void vendor_timer_compile(unsigned long long* result) {
    *result = TUTTI_GLOBALTIMER_NS();
}
