/* SPDX-License-Identifier: GPL-2.0 */
#include "../ap_plan.hpp"
#include <cassert>
#include <cstdio>
#include <unistd.h>

int main() {
    const std::string file="/tmp/gids-plan-test-"+std::to_string(getpid());
    const std::string valid="GIDS_AP_PLAN_V1 read 4096 16384 2 7\n0\n12288\n";
    auto parse=[&](const std::string &text) {
        { std::ofstream out(file); out<<text; }
        return ApPlan::read(file.c_str());
    };
    auto p=parse(valid);
    assert(!p.write && p.offsets.size()==2 && p.offsets[1]==12288);
    p.validate(16384,512,4096);
    auto w=parse("GIDS_AP_PLAN_V1 write 4096 16384 2 7\n4096\n4096\n");
    assert(w.write && w.offsets[0]==w.offsets[1]);
    for(const char *text : {
        "GIDS_AP_PLAN_V1 read 4096 16384 2 7\n0\n", // truncated
        "GIDS_AP_PLAN_V1 read 4096 16384 1 7\n16384\n", // beyond end
        "GIDS_AP_PLAN_V1 read 4096 16384 1 7\n512\n", // wrong alignment
        "GIDS_AP_PLAN_V1 read 4096 16384 1 7\n-1\n", // signed input
        "GIDS_AP_PLAN_V1 read 4096 16384 1 7\n0\n4096\n", // surplus
        "GIDS_AP_PLAN_V1 read 131072 3840755982336 65536 7\n", // huge snapshot
        "GIDS_AP_PLAN_V1 trim 4096 16384 1 7\n0\n",
        "GIDS_AP_PLAN_V1 read 0 16384 1 7\n0\n"}) {
        bool rejected=false;
        try { parse(text); } catch(const std::exception &) { rejected=true; }
        assert(rejected);
    }
    for(unsigned mode=0;mode<3;++mode) {
        bool rejected=false;
        try { p.validate(mode==0 ? 8192 : 16384,mode==1 ? 8192 : 512,mode==2 ? 512 : 4096); }
        catch(const std::exception &) { rejected=true; }
        assert(rejected);
    }
    std::remove(file.c_str());
}
