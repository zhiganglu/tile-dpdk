/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2014 Tilera Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Tilera Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
#ifndef _RTE_CPUFLAGS_TILE_H_
#define _RTE_CPUFLAGS_TILE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <rte_common.h>

#include "generic/rte_cpuflags.h"

enum rte_cpu_flag_t {
	/* The last item */
	RTE_CPUFLAG_NUMFLAGS,               /**< This should always be the last! */
};

static inline void
rte_cpu_get_features(__rte_unused uint32_t leaf,
		     __rte_unused uint32_t subleaf,
		     __rte_unused cpuid_registers_t out)
{
}

static inline int
rte_cpu_get_flag_enabled(__rte_unused enum rte_cpu_flag_t feature)
{
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _RTE_CPUFLAGS_TILE_H_ */
