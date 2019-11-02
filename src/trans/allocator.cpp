/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/allocator.cpp
 * - Handling for switchable allocator backends
 */

#include "allocator.hpp"

#define DEF_METHOD_ARGS(name, ...)  const AllocatorDataTy ALLOCATOR_METHODS_ARGS_##name[] = { __VA_ARGS__ };
#define DEF_METHOD(name, ret)   { #name, AllocatorDataTy::ret, sizeof(ALLOCATOR_METHODS_ARGS_##name)/sizeof(AllocatorDataTy), ALLOCATOR_METHODS_ARGS_##name }

DEF_METHOD_ARGS(alloc, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(dealloc, AllocatorDataTy::Ptr, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(realloc, AllocatorDataTy::Ptr, AllocatorDataTy::Layout, AllocatorDataTy::Usize)
DEF_METHOD_ARGS(alloc_zeroed, AllocatorDataTy::Layout)

const AllocatorMethod   ALLOCATOR_METHODS[4] = {
    DEF_METHOD(alloc, ResultPtr),
    DEF_METHOD(dealloc, Unit),
    DEF_METHOD(realloc, ResultPtr),
    DEF_METHOD(alloc_zeroed, ResultPtr)
    };
const size_t NUM_ALLOCATOR_METHODS = sizeof(ALLOCATOR_METHODS)/sizeof(ALLOCATOR_METHODS[0]);
