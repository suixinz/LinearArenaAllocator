#include "arena.c"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

enum {
    DIRECT_BLOCK_CAP = 64,
    DIRECT_BLOCK_COUNT = 4,
    RANDOM_SLOT_COUNT = 4096,
    RANDOM_OPERATION_COUNT = 50000,
    RANDOM_MAX_ALLOCATION = 8192,
    RANDOM_MAX_LIVE_RECORDS = 4096,
    REPEATED_ROUND_COUNT = 1000,
    BLOCK_GROWTH_COUNT = 8,
    CLEAR_REUSE_COUNT = 5
};

typedef struct allocation_record {
    unsigned char* ptr;
    size_t size;
    unsigned char value;
    bool active;
} allocation_record;

static size_t tests_run = 0;
static size_t tests_failed = 0;

static void check_result(bool condition, const char* expression,
                         const char* file, int line)
{
    tests_run++;

    if (condition == false) {
        tests_failed++;
        fprintf(stderr, "FAIL: %s (%s:%d)\n", expression, file, line);
    }
}

static void check_memory(const unsigned char* ptr, size_t size,
                         unsigned char value, const char* expression,
                         const char* file, int line)
{
    size_t i = 0;

    tests_run++;

    while (i < size) {
        if (ptr[i] != value) {
            tests_failed++;
            fprintf(stderr,
                    "FAIL: %s: byte %zu is 0x%02x, expected 0x%02x (%s:%d)\n",
                    expression, i, (unsigned int)ptr[i],
                    (unsigned int)value, file, line);
            return;
        }
        i++;
    }
}

static void check_zero_memory(const unsigned char* ptr, size_t size,
                              const char* expression,
                              const char* file, int line)
{
    size_t i = 0;

    tests_run++;

    while (i < size) {
        if (ptr[i] != 0) {
            tests_failed++;
            fprintf(stderr,
                    "FAIL: %s: byte %zu is 0x%02x, expected 0 (%s:%d)\n",
                    expression, i, (unsigned int)ptr[i], file, line);
            return;
        }
        i++;
    }
}

static uint32_t random_next(uint32_t* state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;

    *state = value;
    return value;
}

static size_t random_range(uint32_t* state, size_t limit)
{
    if (limit == 0) {
        return 0;
    }

    return (size_t)(random_next(state) % (uint32_t)limit);
}

/* ================================================================ */
/* White-box: offset_align_up                                       */
/* ================================================================ */

static void test_offset_align_up_direct(void)
{
    size_t a = (size_t)ARENA_ALIGNMENT;

    check_result(offset_align_up(0) == 0,
                 "align_up(0) == 0", __FILE__, __LINE__);
    check_result(offset_align_up(1) == a,
                 "align_up(1) == ARENA_ALIGNMENT", __FILE__, __LINE__);
    check_result(offset_align_up(a - 1) == a,
                 "align_up(a-1) == a", __FILE__, __LINE__);
    check_result(offset_align_up(a) == a,
                 "align_up(a) == a", __FILE__, __LINE__);
    check_result(offset_align_up(a + 1) == a * 2,
                 "align_up(a+1) == 2a", __FILE__, __LINE__);
    check_result(offset_align_up(a * 3 - 1) == a * 3,
                 "align_up(3a-1) == 3a", __FILE__, __LINE__);
    check_result(offset_align_up(a * 3) == a * 3,
                 "align_up(3a) == 3a", __FILE__, __LINE__);
}

/* ================================================================ */
/* White-box: block_init                                            */
/* ================================================================ */

static void test_block_init_direct(void)
{
    block bk;

    memset(&bk, 0, sizeof(bk));

    check_result(block_init(&bk, 0) == false,
                 "block_init(n=0) fails", __FILE__, __LINE__);
    check_result(block_init(NULL, 8) == false,
                 "block_init(NULL) fails", __FILE__, __LINE__);

    check_result(block_init(&bk, 1) == true,
                 "block_init(1) succeeds", __FILE__, __LINE__);
    check_result(bk.offset == 0,
                 "block_init sets offset 0", __FILE__, __LINE__);
    check_result(bk.capacity >= 1,
                 "block_init capacity covers request", __FILE__, __LINE__);
    check_result(bk.capacity == (size_t)ARENA_BLOCK_DEFAULT_CAPACITY,
                 "block_init uses default capacity for small n",
                 __FILE__, __LINE__);

    check_result(block_init(&bk, 1) == false,
                 "block_init on initialized block fails", __FILE__, __LINE__);

    block_destroy(&bk);
    check_result(bk.base == NULL,
                 "block_destroy clears base", __FILE__, __LINE__);
    check_result(bk.capacity == 0,
                 "block_destroy clears capacity", __FILE__, __LINE__);
    check_result(bk.offset == 0,
                 "block_destroy clears offset", __FILE__, __LINE__);
}

