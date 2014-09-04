/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
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
 *     * Neither the name of Intel Corporation nor the names of its
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

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_eal_memconfig.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_errno.h>
#include <rte_string_fns.h>
#include <rte_spinlock.h>

#include "rte_mempool.h"

/*
 * The mPIPE buffer stack requires 128 byte aligned buffers, and restricts
 * available headroom to a max of 127 bytes.  As a result of these
 * restrictions, we keep the buffer metadata (mbuf, ofpbuf, etc.) outside the
 * "mPIPE buffer", with some careful alignment jugglery and probing.
 *
 * The layout of each element in the mPIPE mempool is therefore as follows:
 *
 *   +--------------------------------+
 *   | variable sized pad space       |
 *   +--------------------------------+ cacheline boundary
 *   | struct rte_mempool_obj_header  |
 *   +--------------------------------+ cacheline boundary
 *   | struct rte_mbuf/ofpbuf/...     |
 *   +--------------------------------+ 128 byte aligned boundary
 *   | buffer data headroom           |
 *   |                                |
 *   | buffer data                    |
 *   |                                |
 *   +--------------------------------+
 *
 * The addresses pushed into the mPIPE buffer stack are the 128 byte aligned
 * start address of the buffer headroom.  The mempool user transacts in
 * objects that point to the start of the rte_mbuf/ofpbuf structures in the
 * element.  These objects cannot directly be pushed into the mPIPE buffer
 * stack.
 */

#define BSM_MEM_ALIGN		65536
#define BSM_MAX_META		(4 * CACHE_LINE_SIZE)

TAILQ_HEAD(rte_mempool_list, rte_tailq_entry);

const struct rte_mempool *last_mp;

/* register buffer memory with mPIPE. */
static int
__bsm_register_mem(struct rte_mempool *mp, void *mem, size_t size)
{
	uintptr_t pagesz = mp->mz->hugepage_sz;
	uintptr_t start = (uintptr_t)mem & ~(pagesz - 1);
	uintptr_t end = (uintptr_t)mem + size;
	int rc, count = 0;

	while (start < end) {
		rc = gxio_mpipe_register_page(mp->context, mp->stack_idx,
					      (void *)start, pagesz, 0);
		if (rc < 0)
			return rc;
		start += pagesz;
		count ++;
	}
	return count;
}

/* Probe the element constructor to figure out the metadata size. */
static unsigned
__bsm_meta_size(unsigned elt_size, rte_mempool_obj_ctor_t *obj_init,
		void *obj_init_arg)
{
	struct rte_mempool mp;
	unsigned offset;
	union {
		struct rte_mbuf mbuf;
		unsigned char data[elt_size];
	} u;

	if (!obj_init) {
		RTE_LOG(ERR, MEMPOOL, "No constructor to probe meta size!\n");
		return 0;
	}

	memset(&u, 0, sizeof(u));
	memset(&mp, 0, sizeof(mp));

	mp.elt_size = elt_size;

	obj_init(&mp, obj_init_arg, &u.mbuf, 0);

	offset = (uintptr_t)u.mbuf.buf_addr - (uintptr_t)&u.mbuf;
	if (offset > BSM_MAX_META) {
		RTE_LOG(ERR, MEMPOOL, "Meta data overflow (%d > %d)!\n",
			offset, BSM_MAX_META);
		return 0;
	}

	RTE_LOG(ERR, MEMPOOL, "Meta data probed to be %d\n",
		offset);

	return offset;
}

