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

#ifndef _RTE_MEMPOOL_TILE_H_
#define _RTE_MEMPOOL_TILE_H_

/**
 * @file
 * RTE Mempool.
 *
 * A memory pool is an allocator of fixed-size object. It is
 * identified by its name, and uses a ring to store free objects. It
 * provides some other optional services, like a per-core object
 * cache, and an alignment helper to ensure that objects are padded
 * to spread them equally on all RAM channels, ranks, and so on.
 *
 * Objects owned by a mempool should never be added in another
 * mempool. When an object is freed using rte_mempool_put() or
 * equivalent, the object data is not modified; the user can save some
 * meta-data in the object data and retrieve them when allocating a
 * new object.
 *
 * Note: the mempool implementation is not preemptable. A lcore must
 * not be interrupted by another task that uses the same mempool
 * (because it uses a ring which is not preemptable). Also, mempool
 * functions must not be used outside the DPDK environment: for
 * example, in linuxapp environment, a thread that is not created by
 * the EAL must not use mempools. This is due to the per-lcore cache
 * that won't work as rte_lcore_id() will not return a correct value.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/queue.h>

#include <rte_log.h>
#include <rte_debug.h>
#include <rte_lcore.h>
#include <rte_common.h>
#include <rte_memory.h>
#include <rte_prefetch.h>
#include <rte_branch_prediction.h>

#include <rte_mpipe.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTE_MEMPOOL_NAMESIZE	32
#define RTE_MEMPOOL_MZ_PREFIX	"TMP_"
#define	RTE_MEMPOOL_MZ_FORMAT	RTE_MEMPOOL_MZ_PREFIX "%s"
#define	RTE_MEMPOOL_OBJ_NAME	RTE_MEMPOOL_MZ_FORMAT

struct rte_mempool {
	TAILQ_ENTRY(rte_mempool) next;   /**< Next in list. */
	char         name[RTE_MEMPOOL_NAMESIZE]; /**< Name of mempool. */
	uint32_t     flags;              /**< Flags of the mempool. */
	uint32_t     size;               /*<< Number of elements in pool. */
	uint32_t     elt_size;           /**< Size of an element. */
	uint32_t     meta_size;          /**< Size of buffer metadata. */
	int          stack_idx;          /**< mPIPE buffer stack index. */
	int          instance;           /**< mPIPE instance. */
	gxio_mpipe_buffer_size_enum_t size_code; /**< mPIPE buffer size enum. */
	gxio_mpipe_context_t *context;   /**< mPIPE context. */
	uint32_t     private_data_size;  /**< Size of private data. */
	phys_addr_t  phys_addr;          /**< Phys. addr. of mempool struct. */
	const struct rte_memzone *mz;    /**< Memory zone. */

	char         private_data[0] __rte_cache_aligned;
}  __rte_cache_aligned;

#define MEMPOOL_F_NO_SPREAD      0x0001 /**< Do not spread in memory. */
#define MEMPOOL_F_NO_CACHE_ALIGN 0x0002 /**< Do not align objs on cache lines.*/
#define MEMPOOL_F_SP_PUT         0x0004 /**< Default put is "single-producer".*/
#define MEMPOOL_F_SC_GET         0x0008 /**< Default get is "single-consumer".*/

static inline void * __attribute__((always_inline))
__mempool_buf_to_obj(const struct rte_mempool *mp, void *buf)
{
	return RTE_PTR_SUB(buf, mp->meta_size);
}

static inline void * __attribute__((always_inline))
__mempool_obj_to_buf(const struct rte_mempool *mp, void *obj)
{
	return RTE_PTR_ADD(obj, mp->meta_size);
}

/**
 * @internal Put several objects back in the mempool; used internally.
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to store back in the mempool, must be strictly
 *   positive.
 * @param is_mp
 *   Mono-producer (0) or multi-producers (1).
 */
static inline void __attribute__((always_inline))
__mempool_put_bulk(struct rte_mempool *mp, void **obj_table,
		    unsigned n)
{
	unsigned i;
	void *buf;

	for (i = 0; i < n; i++) {
		buf = __mempool_obj_to_buf(mp, obj_table[i]);
		gxio_mpipe_push_buffer(mp->context, mp->stack_idx, buf);
	}
}