static void test_block_init_growth_direct(void)
{
    block bk;
    size_t base = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t request = base + 1;
    size_t expected = base;

    while (expected < request) {
        expected <<= 1;
    }

    memset(&bk, 0, sizeof(bk));

    check_result(block_init(&bk, request) == true,
                 "block_init growth succeeds", __FILE__, __LINE__);
    check_result(bk.capacity == expected,
                 "block_init capacity is next power of two",
                 __FILE__, __LINE__);

    block_destroy(&bk);
}

static void test_block_init_overflow_direct(void)
{
    block bk;

    memset(&bk, 0, sizeof(bk));

    check_result(block_init(&bk, SIZE_MAX) == false,
                 "block_init(SIZE_MAX) fails", __FILE__, __LINE__);
    check_result(bk.base == NULL,
                 "failed block_init leaves base NULL", __FILE__, __LINE__);

    check_result(block_init(&bk, SIZE_MAX / 2 + 1) == false,
                 "block_init(> SIZE_MAX/2) fails", __FILE__, __LINE__);
    check_result(bk.base == NULL,
                 "failed overflow block_init leaves base NULL",
                 __FILE__, __LINE__);
}

/* ================================================================ */
/* White-box: arena_bump                                            */
/* ================================================================ */

static void test_arena_bump_direct(void)
{
    arena pool;
    block bk;
    unsigned char* buf = (unsigned char*)malloc(DIRECT_BLOCK_CAP);
    void* p1;
    void* p2;

    check_result(buf != NULL, "direct bump buffer allocated",
                 __FILE__, __LINE__);
    if (buf == NULL) {
        return;
    }

    memset(&pool, 0, sizeof(pool));
    memset(&bk, 0, sizeof(bk));

    bk.base = buf;
    bk.capacity = DIRECT_BLOCK_CAP;
    bk.offset = 0;

    pool.bk_first = &bk;
    pool.bk_last = &bk + 1;
    pool.bk_end = &bk + 1;

    check_result(arena_bump(NULL, 1) == NULL,
                 "bump(NULL) returns NULL", __FILE__, __LINE__);
    check_result(arena_bump(&pool, 0) == NULL,
                 "bump(n=0) returns NULL", __FILE__, __LINE__);

    pool.bk_first = NULL;
    check_result(arena_bump(&pool, 1) == NULL,
                 "bump(bk_first=NULL) returns NULL", __FILE__, __LINE__);
    pool.bk_first = &bk;

    pool.bk_last = &bk;
    check_result(arena_bump(&pool, 1) == NULL,
                 "bump(empty arena) returns NULL", __FILE__, __LINE__);
    pool.bk_last = &bk + 1;

    p1 = arena_bump(&pool, 1);
    check_result(p1 == buf,
                 "bump returns base at offset 0", __FILE__, __LINE__);
    check_result(bk.offset == 1,
                 "bump advances offset by n", __FILE__, __LINE__);

    p2 = arena_bump(&pool, 1);
    check_result(p2 == buf + (size_t)ARENA_ALIGNMENT,
                 "bump aligns second allocation", __FILE__, __LINE__);
    check_result(bk.offset == (size_t)ARENA_ALIGNMENT + 1,
                 "bump advances aligned offset", __FILE__, __LINE__);

    check_result(arena_bump(&pool, DIRECT_BLOCK_CAP) == NULL,
                 "bump rejects oversize", __FILE__, __LINE__);
    check_result(bk.offset == (size_t)ARENA_ALIGNMENT + 1,
                 "failed bump leaves offset unchanged", __FILE__, __LINE__);
    {
        size_t aligned = offset_align_up(bk.offset);
        size_t remaining = DIRECT_BLOCK_CAP - aligned;
        void* p3 = arena_bump(&pool, remaining);
        check_result(p3 == buf + aligned,
                     "bump exact-fit succeeds", __FILE__, __LINE__);
        check_result(bk.offset == DIRECT_BLOCK_CAP,
                     "bump exact-fit fills capacity", __FILE__, __LINE__);
    }

    check_result(arena_bump(&pool, 1) == NULL,
                 "bump fails when full", __FILE__, __LINE__);
    free(buf);
}

