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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>
#include <rte_malloc.h>

#include "test.h"

/*
 * Mempool
 * =======
 *
 * Basic tests: done on one core with and without cache:
 *
 *    - Get one object, put one object
 *    - Get two objects, put two objects
 *    - Get all objects, test that their content is not modified and
 *      put them back in the pool.
 */

#define MEMPOOL_ELT_SIZE 2048
#define MEMPOOL_SIZE (8 * 1024)

static struct rte_mempool *mp;


/*
 * save the object number in the first 4 bytes of object data. All
 * other bytes are set to 0.
 */
static void
my_obj_init(struct rte_mempool *mp, __attribute__((unused)) void *arg,
	    void *obj, unsigned i)
{
	uint32_t *objnum = obj;
	memset(obj, 0, mp->elt_size);
	*objnum = i;
}

/* basic tests (done on one core) */
static int
test_mempool_basic(void)
{
	uint32_t *objnum;
	void **objtable;
	void *obj, *obj2;
	char *obj_data;
	int ret = 0;
	unsigned i, j;

	/* dump the mempool status */
	rte_mempool_dump(stdout, mp);

	printf("get an object\n");
	if (rte_mempool_get(mp, &obj) < 0)
		return -1;
	rte_mempool_dump(stdout, mp);

	printf("get private data\n");
	if (rte_mempool_get_priv(mp) != (char*) mp + sizeof(*mp))
		return -1;

	printf("get physical address of an object\n");
	if (rte_mempool_virt2phy(mp, obj) !=
			(phys_addr_t) (mp->phys_addr +
			(phys_addr_t) ((char*) obj - (char*) mp)))
		return -1;

	printf("put the object back\n");
	rte_mempool_put(mp, obj);
	rte_mempool_dump(stdout, mp);

	printf("get 2 objects\n");
	if (rte_mempool_get(mp, &obj) < 0)
		return -1;
	if (rte_mempool_get(mp, &obj2) < 0) {
		rte_mempool_put(mp, obj);
		return -1;
	}
	rte_mempool_dump(stdout, mp);

	printf("put the objects back\n");
	rte_mempool_put(mp, obj);
	rte_mempool_put(mp, obj2);
	rte_mempool_dump(stdout, mp);

	/*
	 * get many objects: we cannot get them all because the cache
	 * on other cores may not be empty.
	 */
	objtable = malloc(MEMPOOL_SIZE * sizeof(void *));
	if (objtable == NULL) {
		return -1;
	}

	for (i=0; i<MEMPOOL_SIZE; i++) {
		if (rte_mempool_get(mp, &objtable[i]) < 0)
			break;
	}

	printf("got %d buffers\n", i);
	rte_mempool_dump(stdout, mp);

	/*
	 * for each object, check that its content was not modified,
	 * and put objects back in pool
	 */
	while (i--) {
		obj = objtable[i];
		obj_data = obj;
		objnum = obj;
		if (*objnum > MEMPOOL_SIZE) {
			printf("bad object number %u @%p\n", *objnum, objnum);
			ret = -1;
			break;
		}
		for (j=sizeof(*objnum); j<mp->elt_size; j++) {
			if (obj_data[j] != 0)
				ret = -1;
		}

		rte_mempool_put(mp, objtable[i]);
	}

	free(objtable);
	if (ret == -1)
		printf("objects were modified!\n");

	return ret;
}

static int
test_mempool(void)
{
	/* create a mempool (without cache) */
	if (mp == NULL)
		mp = rte_mempool_create("test", MEMPOOL_SIZE,
						MEMPOOL_ELT_SIZE, 0, 0,
						NULL, NULL,
						my_obj_init, NULL,
						SOCKET_ID_ANY, 0);
	if (mp == NULL)
		return -1;

	/* retrieve the mempool from its name */
	if (rte_mempool_lookup("test") != mp) {
		printf("Cannot lookup mempool from its name\n");
		return -1;
	}

	/* basic tests without cache */
	mp = mp;
	if (test_mempool_basic() < 0)
		return -1;

	return 0;
}

static struct test_command mempool_cmd = {
	.command = "mempool_autotest",
	.callback = test_mempool,
};
REGISTER_TEST_COMMAND(mempool_cmd);
