#include "arena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*
 * Arena allocator test suite.
 *
 * Compile:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
 *      -Wstrict-prototypes -Wmissing-prototypes \
 *      arena.c arena_test.c -o arena_test
 *
 * Run:
 *   ./arena_test
 *
 * Sanitizers (Clang/GCC):
 *   cc -std=c11 -g -O1 -fno-omit-frame-pointer \
 *      -fsanitize=address,undefined \
 *      arena.c arena_test.c -o arena_test
 *   ./arena_test
 */

enum {
    RANDOM_SLOT_COUNT = 4096,
    RANDOM_OPERATION_COUNT = 50000,
    RANDOM_CLEAR_INTERVAL = 997,
    RANDOM_MAX_ALLOCATION = 8192,
    RANDOM_MAX_LIVE_RECORDS = 4096,
    LARGE_BLOCK_COUNT = 8
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

static void test_null_and_zero_arguments(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_malloc(NULL, 1) == NULL,
                 "arena_malloc(NULL, 1) == NULL", __FILE__, __LINE__);
    check_result(arena_malloc(NULL, 0) == NULL,
                 "arena_malloc(NULL, 0) == NULL", __FILE__, __LINE__);
    check_result(arena_calloc(NULL, 1, 1) == NULL,
                 "arena_calloc(NULL, 1, 1) == NULL", __FILE__, __LINE__);
    check_result(arena_calloc(NULL, 0, 1) == NULL,
                 "arena_calloc(NULL, 0, 1) == NULL", __FILE__, __LINE__);
    check_result(arena_calloc(NULL, 1, 0) == NULL,
                 "arena_calloc(NULL, 1, 0) == NULL", __FILE__, __LINE__);

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
        arena_clear(arena_pool);
        arena_delete(arena_pool);
    }

    arena_clear(NULL);
    arena_delete(NULL);
}

static void test_basic_allocation_and_alignment(void)
{
    arena* arena_pool = arena_create();
    size_t sizes[] = {
        1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513
    };
    size_t i = 0;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds", __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    while (i < sizeof(sizes) / sizeof(sizes[0])) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, sizes[i]);
        size_t j = 0;

        check_result(ptr != NULL,
                     "arena_malloc() returns memory", __FILE__, __LINE__);

        if (ptr != NULL) {
            check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                         "allocation is ARENA_ALIGNMENT-aligned",
                         __FILE__, __LINE__);

            while (j < sizes[i]) {
                ptr[j] = (unsigned char)(i + j);
                j++;
            }

            j = 0;
            while (j < sizes[i]) {
                check_result(ptr[j] == (unsigned char)(i + j),
                             "written byte can be read back",
                             __FILE__, __LINE__);
                j++;
            }
        }

        i++;
    }

    arena_delete(arena_pool);
}

static void test_calloc(void)
{
    arena* arena_pool = arena_create();
    size_t sizes[] = { 1, 2, 3, 7, 16, 31, 64, 127, 256, 1024 };
    size_t i = 0;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for calloc test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    while (i < sizeof(sizes) / sizeof(sizes[0])) {
        size_t count = sizes[i];
        size_t size = 3;
        size_t total = count * size;
        unsigned char* ptr =
            (unsigned char*)arena_calloc(arena_pool, count, size);

        check_result(ptr != NULL,
                     "arena_calloc() returns memory", __FILE__, __LINE__);

        if (ptr != NULL) {
            check_zero_memory(ptr, total, "arena_calloc() zeroes all bytes",
                              __FILE__, __LINE__);

            memset(ptr, 0xA5, total);

            check_memory(ptr, total, 0xA5,
                         "calloc memory remains writable",
                         __FILE__, __LINE__);
        }

        i++;
    }

    arena_delete(arena_pool);
}