static void test_arena_bump_alignment_boundary_direct(void)
{
    arena pool;
    block bk;
    unsigned char* buf = (unsigned char*)malloc(DIRECT_BLOCK_CAP);
    void* p;

    check_result(buf != NULL, "boundary buffer allocated",
                 __FILE__, __LINE__);
    if (buf == NULL) {
        return;
    }

    memset(&pool, 0, sizeof(pool));
    memset(&bk, 0, sizeof(bk));

    bk.base = buf;
    bk.capacity = DIRECT_BLOCK_CAP;
    bk.offset = 0;

    pool.bk_first = &bk;
    pool.bk_last = &bk + 1;
    pool.bk_end = &bk + 1;

    /* Occupy 1 byte to make the offset unaligned */
    check_result(arena_bump(&pool, 1) == buf,
                 "first byte at base", __FILE__, __LINE__);

    /* Request DIRECT_BLOCK_CAP - 1 bytes: after alignment, the offset is a,
       remaining is DIRECT_BLOCK_CAP - a, which is not enough -> fail */
    check_result(arena_bump(&pool, DIRECT_BLOCK_CAP - 1) == NULL,
                 "bump fails when aligned request exceeds remaining",
                 __FILE__, __LINE__);

    /* Request exactly the remaining capacity: fills up perfectly after alignment */
    p = arena_bump(&pool, DIRECT_BLOCK_CAP - (size_t)ARENA_ALIGNMENT);
    check_result(p == buf + (size_t)ARENA_ALIGNMENT,
                 "bump exact-fit after alignment succeeds",
                 __FILE__, __LINE__);
    check_result(bk.offset == DIRECT_BLOCK_CAP,
                 "exact-fit reaches capacity", __FILE__, __LINE__);

    free(buf);
}

static void test_arena_bump_capacity_not_multiple_direct(void)
{
    arena pool;
    block bk;
    unsigned char* buf = (unsigned char*)malloc(DIRECT_BLOCK_CAP + 1);
    void* p;

    check_result(buf != NULL, "odd capacity buffer allocated",
                 __FILE__, __LINE__);
    if (buf == NULL) {
        return;
    }

    memset(&pool, 0, sizeof(pool));
    memset(&bk, 0, sizeof(bk));

    bk.base = buf;
    bk.capacity = DIRECT_BLOCK_CAP + 1;
    bk.offset = 0;

    pool.bk_first = &bk;
    pool.bk_last = &bk + 1;
    pool.bk_end = &bk + 1;

    p = arena_bump(&pool, 1);
    check_result(p == buf, "odd capacity bump succeeds",
                 __FILE__, __LINE__);

    p = arena_bump(&pool, DIRECT_BLOCK_CAP);
    check_result(p == NULL,
                 "bump rejects when aligned + n > odd capacity",
                 __FILE__, __LINE__);

    free(buf);
}

static void test_arena_bump_offset_overflow_direct(void)
{
    arena pool;
    block bk;
    unsigned char* buf = (unsigned char*)malloc(16);
    void* p;

    check_result(buf != NULL, "overflow guard buffer allocated",
                 __FILE__, __LINE__);
    if (buf == NULL) {
        return;
    }

    memset(&pool, 0, sizeof(pool));
    memset(&bk, 0, sizeof(bk));

    bk.base = buf;
    bk.capacity = 16;
    bk.offset = SIZE_MAX - ((size_t)ARENA_ALIGNMENT - 2);

    pool.bk_first = &bk;
    pool.bk_last = &bk + 1;
    pool.bk_end = &bk + 1;

    p = arena_bump(&pool, 1);
    check_result(p == NULL,
                 "bump guards offset overflow", __FILE__, __LINE__);
    check_result(bk.offset == SIZE_MAX - ((size_t)ARENA_ALIGNMENT - 2),
                 "overflow guard leaves offset unchanged",
                 __FILE__, __LINE__);

    free(buf);
}

/* ================================================================ */
/* White-box: arena_use_block_init                                  */
/* ================================================================ */

static void test_arena_use_block_init_direct(void)
{
    arena pool;
    block bks[DIRECT_BLOCK_COUNT];

    memset(&pool, 0, sizeof(pool));
    memset(bks, 0, sizeof(bks));

    check_result(arena_use_block_init(NULL, 8) == false,
                 "use_block_init(NULL) fails", __FILE__, __LINE__);

    pool.bk_first = bks;
    pool.bk_last = bks;
    pool.bk_end = bks + DIRECT_BLOCK_COUNT;

    check_result(arena_use_block_init(&pool, 8) == true,
                 "use_block_init succeeds on fresh slot",
                 __FILE__, __LINE__);
    check_result(pool.bk_last == bks + 1,
                 "use_block_init advances bk_last", __FILE__, __LINE__);
    check_result(bks[0].base != NULL,
                 "use_block_init fills base", __FILE__, __LINE__);

    pool.bk_last = bks + DIRECT_BLOCK_COUNT;
    check_result(arena_use_block_init(&pool, 8) == false,
                 "use_block_init fails at bk_end", __FILE__, __LINE__);

    pool.bk_last = bks;
    check_result(arena_use_block_init(&pool, 8) == false,
                 "use_block_init fails on initialized slot",
                 __FILE__, __LINE__);
    check_result(pool.bk_last == bks,
                 "failed use_block_init leaves cursor", __FILE__, __LINE__);

    block_destroy(&bks[0]);
}

