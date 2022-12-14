/*
 * Copyright (C) 2012-2018 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

void bo_del(struct fd_bo *bo);
extern simple_mtx_t table_lock;

static void
add_bucket(struct fd_bo_cache *cache, int size)
{
   unsigned int i = cache->num_buckets;

   assert(i < ARRAY_SIZE(cache->cache_bucket));

   list_inithead(&cache->cache_bucket[i].list);
   cache->cache_bucket[i].size = size;
   cache->num_buckets++;
}

/**
 * @coarse: if true, only power-of-two bucket sizes, otherwise
 *    fill in for a bit smoother size curve..
 */
void
fd_bo_cache_init(struct fd_bo_cache *cache, int coarse)
{
   unsigned long size, cache_max_size = 64 * 1024 * 1024;

   /* OK, so power of two buckets was too wasteful of memory.
    * Give 3 other sizes between each power of two, to hopefully
    * cover things accurately enough.  (The alternative is
    * probably to just go for exact matching of sizes, and assume
    * that for things like composited window resize the tiled
    * width/height alignment and rounding of sizes to pages will
    * get us useful cache hit rates anyway)
    */
   add_bucket(cache, 4096);
   add_bucket(cache, 4096 * 2);
   if (!coarse)
      add_bucket(cache, 4096 * 3);

   /* Initialize the linked lists for BO reuse cache. */
   for (size = 4 * 4096; size <= cache_max_size; size *= 2) {
      add_bucket(cache, size);
      if (!coarse) {
         add_bucket(cache, size + size * 1 / 4);
         add_bucket(cache, size + size * 2 / 4);
         add_bucket(cache, size + size * 3 / 4);
      }
   }
}

/* Frees older cached buffers.  Called under table_lock */
void
fd_bo_cache_cleanup(struct fd_bo_cache *cache, time_t time)
{
   int i;

   simple_mtx_assert_locked(&table_lock);

   if (cache->time == time)
      return;

   for (i = 0; i < cache->num_buckets; i++) {
      struct fd_bo_bucket *bucket = &cache->cache_bucket[i];
      struct fd_bo *bo;

      while (!list_is_empty(&bucket->list)) {
         bo = list_entry(bucket->list.next, struct fd_bo, list);

         /* keep things in cache for at least 1 second: */
         if (time && ((time - bo->free_time) <= 1))
            break;

         VG_BO_OBTAIN(bo);
         list_del(&bo->list);
         bo_del(bo);
      }
   }

   cache->time = time;
}

static struct fd_bo_bucket *
get_bucket(struct fd_bo_cache *cache, uint32_t size)
{
   int i;

   /* hmm, this is what intel does, but I suppose we could calculate our
    * way to the correct bucket size rather than looping..
    */
   for (i = 0; i < cache->num_buckets; i++) {
      struct fd_bo_bucket *bucket = &cache->cache_bucket[i];
      if (bucket->size >= size) {
         return bucket;
      }
   }

   return NULL;
}

static struct fd_bo *
find_in_bucket(struct fd_bo_bucket *bucket, uint32_t flags)
{
   struct fd_bo *bo = NULL;

   /* TODO .. if we had an ALLOC_FOR_RENDER flag like intel, we could
    * skip the busy check.. if it is only going to be a render target
    * then we probably don't need to stall..
    *
    * NOTE that intel takes ALLOC_FOR_RENDER bo's from the list tail
    * (MRU, since likely to be in GPU cache), rather than head (LRU)..
    */
   simple_mtx_lock(&table_lock);
   list_for_each_entry (struct fd_bo, entry, &bucket->list, list) {
      if (fd_bo_state(entry) != FD_BO_STATE_IDLE)
         break;
      if (entry->alloc_flags == flags) {
         bo = entry;
         list_del(&bo->list);
         break;
      }
   }
   simple_mtx_unlock(&table_lock);

   return bo;
}

/* NOTE: size is potentially rounded up to bucket size: */
struct fd_bo *
fd_bo_cache_alloc(struct fd_bo_cache *cache, uint32_t *size, uint32_t flags)
{
   struct fd_bo *bo = NULL;
   struct fd_bo_bucket *bucket;

   *size = align(*size, 4096);
   bucket = get_bucket(cache, *size);

   /* see if we can be green and recycle: */
retry:
   if (bucket) {
      *size = bucket->size;
      bo = find_in_bucket(bucket, flags);
      if (bo) {
         VG_BO_OBTAIN(bo);
         if (bo->funcs->madvise(bo, true) <= 0) {
            /* we've lost the backing pages, delete and try again: */
            simple_mtx_lock(&table_lock);
            bo_del(bo);
            simple_mtx_unlock(&table_lock);
            goto retry;
         }
         p_atomic_set(&bo->refcnt, 1);
         bo->reloc_flags = FD_RELOC_FLAGS_INIT;
         return bo;
      }
   }

   return NULL;
}

int
fd_bo_cache_free(struct fd_bo_cache *cache, struct fd_bo *bo)
{
   simple_mtx_assert_locked(&table_lock);

   if (bo->nosync || bo->shared)
      return -1;

   struct fd_bo_bucket *bucket = get_bucket(cache, bo->size);

   /* see if we can be green and recycle: */
   if (bucket) {
      struct timespec time;

      bo->funcs->madvise(bo, false);

      clock_gettime(CLOCK_MONOTONIC, &time);

      bo->free_time = time.tv_sec;
      VG_BO_RELEASE(bo);
      list_addtail(&bo->list, &bucket->list);
      fd_bo_cache_cleanup(cache, time.tv_sec);

      return 0;
   }

   return -1;
}
