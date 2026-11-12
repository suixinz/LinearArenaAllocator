#include "arena.h"

#include <stddef.h>     //size_t
#include <stdint.h>     //SIZE_MAX
#include <stdbool.h>    //bool
#include <stdlib.h>     //malloc()
#include <string.h>     //memset()

/**
 * @brief Control structure for a single allocated memory block.
 */
typedef struct block{

    unsigned char* base; /**< Base address of the memory chunk. */
    size_t offset;       /**< Current bump offset (bytes used). */
    size_t capacity;     /**< Total capacity of this block in bytes. */

}block;

/**
 * @brief The main arena structure.
 *
 * Uses a dynamic array (similar to C++ std::vector) of `block` structures
 * to track memory chunks.
 */
struct arena{

    block* bk_first; /**< Pointer to the start of the block array. */
    block* bk_last;  /**< Pointer past the last initialized block. */
    block* bk_end;   /**< Pointer past the allocated capacity of the array. */

};


arena* arena_create(void){

    arena* arena_pool = (arena*)malloc(1 * sizeof(arena));

    if(arena_pool == NULL){
        return NULL;
    }

    arena_pool->bk_first = NULL;
    arena_pool->bk_last  = NULL;
    arena_pool->bk_end   = NULL;

    return arena_pool;

}

/**
 * @brief Aligns a given offset up to the nearest `ARENA_ALIGNMENT` boundary.
 *
 * @param offset The offset to align.
 * @return The aligned offset.
 * @warning The caller must ensure `offset` does not overflow.
 */
static inline size_t offset_align_up(size_t offset){

    return (offset + ARENA_ALIGNMENT - 1) & (~(ARENA_ALIGNMENT - 1));

}

/**
 * @brief Attempts to allocate `n` bytes by bumping the offset of the current block.
 *
 * Fast path allocation. Checks for alignment, capacity, and overflow.
 *
 * @param arena_pool The arena instance.
 * @param n          Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL if the current block lacks
 *         sufficient space or arguments are invalid.
 */
static void* arena_bump(arena* arena_pool, size_t n){

    if(arena_pool == NULL || arena_pool->bk_first == NULL || n == 0){
        return NULL;
    }

    if(arena_pool->bk_last == arena_pool->bk_first){
        return NULL;
    }

    block* bk_use = arena_pool->bk_last - 1;

    if(bk_use->offset > SIZE_MAX - (ARENA_ALIGNMENT - 1)){
        return NULL;
    }

    size_t aligned = offset_align_up(bk_use->offset);

    if(aligned > bk_use->capacity){
        return NULL;
    }

    if(n > bk_use->capacity - aligned){
        return NULL;
    }

    void* ptr = bk_use->base + aligned;

    bk_use->offset = aligned + n;

    return ptr;                                                 

}

/**
 * @brief Initializes a block by acquiring memory from the system.
 *
 * The block's capacity grows as a power of two starting from
 * `ARENA_BLOCK_DEFAULT_CAPACITY` until it is large enough to satisfy `n`.
 *
 * @param bk The block to initialize. Must be in an uninitialized state (base == NULL).
 * @param n  The minimum requested capacity.
 * @return true on success, false on invalid arguments or allocation failure.
 */
static bool block_init(block* bk, size_t n){

    if(bk == NULL || bk->base != NULL || n == 0){
        return false;
    }

    size_t capacity = ARENA_BLOCK_DEFAULT_CAPACITY;

    while(capacity < n){
        if(capacity > SIZE_MAX / 2){
            return false;
        }
        capacity <<= 1;
    }

    bk->base = (unsigned char*)malloc(capacity);

    if(bk->base == NULL){
        return false;
    }

    bk->capacity = capacity;
    bk->offset   = 0;

    return true;

}

/**
 * @brief Initializes the block pointed to by `bk_last` and advances the cursor.
 *
 * @param arena_pool The arena instance.
 * @param n          The minimum requested capacity for the new block.
 * @return true on success, false if the slot is already initialized, the
 *         array is full, or underlying allocation fails.
 */