/* ================================================================ */
/* White-box: arena_blockarray_init / expand                        */
/* ================================================================ */

static void test_arena_blockarray_init_direct(void)
{
    arena pool;
    block dummy;

    memset(&pool, 0, sizeof(pool));
    memset(&dummy, 0, sizeof(dummy));

    check_result(arena_blockarray_init(NULL) == false,
                 "blockarray_init(NULL) fails", __FILE__, __LINE__);

    check_result(arena_blockarray_init(&pool) == true,
                 "blockarray_init succeeds on empty arena",
                 __FILE__, __LINE__);
    check_result(pool.bk_first != NULL,
                 "blockarray_init fills bk_first", __FILE__, __LINE__);
    check_result(pool.bk_last == pool.bk_first,
                 "blockarray_init sets bk_last", __FILE__, __LINE__);
    check_result((size_t)(pool.bk_end - pool.bk_first) ==
                     (size_t)ARENA_BLOCKARRAY_DEFAULT_CAPACITY,
                 "blockarray_init sets bk_end", __FILE__, __LINE__);
    check_result(pool.bk_first[0].base == NULL,
                 "blockarray_init zeroes slots", __FILE__, __LINE__);

    check_result(arena_blockarray_init(&pool) == false,
                 "blockarray_init fails if already initialized",
                 __FILE__, __LINE__);

    free(pool.bk_first);
    (void)dummy;
}

static void test_arena_blockarray_expand_direct(void)
{
    arena pool;
    size_t old_count = (size_t)ARENA_BLOCKARRAY_DEFAULT_CAPACITY;
    size_t i;

    memset(&pool, 0, sizeof(pool));

    check_result(arena_blockarray_expand(NULL) == false,
                 "blockarray_expand(NULL) fails", __FILE__, __LINE__);

    pool.bk_first = (block*)calloc(old_count, sizeof(block));
    check_result(pool.bk_first != NULL,
                 "expand test array allocated", __FILE__, __LINE__);
    if (pool.bk_first == NULL) {
        return;
    }

    pool.bk_last = pool.bk_first;
    pool.bk_end = pool.bk_first + old_count;

    check_result(arena_blockarray_expand(&pool) == false,
                 "blockarray_expand fails when not full",
                 __FILE__, __LINE__);

    pool.bk_last = pool.bk_end;
    check_result(arena_blockarray_expand(&pool) == true,
                 "blockarray_expand succeeds when full",
                 __FILE__, __LINE__);
    check_result((size_t)(pool.bk_end - pool.bk_first) ==
                     old_count * (size_t)ARENA_BLOCKARRAY_EXPAND_FACTOR,
                 "blockarray_expand multiplies capacity",
                 __FILE__, __LINE__);
    check_result(pool.bk_last == pool.bk_first + old_count,
                 "blockarray_expand keeps cursor at old end",
                 __FILE__, __LINE__);

    i = old_count;
    while (i < (size_t)(pool.bk_end - pool.bk_first)) {
        check_result(pool.bk_first[i].base == NULL,
                     "expanded slot base zeroed", __FILE__, __LINE__);
        check_result(pool.bk_first[i].capacity == 0,
                     "expanded slot capacity zeroed", __FILE__, __LINE__);
        check_result(pool.bk_first[i].offset == 0,
                     "expanded slot offset zeroed", __FILE__, __LINE__);
        i++;
    }

    free(pool.bk_first);
}

/* ================================================================ */
/* White-box: arena_use_block_move                                  */
/* ================================================================ */

static void test_arena_use_block_move_direct(void)
{
    arena pool;
    block bks[DIRECT_BLOCK_COUNT];
    unsigned char* b0;
    unsigned char* b1;

    memset(&pool, 0, sizeof(pool));
    memset(bks, 0, sizeof(bks));

    b0 = (unsigned char*)malloc(32);
    b1 = (unsigned char*)malloc(128);

    check_result(b0 != NULL && b1 != NULL,
                 "move test buffers allocated", __FILE__, __LINE__);
    if (b0 == NULL || b1 == NULL) {
        free(b0);
        free(b1);
        return;
    }

    bks[0].base = b0;
    bks[0].capacity = 32;
    bks[0].offset = 0;

    bks[1].base = b1;
    bks[1].capacity = 128;
    bks[1].offset = 0;

    /* bks[2], bks[3] are left with base == NULL */

    pool.bk_first = bks;
    pool.bk_last = bks;
    pool.bk_end = bks + DIRECT_BLOCK_COUNT;

    check_result(arena_use_block_move(NULL, 8) == false,
                 "move(NULL) fails", __FILE__, __LINE__);

    check_result(arena_use_block_move(&pool, 16) == true,
                 "move picks first block that fits", __FILE__, __LINE__);
    check_result(pool.bk_last == bks + 1,
                 "move advances past chosen block", __FILE__, __LINE__);

    pool.bk_last = bks;

    check_result(arena_use_block_move(&pool, 64) == true,
                 "move skips too-small block", __FILE__, __LINE__);
    check_result(pool.bk_last == bks + 2,
                 "move lands after fitting block", __FILE__, __LINE__);

    pool.bk_last = bks;

    check_result(arena_use_block_move(&pool, 256) == false,
                 "move fails when no block fits", __FILE__, __LINE__);
    check_result(pool.bk_last == bks + 2,
                 "move stops at uninitialized slot", __FILE__, __LINE__);

    free(b0);
    free(b1);
}

