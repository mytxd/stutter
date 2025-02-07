/*
 * map.c
 * Copyright (C) 2019 Marc Kirchner
 *
 * Distributed under terms of the MIT license.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "djb2.h"
#include "gc.h"
#include "log.h"
#include "map.h"
#include "primes.h"

static double load_factor(Map* ht)
{
    LOG_DEBUG("Load factor: %.2f", (double) ht->size / (double) ht->capacity);
    return (double) ht->size / (double) ht->capacity;
}

static MapItem* map_item_new(char* key, void* value, size_t siz)
{
    MapItem* item = (MapItem*) gc_malloc(&gc, sizeof(MapItem));
    item->key = gc_strdup(&gc, key);
    item->size = siz;
    item->value = gc_malloc(&gc, siz);
    memcpy(item->value, value, siz);
    item->next = NULL;
    return item;
}

static void map_item_delete(MapItem* item)
{
    if (item) {
        gc_free(&gc, item->key);
        gc_free(&gc, item->value);
        gc_free(&gc, item);
    }
}

Map* map_new(size_t capacity)
{
    Map* ht = (Map*) gc_malloc(&gc, sizeof(Map));
    ht->capacity = next_prime(capacity);
    ht->size = 0;
    ht->items = gc_calloc(&gc, ht->capacity, sizeof(MapItem*));
    return ht;
}

void map_delete(Map* ht)
{
    MapItem* item, *tmp;
    for (size_t i=0; i < ht->capacity; ++i) {
        if ((item = ht->items[i]) != NULL) {
            while (item) {
                tmp = item;
                item = item->next;
                map_item_delete(tmp);
            }
        }
    }
    gc_free(&gc, ht->items);
    gc_free(&gc, ht);
}

unsigned long map_index(Map* map, char* key)
{
    return djb2(key) % map->capacity;
}

void map_put(Map* ht, char* key, void* value, size_t siz)
{
    // hash
    unsigned long index = map_index(ht, key);
    LOG_DEBUG("index: %lu", index);
    // create item
    MapItem* item = map_item_new(key, value, siz);
    MapItem* cur = ht->items[index];
    // update if exists
    MapItem* prev = NULL;
    while(cur != NULL) {
        if (strcmp(cur->key, key) == 0) {
            // found it
            item->next = cur->next;
            if (!prev) {
                // position 0
                ht->items[index] = item;
            } else {
                // in the list
                prev->next = item;
            }
            map_item_delete(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    // insert (at front of list)
    cur = ht->items[index];
    item->next = cur;
    ht->items[index] = item;
    ht->size++;
    if (load_factor(ht) > 0.7)
        map_resize(ht, next_prime(ht->capacity*2));
}

void* map_get(Map* ht, char* key)
{
    unsigned long index = map_index(ht, key);
    LOG_DEBUG("index: %lu", index);
    MapItem* cur = ht->items[index];
    LOG_DEBUG("ptr: %p", (void *)cur);
    while(cur != NULL) {
        if (strcmp(cur->key, key) == 0) {
            return cur->value;
        }
        cur = cur->next;
    }
    return NULL;
}

void map_remove(Map* ht, char* key)
{
    // ignores unknown keys
    unsigned long index = map_index(ht, key);
    MapItem* cur = ht->items[index];
    MapItem* prev = NULL;
    MapItem* tmp = NULL;
    while(cur != NULL) {
        // Separate chaining w/ linked lists
        if (strcmp(cur->key, key) == 0) {
            LOG_DEBUG("Removing map item at index %lu, pos %lu.", index, cur - ht->items[index]);
            // found it
            if (!prev) {
                // first item in list
                ht->items[index] = cur->next;
            } else {
                // not the first item in the list
                prev->next = cur->next;
            }
            tmp = cur;
            cur = cur->next;
            map_item_delete(tmp);
            ht->size--;
        } else {
            // move on
            LOG_DEBUG("Skipping map item at index %lu, pos %lu.", index, cur - ht->items[index]);
            prev = cur;
            cur = cur->next;
        }
    }
    if (load_factor(ht) < 0.1)
        map_resize(ht, next_prime(ht->capacity/2));
}

void map_resize(Map* ht, size_t new_capacity)
{
    // Replaces the existing items array in the hash table
    // with a resized one and pushes items into the new, correct buckets
    LOG_DEBUG("Resizing to %lu", new_capacity);
    MapItem** resized_items = gc_calloc(&gc, new_capacity, sizeof(MapItem*));

    for (size_t i=0; i<ht->capacity; ++i) {
        MapItem* item = ht->items[i];
        while(item) {
            MapItem* next_item = item->next;
            unsigned long new_index = djb2(item->key) % new_capacity;
            item->next = resized_items[new_index];
            resized_items[new_index] = item;
            item = next_item;
        }
    }
    gc_free(&gc, ht->items);
    ht->capacity = new_capacity;
    ht->items = resized_items;
}
