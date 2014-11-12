/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2014 Tilera Corporation. All rights reserved.
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <pthread.h>
#include <errno.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <rte_tailq.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_spinlock.h>
#include <rte_eal.h>
#include <rte_debug.h>

#include <rte_mpipe.h>

#include "eal_private.h"
#include "eal_internal_cfg.h"

#define MPIPE_MAX_CHANNELS	128

struct rte_eal_mpipe_context
{
	rte_spinlock_t        lock;
	gxio_mpipe_context_t  context;
	struct rte_eal_mpipe_channel_config channels[MPIPE_MAX_CHANNELS];
};

struct rte_eal_mpipe_context rte_eal_mpipe_contexts[GXIO_MPIPE_INSTANCE_MAX];
int rte_eal_mpipe_instances;

int
rte_eal_mpipe_init(void)
{
	struct rte_eal_mpipe_context *context;
	int rc, instance;

	for (instance = 0; instance < GXIO_MPIPE_INSTANCE_MAX; instance++) {
		context = &rte_eal_mpipe_contexts[instance];

		rte_spinlock_init(&context->lock);
		rc = gxio_mpipe_init(&context->context,
				     instance);
		if (rc < 0)
			break;
	}

	rte_eal_mpipe_instances = instance;

	return instance ? 0 : -ENODEV;
}

gxio_mpipe_context_t *
rte_eal_mpipe_context(int instance)
{
	if (instance < 0 || instance >= rte_eal_mpipe_instances)
		return NULL;
	return &rte_eal_mpipe_contexts[instance].context;
}

int
rte_eal_mpipe_channel_config(int instance, int channel,
			     struct rte_eal_mpipe_channel_config *config)
{
	struct rte_eal_mpipe_channel_config *data;
	struct rte_eal_mpipe_context *context;
	gxio_mpipe_rules_t rules;
	int idx, rc = 0;

	if (instance < 0 || instance >= rte_eal_mpipe_instances ||
	    channel < 0 || channel >= MPIPE_MAX_CHANNELS)
		return -EINVAL;

	context = &rte_eal_mpipe_contexts[instance];

	rte_spinlock_lock(&context->lock);

	gxio_mpipe_rules_init(&rules, &context->context);

	for (idx = 0; idx < MPIPE_MAX_CHANNELS; idx++) {
		data = (channel == idx) ? config : &context->channels[idx];

		if (!data->enable)
			continue;

		rc = gxio_mpipe_rules_begin(&rules, data->first_bucket,
					    data->num_buckets, &data->stacks);
		if (rc < 0) {
			goto done;
		}

		rc = gxio_mpipe_rules_add_channel(&rules, idx);
		if (rc < 0) {
			goto done;
		}

		rc = gxio_mpipe_rules_set_headroom(&rules, data->headroom);
		if (rc < 0) {
			goto done;
		}
	}

	rc = gxio_mpipe_rules_commit(&rules);
	if (rc == 0) {
		memcpy(&context->channels[channel], config, sizeof(*config));
	}

done:
	rte_spinlock_unlock(&context->lock);

	return rc;
}