/* create the mempool */
struct rte_mempool *
rte_mempool_create(const char *name, unsigned n, unsigned elt_size,
		   unsigned cache_size __rte_unused, unsigned private_data_size,
		   rte_mempool_ctor_t *mp_init, void *mp_init_arg,
		   rte_mempool_obj_ctor_t *obj_init, void *obj_init_arg,
		   int socket_id, unsigned flags)
{
	unsigned bsm_size, meta_size, pool_size, total_size, i;
	int mz_flags = RTE_MEMZONE_1GB | RTE_MEMZONE_SIZE_HINT_ONLY;
	gxio_mpipe_buffer_size_enum_t size_code;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	gxio_mpipe_context_t *context;
	const struct rte_memzone *mz;
	int rc, instance, stack_idx;
	struct rte_tailq_entry *te;
	struct rte_mempool *mp;
	void *obj, *buf;
	char *mem;

	instance = socket_id % rte_eal_mpipe_instances;
	context = rte_eal_mpipe_context(instance);

	if (!context) {
		RTE_LOG(ERR, MEMPOOL, "No mPIPE context!\n");
		return NULL;
	}

	/* check that we have an initialised tail queue */
	if (RTE_TAILQ_LOOKUP_BY_IDX(RTE_TAILQ_MEMPOOL,
			rte_mempool_list) == NULL) {
		RTE_LOG(ERR, MEMPOOL, "Uninitialized tailq list!\n");
		return NULL;
	}

	/* We cannot operate without hugepages. */
	if (!rte_eal_has_hugepages()) {
		RTE_LOG(ERR, MEMPOOL, "Mempool requires hugepages!\n");
		return NULL;
	}

	/* try to allocate tailq entry */
	te = rte_zmalloc("MEMPOOL_TAILQ_ENTRY", sizeof(*te), 0);
	if (te == NULL) {
		RTE_LOG(ERR, MEMPOOL, "Cannot allocate tailq entry!\n");
		return NULL;
	}

	bsm_size  = gxio_mpipe_calc_buffer_stack_bytes(n);
	bsm_size  = RTE_ALIGN_CEIL(bsm_size, BSM_ALIGN_SIZE);

	pool_size = sizeof(struct rte_mempool) + private_data_size;
	pool_size = RTE_ALIGN_CEIL(pool_size, BSM_ALIGN_SIZE);

	meta_size = __bsm_meta_size(elt_size, obj_init, obj_init_arg);
	size_code = gxio_mpipe_buffer_size_to_buffer_size_enum(elt_size - meta_size);
	elt_size  = gxio_mpipe_buffer_size_enum_to_buffer_size(size_code) + meta_size;

	total_size = (BSM_MEM_ALIGN + bsm_size + pool_size + n * elt_size);

	snprintf(mz_name, sizeof(mz_name), RTE_MEMPOOL_MZ_FORMAT, name);

	rc = gxio_mpipe_alloc_buffer_stacks(context, 1, 0, 0);
	if (rc < 0) {
		RTE_LOG(ERR, MEMPOOL, "Cannot allocate buffer stack!\n");
		return NULL;
	}
	stack_idx = rc;

	rte_rwlock_write_lock(RTE_EAL_MEMPOOL_RWLOCK);

	mz = rte_memzone_reserve(mz_name, total_size, socket_id, mz_flags);
	if (!mz) {
		rte_rwlock_write_unlock(RTE_EAL_MEMPOOL_RWLOCK);
		return NULL;
	}

	mem = mz->addr;
	mp  = mz->addr;
	memset(mp, 0, sizeof(*mp));
	snprintf(mp->name, sizeof(mp->name), "%s", name);
	mp->phys_addr = mz->phys_addr;
	mp->mz = mz;
	mp->size = n;
	mp->flags = flags;
	mp->meta_size = meta_size;
	mp->elt_size = elt_size;
	mp->private_data_size = private_data_size;
	mp->stack_idx = stack_idx;
	mp->size_code = size_code;
	mp->context = context;
	mp->instance = instance;

	mem += pool_size;

	mem = RTE_PTR_ALIGN_CEIL(mem, BSM_MEM_ALIGN);
	rc = gxio_mpipe_init_buffer_stack(context, stack_idx, size_code,
					  mem, bsm_size, 0);
	if (rc < 0) {
		rte_rwlock_write_unlock(RTE_EAL_MEMPOOL_RWLOCK);
		RTE_LOG(ERR, MEMPOOL, "Cannot initialize buffer stack!\n");
		return NULL;
	}
	mem += bsm_size;

	mem = RTE_PTR_ALIGN_CEIL(mem, BSM_ALIGN_SIZE);
	rc = __bsm_register_mem(mp, mem, n * elt_size);
	if (rc < 0) {
		rte_rwlock_write_unlock(RTE_EAL_MEMPOOL_RWLOCK);
		RTE_LOG(ERR, MEMPOOL, "Cannot register buffer mem (%d)!\n", rc);
		return NULL;
	}

	/* call the initializer */
	if (mp_init)
		mp_init(mp, mp_init_arg);

	for (i = 0; i < n; i++) {
		buf = RTE_PTR_ALIGN_CEIL(mem + meta_size, BSM_ALIGN_SIZE);
		obj = __mempool_buf_to_obj(mp, buf);

		if (obj_init)
			obj_init(mp, obj_init_arg, obj, i);
		rte_mempool_mp_put(mp, obj);
		mem = RTE_PTR_ADD(obj, mp->elt_size);
	}

	te->data = (void *) mp;
	RTE_EAL_TAILQ_INSERT_TAIL(RTE_TAILQ_MEMPOOL, rte_mempool_list, te);

	rte_rwlock_write_unlock(RTE_EAL_MEMPOOL_RWLOCK);

	return mp;
}

