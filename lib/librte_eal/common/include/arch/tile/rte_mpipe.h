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

#ifndef _RTE_TILE_MPIPE_H_
#define _RTE_TILE_MPIPE_H_

#define ARCH_ATOMIC_NO_NICKNAMES

#include <gxio/mpipe.h>
#include <arch/mpipe_xaui_def.h>
#include <arch/mpipe_gbe_def.h>

#define BSM_ALIGN_SIZE 128

struct rte_eal_mpipe_channel_config
{
	int enable;
	int first_bucket;
	int num_buckets;
	int headroom;
	gxio_mpipe_rules_stacks_t stacks;
};

#define __bsm_aligned __attribute__((__aligned__(BSM_ALIGN_SIZE)))

extern int rte_eal_mpipe_instances;

extern int
rte_eal_mpipe_init(void);

extern gxio_mpipe_context_t *
rte_eal_mpipe_context(int instance);

extern int
rte_eal_mpipe_channel_config(int instance, int channel,
			     struct rte_eal_mpipe_channel_config *config);

#endif /* _RTE_TILE_MPIPE_H_ */
