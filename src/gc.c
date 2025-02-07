/*
 * gc.c
 * Copyright (C) 2019 Marc Kirchner
 *
 * Distributed under terms of the MIT license.
 *
 * Implements a simple mark & sweep garbage collector.
 */

#include "gc.h"
#include "log.h"

#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "primes.h"

/*
 * Set log level for this compilation unit. If set to LOGLEVEL_DEBUG,
 * the garbage collector will be very chatty.
 */
#undef LOGLEVEL
#define LOGLEVEL LOGLEVEL_INFO

/*
 * Allocations can temporarily be tagged as "marked" an part of the
 * mark-and-sweep implementation or can be tagged as "roots" which are
 * not automatically garbage collected. The latter allows the implementation
 * of global variables.
 */
#define GC_TAG_NONE 0x0
#define GC_TAG_ROOT 0x1
#define GC_TAG_MARK 0x2

/*
 * Store allocations in a hash map with the pointer address
 * as the key.
 */


typedef struct Allocation {
    void* ptr;                // mem pointer
    size_t size;              // allocated size in bytes
    char tag;
    void (*dtor)(void*);      // destructor
    struct Allocation* next;  // separate chaining
} Allocation;

typedef struct AllocationMap {
    size_t capacity;
    size_t min_capacity;
    double downsize_factor;
    double upsize_factor;
    double sweep_factor;
    size_t sweep_limit;
    size_t size;
    Allocation** allocs;
} AllocationMap;

GarbageCollector gc; // global GC object

static Allocation* gc_allocation_new(void* ptr, size_t size, void (*dtor)(void*))
{
    Allocation* a = (Allocation*) malloc(sizeof(Allocation));
    a->ptr = ptr;
    a->size = size;
    a->tag = GC_TAG_NONE;
    a->dtor = dtor;
    a->next = NULL;
    return a;
}

static void gc_allocation_delete(Allocation* a)
{
    free(a);
}

static double gc_allocation_map_load_factor(AllocationMap* am)
{
    return (double) am->size / (double) am->capacity;
}

static AllocationMap* gc_allocation_map_new(size_t min_capacity,
        size_t capacity,
        double sweep_factor,
        double downsize_factor,
        double upsize_factor)
{
    AllocationMap* am = (AllocationMap*) malloc(sizeof(AllocationMap));
    am->min_capacity = next_prime(min_capacity);
    am->capacity = next_prime(capacity);
    if (am->capacity < am->min_capacity) am->capacity = am->min_capacity;
    am->sweep_factor = sweep_factor;
    am->sweep_limit = (int) (sweep_factor * am->capacity);
    am->downsize_factor = downsize_factor;
    am->upsize_factor = upsize_factor;
    am->allocs = (Allocation**) calloc(am->capacity, sizeof(Allocation*));
    am->size = 0;
    LOG_DEBUG("Created allocation map (cap=%ld, siz=%ld)", am->capacity, am->size);
    return am;
}

static void gc_allocation_map_delete(AllocationMap* am)
{
    // Iterate over the map
    LOG_DEBUG("Deleting allocation map (cap=%ld, siz=%ld)",
              am->capacity, am->size);
    Allocation *alloc, *tmp;
    for (size_t i = 0; i < am->capacity; ++i) {
        if ((alloc = am->allocs[i])) {
            // Make sure to follow the chain inside a bucket
            while (alloc) {
                tmp = alloc;
                alloc = alloc->next;
                // free the management structure
                gc_allocation_delete(tmp);
            }
        }
    }
    free(am->allocs);
    free(am);
}

static size_t gc_hash(void *ptr)
{
    return ((uintptr_t)ptr) >> 3;
}

static void gc_allocation_map_resize(AllocationMap* am, size_t new_capacity)
{
    if (new_capacity <= am->min_capacity) {
        return;
    }
    // Replaces the existing items array in the hash table
    // with a resized one and pushes items into the new, correct buckets
    LOG_DEBUG("Resizing allocation map (cap=%ld, siz=%ld) -> (cap=%ld)",
              am->capacity, am->size, new_capacity);
    Allocation** resized_allocs = calloc(new_capacity, sizeof(Allocation*));

    for (size_t i = 0; i < am->capacity; ++i) {
        Allocation* alloc = am->allocs[i];
        while (alloc) {
            Allocation* next_alloc = alloc->next;
            size_t new_index = gc_hash(alloc->ptr) % new_capacity;
            alloc->next = resized_allocs[new_index];
            resized_allocs[new_index] = alloc;
            alloc = next_alloc;
        }
    }
    free(am->allocs);
    am->capacity = new_capacity;
    am->allocs = resized_allocs;
    am->sweep_limit = am->size + am->sweep_factor * (am->capacity - am->size);
}