/* ================================================================ */
/* White-box: arena_grow branches                                   */
/* ================================================================ */

static void test_arena_grow_direct(void)
{
    arena pool;

    memset(&pool, 0, sizeof(pool));

    check_result(arena_grow(NULL, 1) == false,
                 "grow(NULL) fails", __FILE__, __LINE__);
    check_result(arena_grow(&pool, 0) == false,
                 "grow(n=0) fails", __FILE__, __LINE__);

    check_result(arena_grow(&pool, 8) == true,
                 "grow initializes blockarray", __FILE__, __LINE__);
    check_result(pool.bk_first != NULL,
                 "grow creates blockarray", __FILE__, __LINE__);
    check_result(pool.bk_last == pool.bk_first + 1,
                 "grow advances bk_last", __FILE__, __LINE__);
    check_result(pool.bk_first[0].base != NULL,
                 "grow initializes first block", __FILE__, __LINE__);

    /* Clean up the structure created by the first grow */
    {
        block* cur = pool.bk_first;
        while (cur < pool.bk_last) {
            block_destroy(cur);
            cur++;
        }
        free(pool.bk_first);
        memset(&pool, 0, sizeof(pool));
    }

    /* Large allocation after clear: move scans all blocks and fails, grow initializes a new block */
    {
        size_t cap = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
        size_t i;
        block* cur;

        check_result(arena_grow(&pool, cap) == true,
                     "grow first block", __FILE__, __LINE__);
        pool.bk_first[0].offset = cap;

        i = 1;
        while (i < 3) {
            check_result(arena_grow(&pool, cap) == true,
                         "grow subsequent block", __FILE__, __LINE__);
            pool.bk_last[-1].offset = cap;
            i++;
        }

        pool.bk_last = pool.bk_first;
        i = 0;
        while (i < 3) {
            pool.bk_first[i].offset = 0;
            i++;
        }

        check_result(arena_grow(&pool, cap * 2 + 1) == true,
                     "grow after clear with large n succeeds",
                     __FILE__, __LINE__);

        cur = pool.bk_first;
        while (cur < pool.bk_last) {
            block_destroy(cur);
            cur++;
        }
        free(pool.bk_first);
    }
}

/* ================================================================ */
/* Public API: Integration + Boundaries                             */
/* ================================================================ */

static void test_null_and_zero_arguments(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_malloc(NULL, 1) == NULL,
                 "arena_malloc(NULL, 1) == NULL", __FILE__, __LINE__);
    check_result(arena_calloc(NULL, 1, 1) == NULL,
                 "arena_calloc(NULL, 1, 1) == NULL", __FILE__, __LINE__);

    check_result(arena_pool != NULL,
                 "arena_create() returns non-NULL", __FILE__, __LINE__);

    if (arena_pool != NULL) {
        check_result(arena_malloc(arena_pool, 0) == NULL,
                     "arena_malloc(arena, 0) == NULL", __FILE__, __LINE__);
        check_result(arena_calloc(arena_pool, 0, 1) == NULL,
                     "arena_calloc(arena, 0, 1) == NULL", __FILE__, __LINE__);
        check_result(arena_calloc(arena_pool, 1, 0) == NULL,
                     "arena_calloc(arena, 1, 0) == NULL", __FILE__, __LINE__);
        arena_clear(arena_pool);
        arena_delete(arena_pool);
    }

    arena_clear(NULL);
    arena_delete(NULL);
}

static void test_basic_allocation_and_alignment(void)
{
    arena* arena_pool = arena_create();
    size_t sizes[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65 };
    size_t i = 0;

    check_result(arena_pool != NULL, "create for alignment test",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    while (i < sizeof(sizes) / sizeof(sizes[0])) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, sizes[i]);
        check_result(ptr != NULL, "allocation succeeds", __FILE__, __LINE__);
        if (ptr != NULL) {
            check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                         "allocation aligned", __FILE__, __LINE__);
            memset(ptr, (int)(i + 1), sizes[i]);
            check_memory(ptr, sizes[i], (unsigned char)(i + 1),
                         "allocation writable", __FILE__, __LINE__);
        }
        i++;
    }

    arena_delete(arena_pool);
}

