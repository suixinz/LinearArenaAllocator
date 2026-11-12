#pragma once

/**
 * @file arena.h
 * @brief A fast region-based (bump) memory allocator.
 *
 * This allocator acquires large chunks of memory from the system and hands
 * out smaller chunks sequentially. It does not support freeing individual
 * allocations. Instead, all memory is reclaimed at once via arena_clear()
 * or arena_delete().
 *
 * @warning This allocator is NOT thread-safe. Concurrent access from
 *          multiple threads requires external synchronization.
 */

#include <stddef.h>   /* size_t, max_align_t (C11) */

/* ==================== Alignment & Compatibility ==================== */

/*
 * MSVC's C standard library (even in C11/C17 mode) does not define
 * max_align_t, and older versions don't support _Static_assert.
 * We provide a fallback for MSVC C mode.
 */
#if defined(_MSC_VER) && !defined(__cplusplus)

    /* 16-byte alignment is conservative and safe on x86/x64 Windows */
    #define ARENA_ALIGNMENT 16

    /* MSVC C mode fallback for _Static_assert */
    typedef char arena_alignment_must_be_power_of_two[
        (ARENA_ALIGNMENT > 0 &&
         (ARENA_ALIGNMENT & (ARENA_ALIGNMENT - 1)) == 0) ? 1 : -1
    ];

#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L

    /* Standard C11 path */
    #define ARENA_ALIGNMENT _Alignof(max_align_t)

    _Static_assert(
        ARENA_ALIGNMENT > 0 &&
        (ARENA_ALIGNMENT & (ARENA_ALIGNMENT - 1)) == 0,
        "ARENA_ALIGNMENT must be a power of two"
    );

#else

    /* Fallback for pre-C11 environments (e.g., C99, C89) */
    #define ARENA_ALIGNMENT 16

    typedef char arena_alignment_must_be_power_of_two[
        (ARENA_ALIGNMENT > 0 &&
         (ARENA_ALIGNMENT & (ARENA_ALIGNMENT - 1)) == 0) ? 1 : -1
    ];

#endif


/* ==================== Block Array ==================== */

#ifndef ARENA_BLOCKARRAY_DEFAULT_CAPACITY
    #define ARENA_BLOCKARRAY_DEFAULT_CAPACITY 3
#endif

#ifndef ARENA_BLOCKARRAY_EXPAND_FACTOR
    #define ARENA_BLOCKARRAY_EXPAND_FACTOR 2
#endif

#if ARENA_BLOCKARRAY_DEFAULT_CAPACITY <= 0
    #error "ARENA_BLOCKARRAY_DEFAULT_CAPACITY must be greater than 0"
#endif

#if ARENA_BLOCKARRAY_EXPAND_FACTOR < 2
    #error "ARENA_BLOCKARRAY_EXPAND_FACTOR must be >= 2"
#endif


/* ==================== Block ==================== */

#ifndef ARENA_BLOCK_DEFAULT_CAPACITY
    #define ARENA_BLOCK_DEFAULT_CAPACITY (1024 * 1024)
#endif

#if ARENA_BLOCK_DEFAULT_CAPACITY <= 0
    #error "ARENA_BLOCK_DEFAULT_CAPACITY must be greater than 0"
#endif


/* ==================== Public API ==================== */

/**
 * @brief Opaque handle to an arena allocator instance.
 */
typedef struct arena arena;

/**
 * @brief Statistics about the arena's memory usage.
 */
typedef struct arena_stats{

    size_t total_blocks;   /**< Number of initialized memory blocks. */
    size_t total_capacity; /**< Total bytes acquired from the system. */
    size_t used_bytes;     /**< Total bytes allocated to the caller. */

}arena_stats;


/**
 * @brief Creates a new arena allocator instance.
 *
 * The returned arena contains no allocated blocks initially. Memory blocks
 * are acquired lazily upon the first allocation request.
 *
 * @return Pointer to the newly created arena, or NULL if memory allocation
 *         for the arena structure itself fails.
 */
arena* arena_create(void);

/**
 * @brief Allocates `n` bytes from the arena.
 *
 * The returned memory is aligned to `ARENA_ALIGNMENT` and is NOT
 * zero-initialized.
 *
 * @note The allocated memory remains valid until arena_clear() or
 *       arena_delete() is called on the arena. Individual blocks cannot
 *       be freed.
 *
 * @param arena_pool The arena to allocate from. Must not be NULL.
 * @param n          The number of bytes to allocate. Must be greater than 0.
 *
 * @return Pointer to the allocated memory, or NULL if `arena_pool` is NULL,
 *         `n` is 0, or the system cannot provide more memory.
 */
void* arena_malloc(arena* arena_pool, size_t n);

/**
 * @brief Allocates memory for an array of `count` elements of `size` bytes
 *        each, and zero-initializes the allocated memory.
 *
 * This function checks for multiplication overflow before delegating to
 * arena_malloc().
 *
 * @param arena_pool The arena to allocate from. Must not be NULL.
 * @param count      Number of elements. Must be greater than 0.
 * @param size       Size of each element in bytes. Must be greater than 0.
 *
 * @return Pointer to the zeroed allocated memory, or NULL if arguments are
 *         invalid, multiplication overflows, or the allocation fails.
 */
void* arena_calloc(arena* arena_pool, size_t count, size_t size);

/**
 * @brief Resets the arena, making all previously allocated memory reusable.
 *
 * This function does NOT return memory to the operating system. Instead, it
 * resets internal offsets, allowing subsequent allocations to reuse the
 * already acquired blocks.
 *
 * @warning All pointers previously returned by arena_malloc() or
 *          arena_calloc() are invalidated and must not be accessed
 *          (read or written) after calling this function.
 *
 * @param arena_pool The arena to clear. If NULL, this function is a no-op.
 */
void arena_clear(arena* arena_pool);

/**
 * @brief Destroys the arena and releases all memory back to the system.
 *
 * This function frees both the internal memory blocks and the arena
 * structure itself.
 *
 * @warning After calling this function, the arena pointer is invalid. It
 *          must not be used in any subsequent calls.
 *
 * @param arena_pool The arena to destroy. If NULL, this function is a no-op.
 */
void arena_delete(arena* arena_pool);

/**
 * @brief Retrieves current memory statistics of the arena.
 *
 * This function is useful for monitoring memory consumption, debugging,
 * and verifying that memory reuse (after arena_clear) is working correctly.
 * Note: This is an O(N) operation where N is the number of initialized blocks.
 *
 * @param arena_pool The arena instance. Must not be NULL.
 * @param out_stats  Pointer to a structure where statistics will be written.
 *                   Must not be NULL.
 */
void arena_get_stats(const arena* arena_pool, arena_stats* out_stats);