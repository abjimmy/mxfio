/* SPDX-License-Identifier: GPL-2.0 */
#include "../wire.h"
#include <cassert>
#include <cstdio>
int main() {
    auto c=rw_capsule(0x1234,3,0x1020304050607080ull,8,false,
                      0x1122334455667788ull,4096,0xabcdef12);
    assert(c.b[0]==2 && c.b[1]==0x40 && c.b[2]==0x34 && c.b[3]==0x12);
    assert(get32(c.b+4)==3 && get64(c.b+24)==0x1122334455667788ull);
    assert(c.b[32]==0 && c.b[33]==0x10 && c.b[34]==0);
    assert(get32(c.b+35)==0xabcdef12 && c.b[39]==0x40);
    assert(get64(c.b+40)==0x1020304050607080ull && get16(c.b+48)==7);
    for(unsigned i=8;i<24;++i) assert(c.b[i]==0);
    assert(be32(0x12345678)==0x78563412 && be64(0x0102030405060708ull)==0x0807060504030201ull);
    c=rw_capsule(65535,1,0,65536,true,0x10000,0xffffff,1);
    assert(c.b[0]==1 && get16(c.b+48)==65535 && c.b[34]==0xff);
    puts("NVMe keyed-SGL / command and mlx5 byte-order tests passed");
}