static void test_calloc_overflow(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_pool != NULL, "create for calloc overflow",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    check_result(arena_calloc(arena_pool, SIZE_MAX, 2) == NULL,
                 "calloc rejects SIZE_MAX*2", __FILE__, __LINE__);
    check_result(arena_calloc(arena_pool, SIZE_MAX / 2 + 1, 2) == NULL,
                 "calloc rejects overflow", __FILE__, __LINE__);
    check_result(arena_calloc(arena_pool, SIZE_MAX, SIZE_MAX) == NULL,
                 "calloc rejects SIZE_MAX*SIZE_MAX", __FILE__, __LINE__);

    check_result(arena_malloc(arena_pool, 1) != NULL,
                 "arena usable after calloc overflow", __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_block_boundary(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    unsigned char* ptr;
    unsigned char* next;

    check_result(arena_pool != NULL, "create for boundary test",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
    check_result(ptr != NULL, "full capacity allocation succeeds",
                 __FILE__, __LINE__);
    if (ptr != NULL) {
        check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                     "full capacity aligned", __FILE__, __LINE__);
        memset(ptr, 0x6D, capacity);
        check_memory(ptr, capacity, 0x6D, "full capacity writable",
                     __FILE__, __LINE__);
    }

    next = (unsigned char*)arena_malloc(arena_pool, 1);
    check_result(next != NULL, "next allocation grows arena",
                 __FILE__, __LINE__);
    if (next != NULL) {
        *next = 0x3C;
        check_result(*next == 0x3C, "next allocation writable",
                     __FILE__, __LINE__);
    }

    arena_delete(arena_pool);
}

static void test_large_allocation_and_block_growth(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t i = 0;

    check_result(arena_pool != NULL, "create for growth test",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    while (i < (size_t)BLOCK_GROWTH_COUNT) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
        check_result(ptr != NULL, "growth allocation succeeds",
                     __FILE__, __LINE__);
        if (ptr == NULL) {
            break;
        }
        memset(ptr, (int)(i + 1), capacity);
        check_memory(ptr, capacity, (unsigned char)(i + 1),
                     "growth allocation intact", __FILE__, __LINE__);
        i++;
    }

    check_result(i == (size_t)BLOCK_GROWTH_COUNT,
                 "all growth blocks allocated", __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_clear_and_reuse(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t i = 0;

    check_result(arena_pool != NULL, "create for clear test",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    while (i < (size_t)CLEAR_REUSE_COUNT) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
        check_result(ptr != NULL, "pre-clear allocation", __FILE__, __LINE__);
        if (ptr == NULL) {
            break;
        }
        memset(ptr, (int)(0x20 + i), capacity);
        i++;
    }

    check_result(i == (size_t)CLEAR_REUSE_COUNT,
                 "pre-clear blocks allocated", __FILE__, __LINE__);

    arena_clear(arena_pool);

    i = 0;
    while (i < (size_t)CLEAR_REUSE_COUNT) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
        check_result(ptr != NULL, "post-clear allocation", __FILE__, __LINE__);
        if (ptr == NULL) {
            break;
        }
        memset(ptr, (int)(0x70 + i), capacity);
        check_memory(ptr, capacity, (unsigned char)(0x70 + i),
                     "post-clear block writable", __FILE__, __LINE__);
        i++;
    }

    check_result(i == (size_t)CLEAR_REUSE_COUNT,
                 "post-clear blocks allocated", __FILE__, __LINE__);

    arena_clear(arena_pool);
    check_result(arena_malloc(arena_pool, 1) != NULL,
                 "arena usable after repeated clear", __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_clear_then_large_allocation(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t i = 0;

    check_result(arena_pool != NULL, "create for clear+large test",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    while (i < 4) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
        check_result(ptr != NULL, "pre-clear full block", __FILE__, __LINE__);
        if (ptr == NULL) {
            break;
        }
        i++;
    }

    check_result(i == 4, "pre-clear blocks created", __FILE__, __LINE__);

    arena_clear(arena_pool);

    {
        size_t big = capacity * 2 + 1;
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, big);

        check_result(ptr != NULL, "post-clear large allocation",
                     __FILE__, __LINE__);
        if (ptr != NULL) {
            check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                         "post-clear large aligned", __FILE__, __LINE__);
            memset(ptr, 0x5A, big);
            check_memory(ptr, big, 0x5A, "post-clear large writable",
                         __FILE__, __LINE__);
        }
    }

    {
        size_t j = 0;
        while (j < 16) {
            unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, 32);
            check_result(ptr != NULL, "post-clear small allocation",
                         __FILE__, __LINE__);
            if (ptr == NULL) {
                break;
            }
            check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                         "post-clear small aligned", __FILE__, __LINE__);
            j++;
        }
        check_result(j == 16, "post-clear small allocations done",
                     __FILE__, __LINE__);
    }

    arena_clear(arena_pool);
    check_result(arena_malloc(arena_pool, capacity) != NULL,
                 "reusable after clear following large alloc",
                 __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_extreme_allocation_sizes(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_pool != NULL, "create for extreme sizes",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    check_result(arena_malloc(arena_pool, SIZE_MAX) == NULL,
                 "malloc(SIZE_MAX) fails", __FILE__, __LINE__);
    check_result(arena_malloc(arena_pool, SIZE_MAX - 1) == NULL,
                 "malloc(SIZE_MAX-1) fails", __FILE__, __LINE__);
    check_result(arena_malloc(arena_pool, SIZE_MAX / 2 + 1) == NULL,
                 "malloc(SIZE_MAX/2+1) fails", __FILE__, __LINE__);
    check_result(arena_malloc(arena_pool,
                              (size_t)ARENA_BLOCK_DEFAULT_CAPACITY + 1) != NULL,
                 "malloc(capacity+1) succeeds", __FILE__, __LINE__);
    check_result(arena_malloc(arena_pool, 1) != NULL,
                 "arena usable after rejections", __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_arena_stats_observability(void)
{
    arena* arena_pool = arena_create();
    arena_stats stats;
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    unsigned char* ptr;

    check_result(arena_pool != NULL, "create for stats test", __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    /* Check initial state */
    arena_get_stats(arena_pool, &stats);
    check_result(stats.total_blocks == 0, "initial blocks == 0", __FILE__, __LINE__);
    check_result(stats.total_capacity == 0, "initial capacity == 0", __FILE__, __LINE__);
    check_result(stats.used_bytes == 0, "initial used bytes == 0", __FILE__, __LINE__);

    /* Allocate exactly one full block */
    ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
    check_result(ptr != NULL, "stats test allocation succeeds", __FILE__, __LINE__);
    
    arena_get_stats(arena_pool, &stats);
    check_result(stats.total_blocks == 1, "1 block allocated", __FILE__, __LINE__);
    check_result(stats.total_capacity == capacity, "capacity matches default", __FILE__, __LINE__);
    check_result(stats.used_bytes == capacity, "used bytes matches allocation", __FILE__, __LINE__);

    /* Clear the arena */
    arena_clear(arena_pool);
    arena_get_stats(arena_pool, &stats);
    
    check_result(stats.total_blocks == 0, "clear resets block count", __FILE__, __LINE__);
    check_result(stats.used_bytes == 0, "clear resets used bytes", __FILE__, __LINE__);

    /* Allocate the same size again */
    ptr = (unsigned char*)arena_malloc(arena_pool, capacity);
    check_result(ptr != NULL, "stats test re-allocation succeeds", __FILE__, __LINE__);
    
    arena_get_stats(arena_pool, &stats);
    check_result(stats.total_blocks == 1, "reused 1 block after clear", __FILE__, __LINE__);
    check_result(stats.total_capacity == capacity, "reused capacity matches", __FILE__, __LINE__);
    check_result(stats.used_bytes == capacity, "reused used bytes matches", __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_repeated_create_clear_delete(void)
{
    size_t round = 0;

    while (round < (size_t)REPEATED_ROUND_COUNT) {
        arena* arena_pool = arena_create();

        check_result(arena_pool != NULL, "repeated create succeeds",
                     __FILE__, __LINE__);
        if (arena_pool == NULL) {
            return;
        }

        check_result(arena_malloc(arena_pool, 1) != NULL,
                     "repeated alloc succeeds", __FILE__, __LINE__);

        arena_clear(arena_pool);

        check_result(arena_malloc(arena_pool, 1) != NULL,
                     "repeated alloc after clear succeeds",
                     __FILE__, __LINE__);

        arena_delete(arena_pool);
        round++;
    }
}

static void test_delete_without_allocation(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_pool != NULL, "create for delete-only test",
                 __FILE__, __LINE__);
    if (arena_pool != NULL) {
        arena_delete(arena_pool);
    }
}

/* ================================================================ */
/* Random Stress                                                    */
/* ================================================================ */

static void test_random_stress(void)
{
    arena* arena_pool = arena_create();
    allocation_record* records;
    uint32_t random_state = UINT32_C(0x9E3779B9);
    size_t operation = 0;
    size_t active_count = 0;

    check_result(arena_pool != NULL, "create for random stress",
                 __FILE__, __LINE__);
    if (arena_pool == NULL) {
        return;
    }

    records = (allocation_record*)calloc(RANDOM_SLOT_COUNT, sizeof(*records));
    check_result(records != NULL, "random records allocated",
                 __FILE__, __LINE__);
    if (records == NULL) {
        arena_delete(arena_pool);
        return;
    }

    while (operation < (size_t)RANDOM_OPERATION_COUNT) {
        uint32_t choice = random_next(&random_state);

        if (choice % 100U < 4U) {
            size_t i = 0;

            arena_clear(arena_pool);

            while (i < RANDOM_SLOT_COUNT) {
                records[i].active = false;
                records[i].ptr = NULL;
                records[i].size = 0;
                i++;
            }

            active_count = 0;
        } else if (active_count < RANDOM_MAX_LIVE_RECORDS) {
            size_t slot = random_range(&random_state, RANDOM_SLOT_COUNT);
            size_t size = random_range(&random_state, RANDOM_MAX_ALLOCATION) + 1;
            unsigned char value = (unsigned char)random_next(&random_state);

            if (records[slot].active == false) {
                unsigned char* ptr =
                    (unsigned char*)arena_malloc(arena_pool, size);

                check_result(ptr != NULL, "random alloc succeeds",
                             __FILE__, __LINE__);

                if (ptr != NULL) {
                    size_t i = 0;

                    check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                                 "random alloc aligned", __FILE__, __LINE__);

                    while (i < size) {
                        ptr[i] = value;
                        i++;
                    }

                    records[slot].ptr = ptr;
                    records[slot].size = size;
                    records[slot].value = value;
                    records[slot].active = true;
                    active_count++;
                }
            }
        }

        if (operation % 113U == 0) {
            size_t i = 0;
            while (i < RANDOM_SLOT_COUNT) {
                if (records[i].active) {
                    check_memory(records[i].ptr, records[i].size,
                                 records[i].value,
                                 "random content intact", __FILE__, __LINE__);
                }
                i++;
            }
        }

        if (operation % 251U == 0) {
            size_t i = 0;
            while (i < RANDOM_SLOT_COUNT) {
                size_t j = i + 1;
                if (records[i].active) {
                    uintptr_t begin_i = (uintptr_t)records[i].ptr;
                    uintptr_t end_i = begin_i + records[i].size;
                    while (j < RANDOM_SLOT_COUNT) {
                        if (records[j].active) {
                            uintptr_t begin_j = (uintptr_t)records[j].ptr;
                            uintptr_t end_j = begin_j + records[j].size;
                            check_result(end_i <= begin_j || end_j <= begin_i,
                                         "random no overlap",
                                         __FILE__, __LINE__);
                        }
                        j++;
                    }
                }
                i++;
            }
        }

        operation++;
    }

    free(records);
    arena_delete(arena_pool);
}

/* ================================================================ */

int main(void)
{
    printf("Arena allocator test suite (white-box)\n");
    printf("ARENA_ALIGNMENT: %zu\n", (size_t)ARENA_ALIGNMENT);
    printf("ARENA_BLOCK_DEFAULT_CAPACITY: %zu\n",
           (size_t)ARENA_BLOCK_DEFAULT_CAPACITY);
    printf("ARENA_BLOCKARRAY_DEFAULT_CAPACITY: %zu\n",
           (size_t)ARENA_BLOCKARRAY_DEFAULT_CAPACITY);
    printf("ARENA_BLOCKARRAY_EXPAND_FACTOR: %zu\n",
           (size_t)ARENA_BLOCKARRAY_EXPAND_FACTOR);

    test_offset_align_up_direct();
    test_block_init_direct();
    test_block_init_growth_direct();
    test_block_init_overflow_direct();
    test_arena_bump_direct();
    test_arena_bump_alignment_boundary_direct();
    test_arena_bump_capacity_not_multiple_direct();
    test_arena_bump_offset_overflow_direct();
    test_arena_use_block_init_direct();
    test_arena_blockarray_init_direct();
    test_arena_blockarray_expand_direct();
    test_arena_use_block_move_direct();
    test_arena_grow_direct();

    test_null_and_zero_arguments();
    test_basic_allocation_and_alignment();
    test_calloc_overflow();
    test_block_boundary();
    test_large_allocation_and_block_growth();
    test_clear_and_reuse();
    test_clear_then_large_allocation();
    test_extreme_allocation_sizes();
    test_arena_stats_observability();
    test_repeated_create_clear_delete();
    test_delete_without_allocation();
    test_random_stress();

    printf("\nTests run: %zu\n", tests_run);
    printf("Tests failed: %zu\n", tests_failed);

    if (tests_failed != 0) {
        printf("RESULT: FAILED\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASSED\n");
    return EXIT_SUCCESS;
}