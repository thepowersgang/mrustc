/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/allocator.hpp
 * - Handling for switchable allocator backends
 */
#include <cstddef>

enum class AllocatorDataTy {
    // - Return
    Unit,   // ()
    ResultPtr,  // (..., *mut i8) + *mut u8
    // - Args
    Layout, // usize, usize
    Ptr,    // *mut u8
    Usize,  // usize
};
struct AllocatorMethod {
    const char* name;
    AllocatorDataTy ret;
    size_t  n_args;
    const AllocatorDataTy* args;    // Terminated by Never
};
enum class AllocatorKind {
    Global,
    DefaultLib,
    DefaultExe,
};

extern const AllocatorMethod   ALLOCATOR_METHODS[];
extern const size_t NUM_ALLOCATOR_METHODS;

