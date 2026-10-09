/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// A recorded plan keeps fio replay and the GPU reference snapshot in lockstep.
struct ApPlan {
    bool write=false;
    uint64_t bytes=0,capacity=0,seed=0;
    std::vector<uint64_t> offsets;

    static ApPlan read(const char *path) {
        std::ifstream in(path);
        std::string magic,operation;
        if (!(in>>magic>>operation) || magic!="GIDS_AP_PLAN_V1" ||
            (operation!="read" && operation!="write"))
            throw std::runtime_error("invalid AP plan header");
        auto number=[&]() {
            std::string token;
            if (!(in>>token) || token.find_first_not_of("0123456789")!=std::string::npos)
                throw std::runtime_error("invalid AP plan number");
            return std::stoull(token);
        };
        ApPlan p;
        p.write=operation=="write";
        p.bytes=number(); p.capacity=number();
        uint64_t count=number(); p.seed=number();
        if (!p.bytes || p.bytes%8 || p.bytes>16*1024*1024 || p.capacity<p.bytes ||
            !count || count>65536 || (!p.write && count>(2ull<<30)/p.bytes))
            throw std::runtime_error("AP plan exceeds block/count/reference memory limits");
        for (uint64_t i=0;i<count;++i) {
            uint64_t offset=number();
            if (offset%p.bytes || offset>p.capacity-p.bytes)
                throw std::runtime_error("AP plan offset outside namespace or unaligned");
            p.offsets.push_back(offset);
        }
        std::string extra;
        if (in>>extra) throw std::runtime_error("extra AP plan entries");
        return p;
    }
    void validate(uint64_t actual_capacity,unsigned lba,unsigned max_bs) const {
        if (capacity!=actual_capacity || !lba || bytes%lba || bytes>max_bs)
            throw std::runtime_error("AP plan does not match identified namespace capacity/LBA or bs");
    }
};