/**
 * Put several objects back in the mempool (multi-producers safe).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the mempool from the obj_table.
 */
static inline void __attribute__((always_inline))
rte_mempool_mp_put_bulk(struct rte_mempool *mp, void **obj_table,
			unsigned n)
{
	__mempool_put_bulk(mp, obj_table, n);
}

/**
 * Put several objects back in the mempool (NOT multi-producers safe).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the mempool from obj_table.
 */
static inline void __attribute__((always_inline))
rte_mempool_sp_put_bulk(struct rte_mempool *mp, void **obj_table,
			unsigned n)
{
	__mempool_put_bulk(mp, obj_table, n);
}

/**
 * Put several objects back in the mempool.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * mempool creation time (see flags).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the mempool from obj_table.
 */
static inline void __attribute__((always_inline))
rte_mempool_put_bulk(struct rte_mempool *mp, void **obj_table,
		     unsigned n)
{
	__mempool_put_bulk(mp, obj_table, n);
}

/**
 * Put one object in the mempool (multi-producers safe).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj
 *   A pointer to the object to be added.
 */
static inline void __attribute__((always_inline))
rte_mempool_mp_put(struct rte_mempool *mp, void *obj)
{
	rte_mempool_mp_put_bulk(mp, &obj, 1);
}

/**
 * Put one object back in the mempool (NOT multi-producers safe).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj
 *   A pointer to the object to be added.
 */
static inline void __attribute__((always_inline))
rte_mempool_sp_put(struct rte_mempool *mp, void *obj)
{
	rte_mempool_sp_put_bulk(mp, &obj, 1);
}

/**
 * Put one object back in the mempool.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * mempool creation time (see flags).
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj
 *   A pointer to the object to be added.
 */
static inline void __attribute__((always_inline))
rte_mempool_put(struct rte_mempool *mp, void *obj)
{
	rte_mempool_put_bulk(mp, &obj, 1);
}

/**
 * @internal Get several objects from the mempool; used internally.
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to get, must be strictly positive.
 * @param is_mc
 *   Mono-consumer (0) or multi-consumers (1).
 * @return
 *   - >=0: Success; number of objects supplied.
 *   - <0: Error; code of ring dequeue function.
 */
static inline int __attribute__((always_inline))
__mempool_get_bulk(struct rte_mempool *mp, void **obj_table,
		   unsigned n)
{
	unsigned i;
	void *buf;

	for (i = 0; i < n; i++) {
		buf = gxio_mpipe_pop_buffer(mp->context, mp->stack_idx);
		if (unlikely(!buf)) {
			__mempool_put_bulk(mp, obj_table, i);
			return -ENOENT;
		}
		obj_table[i] = __mempool_buf_to_obj(mp, buf);
		rte_prefetch0(obj_table[i]);
	}
	return i;
}

/**
 * Get several objects from the mempool (multi-consumers safe).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to get from mempool to obj_table.
 * @return
 *   - 0: Success; objects taken.
 *   - -ENOENT: Not enough entries in the mempool; no object is retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_mc_get_bulk(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	return __mempool_get_bulk(mp, obj_table, n);
}

/**
 * Get several objects from the mempool (NOT multi-consumers safe).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to get from the mempool to obj_table.
 * @return
 *   - 0: Success; objects taken.
 *   - -ENOENT: Not enough entries in the mempool; no object is
 *     retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_sc_get_bulk(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	return __mempool_get_bulk(mp, obj_table, n);
}

/**
 * Get several objects from the mempool.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * mempool creation time (see flags).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to get from the mempool to obj_table.
 * @return
 *   - 0: Success; objects taken
 *   - -ENOENT: Not enough entries in the mempool; no object is retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_get_bulk(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	return __mempool_get_bulk(mp, obj_table, n);
}

/**
 * Get one object from the mempool (multi-consumers safe).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success; objects taken.
 *   - -ENOENT: Not enough entries in the mempool; no object is retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_mc_get(struct rte_mempool *mp, void **obj_p)
{
	return rte_mempool_mc_get_bulk(mp, obj_p, 1);
}

/**
 * Get one object from the mempool (NOT multi-consumers safe).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success; objects taken.
 *   - -ENOENT: Not enough entries in the mempool; no object is retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_sc_get(struct rte_mempool *mp, void **obj_p)
{
	return rte_mempool_sc_get_bulk(mp, obj_p, 1);
}

/**
 * Get one object from the mempool.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behavior that was specified at
 * mempool creation (see flags).
 *
 * If cache is enabled, objects will be retrieved first from cache,
 * subsequently from the common pool. Note that it can return -ENOENT when
 * the local cache and common pool are empty, even if cache from other
 * lcores are full.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success; objects taken.
 *   - -ENOENT: Not enough entries in the mempool; no object is retrieved.
 */