static bool arena_use_block_init(arena* arena_pool, size_t n){

    if(arena_pool == NULL || arena_pool->bk_first == NULL){
        return false;
    }

    if(arena_pool->bk_last == arena_pool->bk_end || arena_pool->bk_last->base != NULL){
        return false;
    }

    if(block_init(arena_pool->bk_last, n)){
        arena_pool->bk_last++;
        return true;
    }

    return false;

}

/**
 * @brief Allocates the initial dynamic array to store block control structures.
 *
 * @param arena_pool The arena instance.
 * @return true on success, false if already initialized or allocation fails.
 */
static bool arena_blockarray_init(arena* arena_pool){

    if(arena_pool == NULL){
        return false;
    }

    if(arena_pool->bk_first != NULL){
        return false;
    }

    arena_pool->bk_first = (block*)calloc(ARENA_BLOCKARRAY_DEFAULT_CAPACITY, sizeof(block));

    if(arena_pool->bk_first == NULL){
        return false;
    }

    arena_pool->bk_last = arena_pool->bk_first;
    arena_pool->bk_end  = arena_pool->bk_first + ARENA_BLOCKARRAY_DEFAULT_CAPACITY;

    return true;

}

/**
 * @brief Expands the dynamic array of blocks when the cursor reaches capacity.
 *
 * Uses realloc to multiply the array capacity by `ARENA_BLOCKARRAY_EXPAND_FACTOR`.
 * Newly added slots are explicitly zeroed.
 *
 * @param arena_pool The arena instance.
 * @return true on success, false if the array is not full, overflows, or realloc fails.
 */
static bool arena_blockarray_expand(arena* arena_pool){

    if(arena_pool == NULL || arena_pool->bk_first == NULL || arena_pool->bk_last != arena_pool->bk_end){
        return false;
    }

    size_t old_block_count = arena_pool->bk_end - arena_pool->bk_first;

    size_t old_size = old_block_count * sizeof(block);

    if(old_size > SIZE_MAX / ARENA_BLOCKARRAY_EXPAND_FACTOR){
        return false;
    }

    size_t new_block_count = old_block_count * ARENA_BLOCKARRAY_EXPAND_FACTOR;

    size_t new_size = old_size * ARENA_BLOCKARRAY_EXPAND_FACTOR;

    block* tmp = (block*)realloc(arena_pool->bk_first, new_size);

    if(tmp == NULL){
        return false;
    }

    memset(tmp + old_block_count, 0, (new_block_count - old_block_count) * sizeof(block));

    arena_pool->bk_first = tmp;
    arena_pool->bk_last  = tmp + old_block_count;
    arena_pool->bk_end   = tmp + old_block_count * ARENA_BLOCKARRAY_EXPAND_FACTOR;

    return true;

}

/**
 * @brief Scans the array for a previously used block that has enough capacity.
 *
 * This is primarily used after arena_clear() to reuse already allocated memory.
 * It advances `bk_last` past the chosen block so that `arena_bump` can use it.
 *
 * @param arena_pool The arena instance.
 * @param n          The requested minimum capacity.
 * @return true if a valid block is found and cursor is moved; false otherwise.
 */
static bool arena_use_block_move(arena* arena_pool, size_t n){

    if(arena_pool == NULL || arena_pool->bk_first == NULL){
        return false;
    }

    while(arena_pool->bk_last < arena_pool->bk_end)
    {

        if(arena_pool->bk_last->base == NULL){
            break;
        }

        if(arena_pool->bk_last->capacity >= n){
            arena_pool->bk_last += 1;
            return true;
        }

        arena_pool->bk_last++;

    }

    return false;
    
}

