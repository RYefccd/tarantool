/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "sysalloc.h"

#include <small/slab_arena.h>

#if TARGET_OS_DARWIN
#include <malloc/malloc.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_size(p);
}
#elif (TARGET_OS_FREEBSD || TARGET_OS_NETBSD || TARGET_OS_OPENBSD)
#include <malloc_np.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_usable_size(p);
}
#elif TARGET_OS_LINUX
#include <malloc.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_usable_size(p);
}
#else
#error "Undefined system type"
#endif

static inline void
system_collect_garbage(struct system_alloc *alloc)
{
	if (alloc->free_mode != SYSTEM_COLLECT_GARBAGE)
		return;

	const int BATCH = 100;
	if (!lifo_is_empty(&alloc->delayed)) {
		for (int i = 0; i < BATCH; i++) {
			void *item = lifo_pop(&alloc->delayed);
			if (item == NULL)
				break;
			sysfree(alloc, item, 0 /* unused parameter */);
		}
	} else {
		/* Finish garbage collection and switch to regular mode */
		alloc->free_mode = SYSTEM_FREE;
	}
}

void
system_alloc_setopt(struct system_alloc *alloc, enum system_opt opt, bool val)
{
	switch (opt) {
	case SYSTEM_DELAYED_FREE_MODE:
		alloc->free_mode = val ? SYSTEM_DELAYED_FREE :
			SYSTEM_COLLECT_GARBAGE;
		break;
	default:
		assert(false);
		break;
	}
}

void
system_stats(struct system_alloc *alloc, struct system_stats *totals,
	     system_stats_cb cb, void *cb_ctx)
{
	totals->used = pm_atomic_load_explicit(&alloc->used_bytes,
		pm_memory_order_relaxed);
	totals->total = quota_total(alloc->quota);
	cb(totals, cb_ctx);
}

void
system_alloc_create(struct system_alloc *alloc, struct quota *quota)
{
	alloc->used_bytes = 0;
	alloc->quota = quota;
	lifo_init(&alloc->delayed);
}

void
system_alloc_destroy(MAYBE_UNUSED struct system_alloc *alloc)
{

}

void
sysfree(struct system_alloc *alloc, void *ptr, MAYBE_UNUSED size_t bytes)
{
	size_t size = portable_malloc_usable_size(ptr);
	uint32_t s = size % QUOTA_UNIT_SIZE, units = size / QUOTA_UNIT_SIZE;
	size_t used_bytes =  pm_atomic_fetch_sub(&alloc->used_bytes, size);
	if (small_align(used_bytes, QUOTA_UNIT_SIZE) >
	    small_align(used_bytes - s, QUOTA_UNIT_SIZE))
		units++;
	if (units > 0)
		quota_release(alloc->quota, units * QUOTA_UNIT_SIZE);
	free(ptr);
}

void
sysfree_delayed(struct system_alloc *alloc, void *ptr, size_t bytes)
{
	if (alloc->free_mode == SYSTEM_DELAYED_FREE && ptr) {
		lifo_push(&alloc->delayed, ptr);
	} else {
		sysfree(alloc, ptr, bytes);
	}
}

void *
sysalloc(struct system_alloc *alloc, size_t bytes)
{
	system_collect_garbage(alloc);

	void *ptr = malloc(bytes);
	if (!ptr)
		return NULL;
	size_t size = portable_malloc_usable_size(ptr);
	uint32_t s = size % QUOTA_UNIT_SIZE, units = size / QUOTA_UNIT_SIZE;
	while (1) {
		size_t used_bytes =  pm_atomic_load(&alloc->used_bytes);
		if (small_align(used_bytes, QUOTA_UNIT_SIZE) <
		    small_align(used_bytes + s, QUOTA_UNIT_SIZE))
			units++;
		if (units > 0) {
			if (quota_use(alloc->quota,
				units * QUOTA_UNIT_SIZE) < 0) {
				free(ptr);
				return NULL;
			}
		}
		if (pm_atomic_compare_exchange_strong(&alloc->used_bytes,
			&used_bytes, used_bytes + size))
			break;
		if (units > 0)
			quota_release(alloc->quota, units * QUOTA_UNIT_SIZE);
	}
	return ptr;
}
