/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling.hpp
 * - Name mangling support
 */
#include "mangling.hpp"
#include <hir/type.hpp>
#include <hir/path.hpp>

::FmtLambda Trans_Mangle(const ::HIR::GenericPath& path)
{
    return FMT_CB(ss,
        ss << "_ZN" << path.m_path.m_crate_name.size() << path.m_path.m_crate_name;
        for(const auto& comp : path.m_path.m_components)
            ss << comp.size() << comp;
        );
}
::FmtLambda Trans_Mangle(const ::HIR::Path& path)
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        return Trans_Mangle(pe);
        ),
    (UfcsUnknown,
        BUG(Span(), "UfcsUnknown - " << path);
        ),
    (UfcsKnown,
        return FMT_CB(ss, );
        ),
    (UfcsInherent,
        return FMT_CB(ss, );
        )
    )
    throw "";
}
::FmtLambda Trans_Mangle(const ::HIR::TypeRef& ty)
{
    TU_MATCHA( (ty.m_data), (te),
    (Infer,
        BUG(Span(), "Infer in trans");
        ),
    (Diverge,
        return FMT_CB(ss, ss << "$D";);
        ),
    (Primitive,
        return FMT_CB(ss, ss << te;);
        ),
    (Path,
        return Trans_Mangle(te.path);
        ),
    (Generic,
        BUG(Span(), "Generic in trans - " << ty);
        ),
    (TraitObject,
        BUG(Span(), "Raw trait object - " << ty);
        ),
    (ErasedType,
        BUG(Span(), "ErasedType in trans - " << ty);
        ),
    (Array,
        return FMT_CB(ss, ss << "$A" << te.size_val << "_" << Trans_Mangle(*te.inner););
        ),
    (Slice,
        return FMT_CB(ss, ss << "$A" << "_" << Trans_Mangle(*te.inner););
        ),
    (Tuple,
        return FMT_CB(ss,
            ss << "$T";
            for(const auto& t : te)
                ss << "_" << Trans_Mangle(t);
            );
        ),
    (Borrow,
        return FMT_CB(ss,
            ss << "$R";
            switch(te.type)
            {
            case ::HIR::BorrowType::Shared: ss << "s"; break;
            case ::HIR::BorrowType::Unique: ss << "u"; break;
            case ::HIR::BorrowType::Owned : ss << "o"; break;
            }
            ss << "_" << Trans_Mangle(*te.inner);
            );
        ),
    (Pointer,
        return FMT_CB(ss,
            ss << "$P";
            switch(te.type)
            {
            case ::HIR::BorrowType::Shared: ss << "s"; break;
            case ::HIR::BorrowType::Unique: ss << "u"; break;
            case ::HIR::BorrowType::Owned : ss << "o"; break;
            }
            ss << "_" << Trans_Mangle(*te.inner);
            );
        ),
    (Function,
        return FMT_CB(ss,
            if(te.m_abi != "Rust")
                ss << "extern_" << te.m_abi << "_";
            if(te.is_unsafe)
                ss << "unsafe_";
            ss << "fn_" << te.m_arg_types.size();
            for(const auto& ty : te.m_arg_types)
                ss << "_" << Trans_Mangle(ty);
            ss << "_" << Trans_Mangle(*te.m_rettype);
            );
        ),
    (Closure,
        BUG(Span(), "Closure during trans - " << ty);
        )
    )
    
    throw "";
}