/**
 * @brief Slow path for allocation. Finds or creates a suitable block.
 *
 * Tries to reuse a block (via move), expands the control array if necessary,
 * and initializes a new block if no existing block is large enough.
 *
 * @param arena_pool The arena instance.
 * @param n          The requested size.
 * @return true if a block was successfully prepared for `arena_bump`, false otherwise.
 */
static bool arena_grow(arena* arena_pool, size_t n){

    if(arena_pool == NULL || n == 0){
        return false;
    }

    if(arena_pool->bk_first == NULL){
        if(arena_blockarray_init(arena_pool) == false){
            return false;
        }
    }

    if(arena_pool->bk_last != arena_pool->bk_end){
        if(arena_use_block_move(arena_pool, n) == true){
            return true;
        }
    }

    if(arena_pool->bk_last == arena_pool->bk_end){
        if(arena_blockarray_expand(arena_pool) == false){
            return false;
        }
    }

    return arena_use_block_init(arena_pool, n);
    /* if arena_use_block_init(arena_pool, n) fails,
    ** arena_blockarray_init/arena_blockarray_expand can't rollback!! 
    */
}

void* arena_malloc(arena* arena_pool, size_t n){

    if(arena_pool == NULL || n == 0){
        return NULL;
    }

    void* ptr = NULL;

    ptr = arena_bump(arena_pool, n);
    if(ptr != NULL){
        return ptr;
    }

    if(arena_grow(arena_pool, n) == false){
        return NULL;
    }

    return arena_bump(arena_pool, n);

}

void* arena_calloc(arena* arena_pool, size_t count, size_t size){

    if(arena_pool == NULL || count == 0 || size == 0){
        return NULL;
    }

    if(count > SIZE_MAX / size){
        return NULL;
    }

    size_t n = count * size;

    void* ptr = arena_malloc(arena_pool, n);

    if(ptr == NULL){
        return NULL;
    }

    memset(ptr, 0, n);

    return ptr;

}

void arena_clear(arena* arena_pool){

    if(arena_pool == NULL || arena_pool->bk_first == NULL){
        return;
    }

    block* cur_bk = arena_pool->bk_first;

    while(cur_bk < arena_pool->bk_last){

        cur_bk->offset = 0;
        cur_bk++;

    }

    arena_pool->bk_last = arena_pool->bk_first;

}

/**
 * @brief Releases the internal memory of a block, leaving the struct zeroed.
 *
 * @param bk The block to destroy.
 */
static void block_destroy(block* bk){

    if(bk == NULL){
        return;
    }

    free(bk->base);

    bk->base = NULL;
    bk->offset = 0;
    bk->capacity = 0;

} 

//"destroy" means internal resource will be released but object will be retained.
//while "delete" means internal resource and the object itself will both be released together.

void arena_delete(arena* arena_pool){

    if(arena_pool == NULL){
        return;
    }

    if(arena_pool->bk_first == NULL){
        goto cleanup;
    }

    block* cur_bk = arena_pool->bk_first;

    while(cur_bk < arena_pool->bk_end){
        block_destroy(cur_bk);
        cur_bk++;
    }

    free(arena_pool->bk_first);

cleanup:

    free(arena_pool);

}

void arena_get_stats(const arena* arena_pool, arena_stats* out_stats){

    if(arena_pool == NULL || out_stats == NULL){
        return;
    }

    out_stats->total_blocks   = 0;
    out_stats->total_capacity = 0;
    out_stats->used_bytes     = 0;

    if(arena_pool->bk_first == NULL || arena_pool->bk_last == arena_pool->bk_first){
        return;
    }

    /*
     * Iterate up to bk_last.
     * bk_last points past the last initialized block, whereas bk_end
     * points to the physical array capacity boundary.
     */
    block* cur_bk = arena_pool->bk_first;

    while(cur_bk < arena_pool->bk_last){

        out_stats->total_blocks++;
        out_stats->total_capacity += cur_bk->capacity;
        out_stats->used_bytes     += cur_bk->offset;

        cur_bk++;
    }

}