static Allocation* gc_allocation_map_put(AllocationMap* am,
        void* ptr,
        size_t size,
        void (*dtor)(void*))
{
    size_t index = gc_hash(ptr) % am->capacity;
    LOG_DEBUG("PUT request for allocation ix=%ld", index);
    Allocation* alloc = gc_allocation_new(ptr, size, dtor);
    Allocation* cur = am->allocs[index];
    Allocation* prev = NULL;
    /* Upsert if ptr is already known (e.g. dtor update). */
    while(cur != NULL) {
        if (cur->ptr == ptr) {
            // found it
            alloc->next = cur->next;
            if (!prev) {
                // position 0
                am->allocs[index] = alloc;
            } else {
                // in the list
                prev->next = alloc;
            }
            gc_allocation_delete(cur);
            LOG_DEBUG("AllocationMap Upsert at ix=%ld", index);
            return alloc;

        }
        prev = cur;
        cur = cur->next;
    }
    /* Insert at the front of the separate chaining list */
    cur = am->allocs[index];
    alloc->next = cur;
    am->allocs[index] = alloc;
    am->size++;
    LOG_DEBUG("AllocationMap insert at ix=%ld", index);
    /* Test if we need to increase the size of the allocation map */
    double load_factor = gc_allocation_map_load_factor(am);
    if (load_factor > am->upsize_factor) {
        LOG_DEBUG("Load factor %0.3g > %0.3g. Triggering upsize.", load_factor, am->upsize_factor);
        gc_allocation_map_resize(am, next_prime(am->capacity * 2));
    }
    return alloc;
}


static Allocation* gc_allocation_map_get(AllocationMap* am, void* ptr)
{
    size_t index = gc_hash(ptr) % am->capacity;
    // LOG_DEBUG("GET request for allocation ix=%ld (ptr=%p)", index, ptr);
    Allocation* cur = am->allocs[index];
    while(cur) {
        if (cur->ptr == ptr) return cur;
        cur = cur->next;
    }
    return NULL;
}


static void gc_allocation_map_remove(AllocationMap* am, void* ptr)
{
    // ignores unknown keys
    size_t index = gc_hash(ptr) % am->capacity;
    Allocation* cur = am->allocs[index];
    Allocation* prev = NULL;
    while(cur != NULL) {
        if (cur->ptr == ptr) {
            // found it
            if (!prev) {
                // first item in list
                am->allocs[index] = cur->next;
            } else {
                // not the first item in the list
                prev->next = cur->next;
            }
            gc_allocation_delete(cur);
            am->size--;
        } else {
            // move on
            prev = cur;
        }
        cur = cur->next;
    }
    double load_factor = gc_allocation_map_load_factor(am);
    if (load_factor < am->downsize_factor) {
        LOG_DEBUG("Load factor %0.3g < %0.3g. Triggering downsize.", load_factor, am->downsize_factor);
        gc_allocation_map_resize(am, next_prime(am->capacity / 2));
    }
}


static void* gc_mcalloc(size_t count, size_t size)
{
    if (!count) return malloc(size);
    return calloc(count, size);
}