static inline int __attribute__((always_inline))
rte_mempool_get(struct rte_mempool *mp, void **obj_p)
{
	return rte_mempool_get_bulk(mp, obj_p, 1);
}

/**
 * An object constructor callback function for mempool.
 *
 * Arguments are the mempool, the opaque pointer given by the user in
 * rte_mempool_create(), the pointer to the element and the index of
 * the element in the pool.
 */
typedef void (rte_mempool_obj_ctor_t)(struct rte_mempool *, void *,
				      void *, unsigned);

/**
 * A mempool constructor callback function.
 *
 * Arguments are the mempool and the opaque pointer given by the user in
 * rte_mempool_create().
 */
typedef void (rte_mempool_ctor_t)(struct rte_mempool *, void *);

/**
 * Creates a new mempool named *name* in memory.
 *
 * This function uses ``memzone_reserve()`` to allocate memory. The
 * pool contains n elements of elt_size. Its size is set to n.
 * All elements of the mempool are allocated together with the mempool header,
 * in one physically continuous chunk of memory.
 *
 * @param name
 *   The name of the mempool.
 * @param n
 *   The number of elements in the mempool. The optimum size (in terms of
 *   memory usage) for a mempool is when n is a power of two minus one:
 *   n = (2^q - 1).
 * @param elt_size
 *   The size of each element.
 * @param cache_size
 *   If cache_size is non-zero, the rte_mempool library will try to
 *   limit the accesses to the common lockless pool, by maintaining a
 *   per-lcore object cache. This argument must be lower or equal to
 *   CONFIG_RTE_MEMPOOL_CACHE_MAX_SIZE. It is advised to choose
 *   cache_size to have "n modulo cache_size == 0": if this is
 *   not the case, some elements will always stay in the pool and will
 *   never be used. The access to the per-lcore table is of course
 *   faster than the multi-producer/consumer pool. The cache can be
 *   disabled if the cache_size argument is set to 0; it can be useful to
 *   avoid losing objects in cache. Note that even if not used, the
 *   memory space for cache is always reserved in a mempool structure,
 *   except if CONFIG_RTE_MEMPOOL_CACHE_MAX_SIZE is set to 0.
 * @param private_data_size
 *   The size of the private data appended after the mempool
 *   structure. This is useful for storing some private data after the
 *   mempool structure, as is done for rte_mbuf_pool for example.
 * @param mp_init
 *   A function pointer that is called for initialization of the pool,
 *   before object initialization. The user can initialize the private
 *   data in this function if needed. This parameter can be NULL if
 *   not needed.
 * @param mp_init_arg
 *   An opaque pointer to data that can be used in the mempool
 *   constructor function.
 * @param obj_init
 *   A function pointer that is called for each object at
 *   initialization of the pool. The user can set some meta data in
 *   objects if needed. This parameter can be NULL if not needed.
 *   The obj_init() function takes the mempool pointer, the init_arg,
 *   the object pointer and the object number as parameters.
 * @param obj_init_arg
 *   An opaque pointer to data that can be used as an argument for
 *   each call to the object constructor function.
 * @param socket_id
 *   The *socket_id* argument is the socket identifier in the case of
 *   NUMA. The value can be *SOCKET_ID_ANY* if there is no NUMA
 *   constraint for the reserved zone.
 * @param flags
 *   The *flags* arguments is an OR of following flags:
 *   - MEMPOOL_F_NO_SPREAD: By default, objects addresses are spread
 *     between channels in RAM: the pool allocator will add padding
 *     between objects depending on the hardware configuration. See
 *     Memory alignment constraints for details. If this flag is set,
 *     the allocator will just align them to a cache line.
 *   - MEMPOOL_F_NO_CACHE_ALIGN: By default, the returned objects are
 *     cache-aligned. This flag removes this constraint, and no
 *     padding will be present between objects. This flag implies
 *     MEMPOOL_F_NO_SPREAD.
 *   - MEMPOOL_F_SP_PUT: If this flag is set, the default behavior
 *     when using rte_mempool_put() or rte_mempool_put_bulk() is
 *     "single-producer". Otherwise, it is "multi-producers".
 *   - MEMPOOL_F_SC_GET: If this flag is set, the default behavior
 *     when using rte_mempool_get() or rte_mempool_get_bulk() is
 *     "single-consumer". Otherwise, it is "multi-consumers".
 * @return
 *   The pointer to the new allocated mempool, on success. NULL on error
 *   with rte_errno set appropriately. Possible rte_errno values include:
 *    - E_RTE_NO_CONFIG - function could not get pointer to rte_config structure
 *    - E_RTE_SECONDARY - function was called from a secondary process instance
 *    - E_RTE_NO_TAILQ - no tailq list could be got for the ring or mempool list
 *    - EINVAL - cache size provided is too large
 *    - ENOSPC - the maximum number of memzones has already been allocated
 *    - EEXIST - a memzone with the same name already exists
 *    - ENOMEM - no appropriate memory area found in which to create memzone
 */
