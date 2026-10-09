// Compile public headers with the host compiler and resolve runtime aliases.
// main deliberately makes no SDK call and requires no selected GPU.
#include <tutti/storage_runtime.h>
#include <tutti/gpu_vendor/maca_primitives.h>
#include <type_traits>
static_assert(cudaErrorInvalidValue == mcErrorInvalidValue);
static_assert(cudaErrorNotSupported == mcErrorNotSupported);
static_assert(std::is_same<decltype(&cudaPeekAtLastError),
                           decltype(&mcPeekAtLastError)>::value);
int main() { return 0; }