/* dump the status of the mempool on the console */
void
rte_mempool_dump(FILE *f, const struct rte_mempool *mp)
{
	fprintf(f, "mempool <%s>@%p\n", mp->name, mp);
	fprintf(f, "  flags=%x\n", mp->flags);
	fprintf(f, "  count=%d\n", rte_mempool_count(mp));
}

/* dump the status of all mempools on the console */
void
rte_mempool_list_dump(FILE *f)
{
	const struct rte_mempool *mp = NULL;
	struct rte_tailq_entry *te;
	struct rte_mempool_list *mempool_list;

	if ((mempool_list =
	     RTE_TAILQ_LOOKUP_BY_IDX(RTE_TAILQ_MEMPOOL, rte_mempool_list)) == NULL) {
		rte_errno = E_RTE_NO_TAILQ;
		return;
	}

	rte_rwlock_read_lock(RTE_EAL_MEMPOOL_RWLOCK);

	TAILQ_FOREACH(te, mempool_list, next) {
		mp = (struct rte_mempool *) te->data;
		rte_mempool_dump(f, mp);
	}

	rte_rwlock_read_unlock(RTE_EAL_MEMPOOL_RWLOCK);
}

/* search a mempool from its name */
struct rte_mempool *
rte_mempool_lookup(const char *name)
{
	struct rte_mempool *mp = NULL;
	struct rte_tailq_entry *te;
	struct rte_mempool_list *mempool_list;

	if ((mempool_list =
	     RTE_TAILQ_LOOKUP_BY_IDX(RTE_TAILQ_MEMPOOL, rte_mempool_list)) == NULL) {
		rte_errno = E_RTE_NO_TAILQ;
		return NULL;
	}

	rte_rwlock_read_lock(RTE_EAL_MEMPOOL_RWLOCK);

	TAILQ_FOREACH(te, mempool_list, next) {
		mp = (struct rte_mempool *) te->data;
		if (strncmp(name, mp->name, RTE_MEMPOOL_NAMESIZE) == 0)
			break;
	}

	rte_rwlock_read_unlock(RTE_EAL_MEMPOOL_RWLOCK);

	if (te == NULL) {
		rte_errno = ENOENT;
		return NULL;
	}

	return mp;
}

void rte_mempool_walk(void (*func)(const struct rte_mempool *, void *),
		      void *arg)
{
	struct rte_tailq_entry *te = NULL;
	struct rte_mempool_list *mempool_list;

	if ((mempool_list =
	     RTE_TAILQ_LOOKUP_BY_IDX(RTE_TAILQ_MEMPOOL, rte_mempool_list)) == NULL) {
		rte_errno = E_RTE_NO_TAILQ;
		return;
	}

	rte_rwlock_read_lock(RTE_EAL_MEMPOOL_RWLOCK);

	TAILQ_FOREACH(te, mempool_list, next) {
		(*func)((struct rte_mempool *) te->data, arg);
	}

	rte_rwlock_read_unlock(RTE_EAL_MEMPOOL_RWLOCK);
}