struct rte_mempool *
rte_mempool_create(const char *name, unsigned n, unsigned elt_size,
		   unsigned cache_size, unsigned private_data_size,
		   rte_mempool_ctor_t *mp_init, void *mp_init_arg,
		   rte_mempool_obj_ctor_t *obj_init, void *obj_init_arg,
		   int socket_id, unsigned flags);

/**
 * Search a mempool from its name
 *
 * @param name
 *   The name of the mempool.
 * @return
 *   The pointer to the mempool matching the name, or NULL if not found.
 *   NULL on error
 *   with rte_errno set appropriately. Possible rte_errno values include:
 *    - ENOENT - required entry not available to return.
 *
 */
struct rte_mempool *rte_mempool_lookup(const char *name);

/**
 * Dump the status of the mempool to the console.
 *
 * @param f
 *   A pointer to a file for output
 * @param mp
 *   A pointer to the mempool structure.
 */
void rte_mempool_dump(FILE *f, const struct rte_mempool *mp);

/**
 * Dump the status of all mempools on the console
 *
 * @param f
 *   A pointer to a file for output
 */
void rte_mempool_list_dump(FILE *f);

/**
 * Walk list of all memory pools
 *
 * @param func
 *   Iterator function
 * @param arg
 *   Argument passed to iterator
 */
void rte_mempool_walk(void (*func)(const struct rte_mempool *, void *arg),
		      void *arg);


/**
 * Return the physical address of elt, which is an element of the pool mp.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param elt
 *   A pointer (virtual address) to the element of the pool.
 * @return
 *   The physical address of the elt element.
 */
static inline phys_addr_t __attribute__((always_inline))
rte_mempool_virt2phy(const struct rte_mempool *mp, const void *elt)
{
	uintptr_t off = (const char *)elt - (const char *)mp;
	return mp->phys_addr + off;
}

/**
 * Return a pointer to the private data in an mempool structure.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @return
 *   A pointer to the private data.
 */
static inline void *rte_mempool_get_priv(struct rte_mempool *mp)
{
	return mp->private_data;
}

/**
 * Return the number of entries in the mempool.
 *
 * When cache is enabled, this function has to browse the length of
 * all lcores, so it should not be used in a data path, but only for
 * debug purposes.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @return
 *   The number of entries in the mempool.
 */
static inline unsigned
rte_mempool_count(const struct rte_mempool *mp)
{
	return gxio_mpipe_get_buffer_count(mp->context, mp->stack_idx);
}

#ifdef __cplusplus
}
#endif

#endif /* _RTE_MEMPOOL_TILE_H_ */
