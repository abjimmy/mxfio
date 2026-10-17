/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Only the accelerator attributes/clock are substituted. Submission,
 * command construction and SQ/CQ state machines use the production code. */
#define TUTTI_DEVICE __device__
#define TUTTI_HOST __host__
#define TUTTI_FORCEINLINE __forceinline__
#define TUTTI_THREAD_IDX_X threadIdx.x
#define TUTTI_BLOCK_IDX_X blockIdx.x
#define TUTTI_BLOCK_DIM_X blockDim.x
#define TUTTI_GLOBALTIMER_NS() (static_cast<unsigned long long>(queue_mock::sleeps))
