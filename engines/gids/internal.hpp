/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "adapter.h"
#include <tutti/cuda_like.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include "profile_types.h"

inline void gpu_check(cudaError_t e, const char *what) {
    if (e != cudaSuccess) throw std::runtime_error(std::string(what)+": "+cudaGetErrorString(e));
}
struct Request { uint64_t offset; uint32_t bytes, write; };
struct Result { uint32_t result, raw; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual void open(const gids_options &, void *, size_t, gids_geometry &) = 0;
    virtual void prepare(unsigned, const Request &) = 0;
    virtual void launch(unsigned, const Request *, Result *, cudaStream_t) = 0;
    virtual void profile(const Request *,Result *,void *,const void *,ApSample *,cudaStream_t) = 0;
    virtual void diagnose(unsigned) {}
};
std::unique_ptr<Backend> make_pcie();
ApCalibration calibrate_ap_clock(cudaStream_t);
cudaError_t launch_pcie_profile(void *,const void *,const Request *,Result *,
    void *,const void *,ApSample *,unsigned,cudaStream_t);
cudaError_t launch_pcie_batch(const void *, const void *, const Request *,
                             Result *, unsigned, unsigned, unsigned, cudaStream_t);
