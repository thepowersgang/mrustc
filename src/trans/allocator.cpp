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
DEF_METHOD_ARGS(oom, AllocatorDataTy::AllocError)
DEF_METHOD_ARGS(dealloc, AllocatorDataTy::Ptr, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(usable_size, AllocatorDataTy::LayoutRef)
DEF_METHOD_ARGS(realloc, AllocatorDataTy::Ptr, AllocatorDataTy::Layout, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(alloc_zeroed, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(alloc_excess, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(realloc_excess, AllocatorDataTy::Ptr, AllocatorDataTy::Layout, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(grow_in_place, AllocatorDataTy::Ptr, AllocatorDataTy::Layout, AllocatorDataTy::Layout)
DEF_METHOD_ARGS(shrink_in_place, AllocatorDataTy::Ptr, AllocatorDataTy::Layout, AllocatorDataTy::Layout)

const AllocatorMethod   ALLOCATOR_METHODS[10] = {
    DEF_METHOD(alloc, ResultPtr),
    DEF_METHOD(oom, Never),
    DEF_METHOD(dealloc, Unit),
    DEF_METHOD(usable_size, UsizePair),
    DEF_METHOD(realloc, ResultPtr),
    DEF_METHOD(alloc_zeroed, ResultPtr),
    DEF_METHOD(alloc_excess, ResultExcess),
    DEF_METHOD(realloc_excess, ResultExcess),
    DEF_METHOD(grow_in_place, ResultUnit),
    DEF_METHOD(shrink_in_place, ResultUnit)
    };