static void* gc_allocate(GarbageCollector* gc, size_t count, size_t size, void(*dtor)(void*))
{
    /* Allocation logic that generalizes over malloc/calloc. */

    /* Attempt to allocate memory */
    void* ptr = gc_mcalloc(count, size);
    size_t alloc_size = count ? count * size : size;
    /* If allocation fails, attempt to free some memory and try again. */
    if (!ptr && (errno == EAGAIN || errno == ENOMEM)) {
        gc_run(gc);
        ptr = gc_mcalloc(count, size);
    }
    /* Start managing the memory we received from the system */
    if (ptr) {
        LOG_DEBUG("Allocated %zu bytes at %p", alloc_size, (void*) ptr);
        Allocation* alloc = gc_allocation_map_put(gc->allocs, ptr, alloc_size, dtor);
        /* Deal with metadata allocation failure */
        if (alloc) {
            LOG_DEBUG("Managing %zu bytes at %p", alloc_size, (void*) alloc->ptr);
            if (gc->allocs->size > gc->allocs->sweep_limit) {
                size_t freed_mem = gc_run(gc);
                LOG_DEBUG("Garbage collection cleaned up %lu bytes.", freed_mem);
            }
            ptr = alloc->ptr;
        } else {
            /* We failed to allocate the metadata, give it another try or at least
             * attempt to fail cleanly. */
            gc_run(gc);
            alloc = gc_allocation_map_put(gc->allocs, ptr, alloc_size, dtor);
            if (alloc) {
                ptr = alloc->ptr;
            } else {
                free(ptr);
                ptr = NULL;
            }
        }
    }
    return ptr;
}
void* gc_malloc(GarbageCollector* gc, size_t size)
{
    return gc_malloc_ext(gc, size, NULL);
}


void* gc_malloc_ext(GarbageCollector* gc, size_t size, void(*dtor)(void*))
{
    return gc_allocate(gc, 0, size, dtor);
}


void* gc_calloc(GarbageCollector* gc, size_t count, size_t size)
{
    return gc_calloc_ext(gc, count, size, NULL);
}


void* gc_calloc_ext(GarbageCollector* gc, size_t count, size_t size,
                    void(*dtor)(void*))
{
    return gc_allocate(gc, count, size, dtor);
}


void* gc_realloc(GarbageCollector* gc, void* p, size_t size)
{
    Allocation* alloc = gc_allocation_map_get(gc->allocs, p);
    if (p && !alloc) {
        // the user passed an unknown pointer
        errno = EINVAL;
        return NULL;
    }
    void* q = realloc(p, size);
    if (!q) {
        // realloc failed but p is still valid
        return NULL;
    }
    if (!p) {
        // allocation, not reallocation
        Allocation* alloc = gc_allocation_map_put(gc->allocs, q, size, NULL);
        return alloc->ptr;
    }
    if (p == q) {
        // successful reallocation w/o copy
        alloc->size = size;
    } else {
        // successful reallocation w/ copy
        gc_allocation_map_remove(gc->allocs, p);
        gc_allocation_map_put(gc->allocs, p, size, alloc->dtor);
    }
    return q;
}

void gc_free(GarbageCollector* gc, void* ptr)
{
    Allocation* alloc = gc_allocation_map_get(gc->allocs, ptr);
    if (alloc) {
        if (alloc->dtor) {
            alloc->dtor(ptr);
        }
        free(ptr);
        gc_allocation_map_remove(gc->allocs, ptr);
    } else {
        LOG_WARNING("Ignoring request to free unknown pointer %p", (void*) ptr);
    }
}

void gc_start(GarbageCollector* gc, void* bos)
{
    gc_start_ext(gc, bos, 1024, 1024, 0.2, 0.8, 0.5);
}

void gc_start_ext(GarbageCollector* gc,
                  void* bos,
                  size_t initial_capacity,
                  size_t min_capacity,
                  double downsize_load_factor,
                  double upsize_load_factor,
                  double sweep_factor)
{
    double downsize_limit = downsize_load_factor > 0.0 ? downsize_load_factor : 0.2;
    double upsize_limit = upsize_load_factor > 0.0 ? upsize_load_factor : 0.8;
    sweep_factor = sweep_factor > 0.0 ? sweep_factor : 0.5;
    gc->paused = false;
    gc->bos = bos;
    initial_capacity = initial_capacity < min_capacity ? min_capacity : initial_capacity;
    gc->allocs = gc_allocation_map_new(min_capacity, initial_capacity,
                                       sweep_factor, downsize_limit, upsize_limit);
    LOG_DEBUG("Created new garbage collector (cap=%ld, siz=%ld).", gc->allocs->capacity,
              gc->allocs->size);
}

void gc_stop(GarbageCollector* gc)
{
    gc_run(gc);
    gc_allocation_map_delete(gc->allocs);
    return;
}

void gc_pause(GarbageCollector* gc)
{
    gc->paused = true;
}

void gc_resume(GarbageCollector* gc)
{
    gc->paused = false;
}