static void test_calloc_overflow(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for overflow test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    check_result(arena_calloc(arena_pool, SIZE_MAX, 2) == NULL,
                 "calloc rejects SIZE_MAX * 2",
                 __FILE__, __LINE__);

    check_result(arena_calloc(arena_pool, SIZE_MAX / 2 + 1, 2) == NULL,
                 "calloc rejects multiplication overflow",
                 __FILE__, __LINE__);

    check_result(arena_calloc(arena_pool, SIZE_MAX, SIZE_MAX) == NULL,
                 "calloc rejects SIZE_MAX * SIZE_MAX",
                 __FILE__, __LINE__);

    check_result(arena_calloc(arena_pool, 1, SIZE_MAX) == NULL,
                 "calloc of SIZE_MAX bytes fails safely or returns NULL",
                 __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_block_boundary(void)
{
    arena* arena_pool = arena_create();
    unsigned char* ptr;
    unsigned char* next;
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for block boundary test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    /*
     * A fresh block has offset zero. Its configured capacity is a multiple
     * of the default alignment for the supplied configuration.
     */
    ptr = (unsigned char*)arena_malloc(arena_pool, capacity);

    check_result(ptr != NULL,
                 "allocation equal to default block capacity succeeds",
                 __FILE__, __LINE__);

    if (ptr != NULL) {
        check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                     "full-block allocation is aligned",
                     __FILE__, __LINE__);

        memset(ptr, 0x6D, capacity);
        check_memory(ptr, capacity, 0x6D,
                     "full-block allocation is writable",
                     __FILE__, __LINE__);
    }

    next = (unsigned char*)arena_malloc(arena_pool, 1);

    check_result(next != NULL,
                 "allocation after a full block grows the arena",
                 __FILE__, __LINE__);

    if (next != NULL) {
        check_result((uintptr_t)next % ARENA_ALIGNMENT == 0,
                     "allocation in next block is aligned",
                     __FILE__, __LINE__);
        *next = 0x3C;
        check_result(*next == 0x3C,
                     "allocation in next block is writable",
                     __FILE__, __LINE__);
    }

    arena_delete(arena_pool);
}

static void test_large_allocation_and_block_growth(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t i = 0;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for block growth test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    while (i < LARGE_BLOCK_COUNT) {
        unsigned char* ptr =
            (unsigned char*)arena_malloc(arena_pool, capacity);

        check_result(ptr != NULL,
                     "large allocation forces block creation",
                     __FILE__, __LINE__);

        if (ptr == NULL) {
            break;
        }

        check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                     "large block allocation is aligned",
                     __FILE__, __LINE__);

        memset(ptr, (int)(i + 1), capacity);
        check_memory(ptr, capacity, (unsigned char)(i + 1),
                     "large block content remains intact",
                     __FILE__, __LINE__);

        i++;
    }

    check_result(i == LARGE_BLOCK_COUNT,
                 "multiple blocks and block-array expansion succeeded",
                 __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_clear_and_reuse(void)
{
    arena* arena_pool = arena_create();
    size_t capacity = (size_t)ARENA_BLOCK_DEFAULT_CAPACITY;
    size_t i = 0;
    unsigned char* first = NULL;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for clear test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    while (i < 5) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);

        check_result(ptr != NULL,
                     "allocate block before clear",
                     __FILE__, __LINE__);

        if (ptr == NULL) {
            break;
        }

        memset(ptr, (int)(0x20 + i), capacity);

        if (i == 0) {
            first = ptr;
        }

        i++;
    }

    check_result(i == 5,
                 "five blocks allocated before clear",
                 __FILE__, __LINE__);

    arena_clear(arena_pool);

    /*
     * The arena contract invalidates previous allocations after clear.
     * Do not read or write old pointers here. Verify only that fresh
     * allocations work and that the arena remains usable.
     */
    i = 0;
    while (i < 5) {
        unsigned char* ptr = (unsigned char*)arena_malloc(arena_pool, capacity);

        check_result(ptr != NULL,
                     "allocation succeeds after clear",
                     __FILE__, __LINE__);

        if (ptr == NULL) {
            break;
        }

        memset(ptr, (int)(0x70 + i), capacity);
        check_memory(ptr, capacity, (unsigned char)(0x70 + i),
                     "reused block can be written and read",
                     __FILE__, __LINE__);

        i++;
    }

    check_result(i == 5,
                 "previously allocated blocks can be reused",
                 __FILE__, __LINE__);

    (void)first;

    arena_clear(arena_pool);
    arena_clear(arena_pool);

    check_result(arena_malloc(arena_pool, 64) != NULL,
                 "allocation succeeds after repeated clear",
                 __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_extreme_allocation_sizes(void)
{
    arena* arena_pool = arena_create();

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for extreme-size test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    check_result(arena_malloc(arena_pool, SIZE_MAX) == NULL,
                 "arena_malloc(SIZE_MAX) fails safely",
                 __FILE__, __LINE__);

    check_result(arena_malloc(arena_pool, SIZE_MAX - 1) == NULL,
                 "arena_malloc(SIZE_MAX - 1) fails safely",
                 __FILE__, __LINE__);

    check_result(arena_malloc(arena_pool, SIZE_MAX / 2 + 1) == NULL,
                 "arena_malloc(large size) fails safely",
                 __FILE__, __LINE__);

    check_result(arena_malloc(arena_pool, (size_t)ARENA_BLOCK_DEFAULT_CAPACITY + 1)
                     != NULL,
                 "allocation slightly larger than default block succeeds",
                 __FILE__, __LINE__);

    arena_delete(arena_pool);
}

static void test_random_stress(void)
{
    arena* arena_pool = arena_create();
    allocation_record* records;
    uint32_t random_state = UINT32_C(0x9E3779B9);
    size_t operation = 0;
    size_t active_count = 0;

    check_result(arena_pool != NULL,
                 "arena_create() succeeds for random stress test",
                 __FILE__, __LINE__);

    if (arena_pool == NULL) {
        return;
    }

    records = (allocation_record*)calloc(RANDOM_SLOT_COUNT, sizeof(*records));

    check_result(records != NULL,
                 "test record allocation succeeds",
                 __FILE__, __LINE__);

    if (records == NULL) {
        arena_delete(arena_pool);
        return;
    }

    while (operation < RANDOM_OPERATION_COUNT) {
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

                check_result(ptr != NULL,
                             "random allocation succeeds",
                             __FILE__, __LINE__);

                if (ptr != NULL) {
                    size_t i = 0;

                    check_result((uintptr_t)ptr % ARENA_ALIGNMENT == 0,
                                 "random allocation is aligned",
                                 __FILE__, __LINE__);

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
                                 "random allocation contents remain intact",
                                 __FILE__, __LINE__);
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
                                         "active allocations do not overlap",
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

    {
        size_t i = 0;

        while (i < RANDOM_SLOT_COUNT) {
            if (records[i].active) {
                check_memory(records[i].ptr, records[i].size,
                             records[i].value,
                             "final random allocation contents remain intact",
                             __FILE__, __LINE__);
            }

            i++;
        }
    }

    free(records);
    arena_delete(arena_pool);
}

static void test_repeated_create_clear_delete(void)
{
    size_t round = 0;

    while (round < 1000) {
        arena* arena_pool = arena_create();

        check_result(arena_pool != NULL,
                     "repeated arena_create() succeeds",
                     __FILE__, __LINE__);

        if (arena_pool == NULL) {
            return;
        }

        check_result(arena_malloc(arena_pool, 1) != NULL,
                     "repeated arena allocation succeeds",
                     __FILE__, __LINE__);

        arena_clear(arena_pool);

        check_result(arena_malloc(arena_pool, 1) != NULL,
                     "repeated allocation after clear succeeds",
                     __FILE__, __LINE__);

        arena_delete(arena_pool);

        round++;
    }
}

int main(void)
{
    printf("Arena allocator test suite\n");
    printf("ARENA_ALIGNMENT: %zu\n", (size_t)ARENA_ALIGNMENT);
    printf("ARENA_BLOCK_DEFAULT_CAPACITY: %zu\n",
           (size_t)ARENA_BLOCK_DEFAULT_CAPACITY);
    printf("ARENA_BLOCKARRAY_DEFAULT_CAPACITY: %zu\n",
           (size_t)ARENA_BLOCKARRAY_DEFAULT_CAPACITY);
    printf("ARENA_BLOCKARRAY_EXPAND_FACTOR: %zu\n",
           (size_t)ARENA_BLOCKARRAY_EXPAND_FACTOR);

    test_null_and_zero_arguments();
    test_basic_allocation_and_alignment();
    test_calloc();
    test_calloc_overflow();
    test_block_boundary();
    test_large_allocation_and_block_growth();
    test_clear_and_reuse();
    test_extreme_allocation_sizes();
    test_random_stress();
    test_repeated_create_clear_delete();

    printf("\nTests run: %zu\n", tests_run);
    printf("Tests failed: %zu\n", tests_failed);

    if (tests_failed != 0) {
        printf("RESULT: FAILED\n");
        return EXIT_FAILURE;
    }

    printf("RESULT: PASSED\n");
    return EXIT_SUCCESS;
}
