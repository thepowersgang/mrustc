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
    Never,  // !
    Unit,   // ()
    ResultPtr,  // (..., *mut i8) + *mut u8
    ResultExcess,   // (..., *mut i8, *mut i8) + *mut u8
    UsizePair,  // (..., *mut usize, *mut usize) + ()
    ResultUnit, // i8
    // - Args
    Layout, // usize, usize
    LayoutRef,  // *const Layout  [actually *const i8]
    AllocError, // *const i8
    Ptr,    // *mut u8
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

extern const AllocatorMethod   ALLOCATOR_METHODS[10];

