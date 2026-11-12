
# Arena Allocator

A lightweight and high-performance C language region-based memory allocator (Arena / Bump Allocator).

This project uses "pointer bumping" to allocate memory. It requests large chunks of memory from the system and hands out smaller pieces sequentially to the caller. Because it **does not** provide an interface to free individual allocations, it is extremely fast (almost O(1) pointer arithmetic) and perfectly suited for use cases where objects share the same lifecycle and can be freed all at once (e.g., AST nodes in a parser, network request contexts, or batch processing tasks).

## Features

* **Blazing Fast**: Allocates memory by simply advancing an internal pointer, eliminating the need to search through free lists.
* **Auto-Expansion**: Uses a dynamic array (similar to C++ `std::vector`) to manage memory chunks. When the current chunk is full, it automatically allocates a larger one.
* **Memory Reuse**: `arena_clear()` resets the entire pool in O(1) time. Previously acquired memory chunks are retained for future reuse rather than being returned to the OS.
* **Strict Alignment**: All returned pointers are guaranteed to be aligned to `max_align_t` (or a 16-byte fallback).
* **Robust Protection**: Built-in integer overflow checks and strict boundary validation.

## Quick Start

### Compilation
Simply add `arena.c` and `arena.h` to your project and compile them with your sources.

```sh
cc -std=c11 -O2 arena.c your_program.c -o your_program
```

### Basic Usage

```c
#include "arena.h"
#include <stdio.h>

int main(void) {
    // 1. Create an arena instance
    arena* pool = arena_create();
    if (pool == NULL) return 1;

    // 2. Allocate memory (no need to free individually)
    int* numbers = (int*)arena_malloc(pool, 10 * sizeof(int));
    char* string = (char*)arena_calloc(pool, 64, sizeof(char));

    // Use the memory...
    numbers[0] = 42;

    // 3. Reset the pool: all previous pointers become invalid, 
    // but the underlying memory is retained and reused.
    arena_clear(pool);

    // 4. Subsequent allocations will quickly reuse the previous chunks
    void* new_data = arena_malloc(pool, 128);

    // 5. Destroy the pool: releases all memory back to the system
    arena_delete(pool);
    
    return 0;
}
```

## API Reference

### Lifecycle Management
* `arena* arena_create(void)`
  Creates a new allocator instance. Returns `NULL` on failure.
* `void arena_clear(arena* arena_pool)`
  **Resets the allocator**. Internal offsets are reset to 0. All previously returned pointers are invalidated. Note: This does **not** return memory to the OS, but prepares it for subsequent allocations.
* `void arena_delete(arena* arena_pool)`
  **Destroys the allocator**. Frees all internally held memory chunks and the `arena` instance itself. The pointer must not be used after this call.

### Memory Allocation
* `void* arena_malloc(arena* arena_pool, size_t n)`
  Allocates `n` bytes. **Memory is not initialized**. Returns `NULL` on failure or if `n == 0`.
* `void* arena_calloc(arena* arena_pool, size_t count, size_t size)`
  Allocates `count * size` bytes and **zero-initializes** the memory. Includes built-in multiplication overflow detection.

> **Note**: This project **does not** support freeing individual allocations (there is no `arena_free`). Memory is either reused in bulk via `arena_clear()` or destroyed via `arena_delete()`.

## Configuration Macros

You can override the default behavior by defining these macros before including `arena.h`, or via compiler flags:

| Macro | Default Value | Description |
| :--- | :--- | :--- |
| `ARENA_ALIGNMENT` | `_Alignof(max_align_t)` or `16` | Memory alignment granularity in bytes. Must be a power of two. |
| `ARENA_BLOCK_DEFAULT_CAPACITY` | `1048576` (1MB) | Initial capacity of a single memory block. Doubles if a larger request is made. |
| `ARENA_BLOCKARRAY_DEFAULT_CAPACITY` | `3` | Initial capacity of the dynamic array tracking the chunks. |
| `ARENA_BLOCKARRAY_EXPAND_FACTOR` | `2` | Expansion multiplier when the tracking array is full. |

## Important Notes & Limitations

1. **Not Thread-Safe**
   The allocator contains **no locking mechanisms**. If used in a multi-threaded environment, ensure external synchronization or assign a dedicated `arena` instance to each thread.
2. **Pointer Invalidation**
   After calling `arena_clear()`, all pointers previously returned by `arena_malloc` are **immediately invalidated**. Accessing them results in Undefined Behavior (UB) and risks data corruption.
3. **Custom Alignment**
   All allocations are aligned to `ARENA_ALIGNMENT`. If you require stricter alignment (e.g., 32 or 64 bytes for SIMD), you must manually request extra space and align the pointer in your application layer.

## Testing

The project includes a comprehensive white-box and black-box test suite covering boundary conditions, internal expansion logic, random stress tests, and alignment validation. The test suite executes **over 220,000 assertions** to ensure absolute stability.

### How to Run Tests

```sh
# Standard compilation (strict warnings)
cc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow arena.c arena_test_ds.c -o arena_test_ds
./arena_test_ds

# Compile with AddressSanitizer and UndefinedBehaviorSanitizer (Highly Recommended)
cc -std=c11 -g -fsanitize=address,undefined arena.c arena_test_ds.c -o arena_test_ds_asan
./arena_test_ds_asan
```

### Test File Structure (Excerpt)

The test suite is highly rigorous. It tests internal `static` functions directly by including the `.c` file, allowing precise manipulation of the internal state machine.

Here is the core testing infrastructure and a sample of the white-box tests:

```c
#include "arena.c" /* Include the implementation for white-box testing */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static size_t tests_run = 0;
static size_t tests_failed = 0;

static void check_result(bool condition, const char* expression, const char* file, int line)
{
    tests_run++;
    if (condition == false) {
        tests_failed++;
        fprintf(stderr, "FAIL: %s (%s:%d)\n", expression, file, line);
    }
}

/* Example: Direct white-box test of the bump allocator's alignment boundary */
static void test_arena_bump_alignment_boundary_direct(void)
{
    arena pool;
    block bk;
    unsigned char* buf = (unsigned char*)malloc(64);
    
    check_result(buf != NULL, "boundary buffer allocated", __FILE__, __LINE__);
    if (buf == NULL) return;

    memset(&pool, 0, sizeof(pool));
    memset(&bk, 0, sizeof(bk));

    bk.base = buf;
    bk.capacity = 64;
    bk.offset = 0;

    pool.bk_first = &bk;
    pool.bk_last = &bk + 1;
    pool.bk_end = &bk + 1;

    /* First allocation takes 1 byte */
    check_result(arena_bump(&pool, 1) == buf, "first byte at base", __FILE__, __LINE__);

    /* Requesting 63 bytes fails because alignment moves offset to 16, leaving only 48 bytes */
    check_result(arena_bump(&pool, 63) == NULL, 
                 "bump fails when aligned request exceeds remaining", __FILE__, __LINE__);

    /* Requesting exactly the remaining capacity succeeds */
    void* p = arena_bump(&pool, 64 - (size_t)ARENA_ALIGNMENT);
    check_result(p == buf + (size_t)ARENA_ALIGNMENT, 
                 "bump exact-fit after alignment succeeds", __FILE__, __LINE__);

    free(buf);
}

int main(void)
{
    // ... Run all tests ...
    test_arena_bump_alignment_boundary_direct();
    
    printf("\nTests run: %zu\n", tests_run);
    printf("Tests failed: %zu\n", tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.

