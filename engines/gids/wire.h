/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __CUDACC__
#define GIDS_HD __host__ __device__
#else
#define GIDS_HD
#endif
/* NVMe little-endian capsules and mlx5 big-endian WQE words. Byte access
 * avoids compiler packing, unaligned loads and host endian assumptions. */
struct Capsule { uint8_t b[64]; };
GIDS_HD inline void put16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
GIDS_HD inline void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=v>>(i*8); }
GIDS_HD inline void put64(uint8_t *p,uint64_t v) { for(unsigned i=0;i<8;++i) p[i]=v>>(i*8); }
GIDS_HD inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0])|(uint16_t(p[1])<<8); }
GIDS_HD inline uint32_t get32(const uint8_t *p) { return uint32_t(get16(p))|(uint32_t(get16(p+2))<<16); }
GIDS_HD inline uint64_t get64(const uint8_t *p) { return uint64_t(get32(p))|(uint64_t(get32(p+4))<<32); }
GIDS_HD inline uint32_t be32(uint32_t v) { return (v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|(v<<24); }
GIDS_HD inline uint64_t be64(uint64_t v) { return (uint64_t(be32(uint32_t(v)))<<32)|be32(uint32_t(v>>32)); }
GIDS_HD inline void keyed_sgl(Capsule &c,uint64_t addr,uint32_t length,uint32_t key) {
    c.b[1]=0x40; /* PSDT: SGL */
    put64(c.b+24,addr);
    c.b[32]=length; c.b[33]=length>>8; c.b[34]=length>>16;
    put32(c.b+35,key); c.b[39]=0x40; /* keyed data block, address subtype; no invalidation */
}
GIDS_HD inline Capsule rw_capsule(uint16_t cid,uint32_t nsid,uint64_t lba,uint32_t blocks,bool write,uint64_t addr,uint32_t length,uint32_t key) {
    Capsule c{}; c.b[0]=write?1:2; put16(c.b+2,cid); put32(c.b+4,nsid);
    keyed_sgl(c,addr,length,key); put64(c.b+40,lba); put16(c.b+48,blocks-1);
    return c;
}
static_assert(sizeof(Capsule)==64,"NVMe command capsule ABI");
