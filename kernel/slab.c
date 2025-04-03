#include "slab.h"

#include "riscv.h"
#include "types.h"

#include "debug.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "spinlock.h"

void print_kmem_cache(struct kmem_cache *cache,
                      void (*slab_obj_printer)(void *)) {
  // debug("slab.c: print_kmem_cache\n");
  printf(
      "[SLAB] kmem_cache { name: %s, object_size: %u, at: %p, in_cache_obj: %d "
      "}\n",
      cache->name, cache->object_size, cache, 0);

  if (!list_empty(&cache->partial)) {
    printf("[SLAB]  [ partial slabs ]\n");
    struct slab *slab;
    list_for_each_entry(slab, &cache->partial, neighbors) {
      printf("[SLAB]   [ slab %p ] { freelist: %p, nxt: %p }\n", slab,
             slab->freelist, slab_entry(slab->neighbors.next));

      int obj_count = (PGSIZE - sizeof(struct slab)) / cache->object_size;
      char *obj = (char *)slab + sizeof(struct slab);
      for (int i = 0; i < obj_count; ++i) {
        printf("[SLAB]    [ idx %d ] { addr: %p, as_ptr: %p", i, obj,
               ((struct run *)obj)->next);
        if (slab_obj_printer) {
          printf(", as_obj: {");
          slab_obj_printer(obj);
          printf("}");
        }
        printf(" }\n");
        obj += cache->object_size;
      }
    }
  }

  if (!list_empty(&cache->full)) {
    debug("[slab]  [ full slabs ]\n");
    struct slab *slab;
    list_for_each_entry(slab, &cache->full, neighbors) {
      debug("[slab]   [ slab %p ] { freelist: %p, nxt: %p }\n", slab,
            slab->freelist, slab_entry(slab->neighbors.next));

      int obj_count = (PGSIZE - sizeof(struct slab)) / cache->object_size;
      char *obj = (char *)slab + sizeof(struct slab);
      for (int i = 0; i < obj_count; ++i) {
        debug("[slab]    [ idx %d ] { addr: %p, as_ptr: %p", i, obj,
              ((struct run *)obj)->next);
        if (slab_obj_printer) {
          debug(", as_obj: {");
          slab_obj_printer(obj);
          debug("}");
        }
        debug(" }\n");
        obj += cache->object_size;
      }
    }
  }

  printf("[SLAB] print_kmem_cache end\n");
}

struct slab *new_slab(uint object_size) {
  struct slab *slab = kalloc();
  int obj_count = (PGSIZE - sizeof(struct slab)) / object_size;
  debug("new_slab: obj_count=%d\n", obj_count);
  CHECK(obj_count > 0);

  slab->allocated = 0;

  char *run = (char *)slab + sizeof(struct slab);
  slab->freelist = (struct run *)run;
  debug("slab.c: new_slab: slab=%p, slab->freelist=%p\n", slab, slab->freelist);
  for (int i = 0; i < obj_count - 1; ++i) {
    char *next = run + object_size;
    // debug("new_slab: next=%p\n", next);
    ((struct run *)run)->next = (struct run *)next;
    run = next;
  }
  ((struct run *)run)->next = 0;

  INIT_LIST_HEAD(&slab->neighbors);
  return slab;
}

void *slab_alloc(struct slab *slab) {
  ++slab->allocated;
  void *ret = slab->freelist;
  slab->freelist = slab->freelist->next;
  return ret;
}

void slab_free(struct slab *slab, void *obj) {
  --slab->allocated;
  ((struct run *)obj)->next = slab->freelist;
  slab->freelist = (struct run *)obj;
}

struct kmem_cache *kmem_cache_create(char *name, uint object_size) {
  struct kmem_cache *cache = kalloc();
  memmove(cache->name, name, MP2_CACHE_MAX_NAME);
  cache->object_size = object_size;
  initlock(&cache->lock, "kmem_cache");

  INIT_LIST_HEAD(&cache->full);
  INIT_LIST_HEAD(&cache->partial);
  // INIT_LIST_HEAD(&cache->free);
  // cache->available_count = 0;

  printf(
      "[SLAB] New kmem_cache (name: %s, object size: %u bytes, at: %p, "
      "max objects per slab: %lu, support in cache obj: %d) is created\n",
      cache->name, cache->object_size, cache,
      (PGSIZE - sizeof(struct slab)) / object_size, 0);
  return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache) {}

void *kmem_cache_alloc(struct kmem_cache *cache) {
  printf("[SLAB] Alloc request on cache %s\n", cache->name);

  // TODO: use cache as slab

  struct slab *slab;
  void *ret;
  if (!list_empty(&cache->partial)) {
    slab = slab_entry(cache->partial.next);
    ret = slab_alloc(slab);
    if (!slab->freelist) {  // Slab is full.
      list_del_init(&slab->neighbors);
      list_add(&slab->neighbors, &cache->full);
    }
  } else {
    slab = new_slab(cache->object_size);
    ret = slab_alloc(slab);
    printf("[SLAB] A new slab %p (%s) is allocated\n", slab, cache->name);
    list_add(&slab->neighbors, &cache->partial);
  }
  printf("[SLAB] Object %p in slab %p (%s) is allocated and initialized\n", ret,
         slab, cache->name);
  return ret;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
  // debug("slab.c: kmem_cache_free: obj=%p; before free:    ", obj);
  // print_kmem_cache(cache, 0);

  // Find slab that contains `obj`.
  struct slab *slab;
  if (!list_empty(&cache->full)) {
    list_for_each_entry(slab, &cache->full, neighbors) {
      debug("slab.c: kmem_cache_free: finding obj in full slab=%p\n", slab);
      if ((char *)slab <= (char *)obj && (char *)obj < (char *)slab + PGSIZE) {
        printf("[SLAB] Free %p in slab %p (%s)\n", obj, slab, cache->name);
        list_del_init(&slab->neighbors);

        if (slab->allocated == 1) {  // full -> free
          kfree(slab);
          printf("[SLAB] slab %p (%s) is freed due to save memory\n", slab,
                 cache->name);
        } else {  // full -> partial
          slab_free(slab, obj);
          list_add(&slab->neighbors, &cache->partial);
        }

        printf("[SLAB] End of free\n");
        return;
      }
    }
  }

  if (!list_empty(&cache->partial)) {
    list_for_each_entry(slab, &cache->partial, neighbors) {
      debug("slab.c: kmem_cache_free: finding obj in partial slab=%p\n", slab);
      if ((char *)slab <= (char *)obj && (char *)obj < (char *)slab + PGSIZE) {
        printf("[SLAB] Free %p in slab %p (%s)\n", obj, slab, cache->name);

        if (slab->allocated == 1) {  // partial -> free
          list_del_init(&slab->neighbors);
          kfree(slab);
          printf("[SLAB] slab %p (%s) is freed due to save memory\n", slab,
                 cache->name);
        } else {  // partial -> partial
          slab_free(slab, obj);
        }

        printf("[SLAB] End of free\n");
        return;
      }
    }
  }

  debug("slab.c: kmem_cache_free: ERROR target not found\n");
  // CHECK(0);
  //  debug("slab.c: kmem_cache_free: obj=%p; after free:    ", obj);
  //  print_kmem_cache(cache, 0);
  //  debug("\n");
}