void gc_mark_alloc(GarbageCollector* gc, void* ptr)
{
    Allocation* alloc = gc_allocation_map_get(gc->allocs, ptr);
    /* Mark if alloc exists and is not tagged already, otherwise skip */
    if (alloc && !(alloc->tag & GC_TAG_MARK)) {
        LOG_DEBUG("Marking allocation (ptr=%p)", ptr);
        alloc->tag |= GC_TAG_MARK;
        /* Iterate over allocation contents and mark them as well */
        LOG_DEBUG("Checking allocation (ptr=%p, size=%lu) contents", ptr, alloc->size);
        for (char* p = (char*) alloc->ptr;
                p < (char*) alloc->ptr + alloc->size;
                ++p) {
            LOG_DEBUG("Checking allocation (ptr=%p) @%lu with value %p",
                      ptr, p-((char*) alloc->ptr), *(void**)p);
            gc_mark_alloc(gc, *(void**)p);
        }
    }
}

void gc_mark_stack(GarbageCollector* gc)
{
    LOG_DEBUG("Marking the stack (gc@%p) in increments of %ld", (void*) gc, sizeof(char));
    char dummy;
    void *tos = (void*) &dummy;
    void *bos = gc->bos;
    if (tos > bos) {
        void* tmp = tos;
        tos = gc->bos;
        bos = tmp;
    }
    for (char* p = (char*) tos; p < (char*) bos; ++p) {
        // LOG_DEBUG("Checking stack location %p with value %p", (void*) p, *(void**)p);
        gc_mark_alloc(gc, *(void**)p);
    }
}

void gc_mark_roots(GarbageCollector* gc)
{
    LOG_DEBUG("Marking roots%s", "");
    for (size_t i = 0; i < gc->allocs->capacity; ++i) {
        Allocation* chunk = gc->allocs->allocs[i];
        while (chunk) {
            if (chunk->tag & GC_TAG_ROOT) {
                LOG_DEBUG("Marking root @ %p", chunk->ptr);
                gc_mark_alloc(gc, chunk->ptr);
            }
            chunk = chunk->next;
        }
    }
}

void gc_mark(GarbageCollector* gc)
{
    /* Note: We only look at the stack and the heap, and ignore BSS. */
    LOG_DEBUG("Initiating GC mark (gc@%p)", (void*) gc);
    /* Scan the heap for roots */
    gc_mark_roots(gc);
    /* Dump registers onto stack and scan the stack */
    void (*volatile _mark_stack)(GarbageCollector*) = gc_mark_stack;
    jmp_buf ctx;
    memset(&ctx, 0, sizeof(jmp_buf));
    setjmp(ctx);
    _mark_stack(gc);
}

size_t gc_sweep(GarbageCollector* gc)
{
    LOG_DEBUG("Initiating GC sweep (gc@%p)", (void*) gc);
    size_t total = 0;
    for (size_t i = 0; i < gc->allocs->capacity; ++i) {
        Allocation* chunk = gc->allocs->allocs[i];
        /* Iterate over separate chaining */
        while (chunk) {
            if (chunk->tag & GC_TAG_MARK) {
                LOG_DEBUG("Found used allocation %p (ptr=%p)", (void*) chunk, (void*) chunk->ptr);
                /* unmark */
                chunk->tag &= ~GC_TAG_MARK;
            } else {
                LOG_DEBUG("Found unused allocation %p (ptr=%p)", (void*) chunk, (void*) chunk->ptr);
                /* no reference to this chunk, hence delete it */
                //total += 1;
                total += chunk->size;
                if (chunk->dtor) {
                    chunk->dtor(chunk->ptr);
                }
                free(chunk->ptr);
                /* and remove it from the bookkeeping */
                gc_allocation_map_remove(gc->allocs, chunk->ptr);
            }
            chunk = chunk->next;
        }
    }
    return total;
}

size_t gc_run(GarbageCollector* gc)
{
    LOG_DEBUG("Initiating GC run (gc@%p)", (void*) gc);
    gc_mark(gc);
    return gc_sweep(gc);
}

char* gc_strdup (GarbageCollector* gc, const char* s)
{
    size_t len = strlen(s) + 1;
    void *new = gc_malloc(gc, len);

    if (new == NULL) {
        return NULL;
    }
    return (char*) memcpy(new, s, len);
}
