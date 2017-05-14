/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/target.cpp
 * - Target-specific information
 */
#include "target.hpp"

// TODO: Replace with target selection
#define POINTER_SIZE_BYTES  8

bool Target_GetSizeAndAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align)
{
    TU_MATCHA( (ty.m_data), (te),
    (Infer,
        BUG(sp, "sizeof on _ type");
        ),
    (Diverge,
        out_size = 0;
        out_align = 0;
        return true;
        ),
    (Primitive,
        switch(te)
        {
        case ::HIR::CoreType::Bool:
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::I8:
            out_size = 1;
            out_align = 1;
            return true;
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::I16:
            out_size = 2;
            out_align = 2;
            return true;
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::Char:
            out_size = 4;
            out_align = 4;
            return true;
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::I64:
            out_size = 8;
            out_align = 8;
            return true;
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::I128:
            out_size = 16;
            // TODO: If i128 is emulated, this can be 8
            out_align = 16;
            return true;
        case ::HIR::CoreType::Usize:
        case ::HIR::CoreType::Isize:
            out_size = POINTER_SIZE_BYTES;
            out_align = POINTER_SIZE_BYTES;
            return true;
        case ::HIR::CoreType::F32:
            out_size = 4;
            out_align = 4;
            return true;
        case ::HIR::CoreType::F64:
            out_size = 8;
            out_align = 8;
            return true;
        case ::HIR::CoreType::Str:
            BUG(sp, "sizeof on a `str` - unsized");
        }
        ),
    (Path,
        // TODO:
        return false;
        ),
    (Generic,
        // Unknown - return false
        return false;
        ),
    (TraitObject,
        BUG(sp, "sizeof on a trait object - unsized");
        ),
    (ErasedType,
        BUG(sp, "sizeof on an erased type - shouldn't exist");
        ),
    (Array,
        // TODO: 
        size_t  size;
        if( !Target_GetSizeAndAlignOf(sp, *te.inner, size,out_align) )
            return false;
        size *= te.size_val;
        ),
    (Slice,
        BUG(sp, "sizeof on a slice - unsized");
        ),
    (Tuple,
        out_size = 0;
        out_align = 0;

        // TODO: Struct reordering
        for(const auto& t : te)
        {
            size_t  size, align;
            if( !Target_GetSizeAndAlignOf(sp, t, size,align) )
                return false;
            out_size += size;
            out_align = ::std::max(out_align, align);
        }
        ),
    (Borrow,
        // TODO
        ),
    (Pointer,
        // TODO
        ),
    (Function,
        // Pointer size
        ),
    (Closure,
        // TODO.
        )
    )
    return false;
}
bool Target_GetSizeOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size)
{
    size_t  ignore_align;
    return Target_GetSizeAndAlignOf(sp, ty, out_size, ignore_align);
}
bool Target_GetAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_align)
{
    size_t  ignore_size;
    return Target_GetSizeAndAlignOf(sp, ty, ignore_size, out_align);
}
