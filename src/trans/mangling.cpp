/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling.hpp
 * - Name mangling support
 *
 *
 * $D = ! type
 * $A = Array
 * $S = *-ptr
 * $R = &-ptr
 * $P = + symbol
 * $E = = symbol
 * $C = , symbol
 * $H = # symbol
 * $pL/$pR = Left/right paren
 * $aL/$aR = Left/right angle (<>)
 */
#include "mangling.hpp"
#include <hir/type.hpp>
#include <hir/path.hpp>

namespace {
    ::std::string   escape_str(const ::std::string& s) {
        ::std::string   output;
        output.reserve(s.size() + 1);
        for(auto v : s)
            if( v == '#' )
                output += "$H";
            else if( v == '-' )
                output += "_";
            else
                output += v;
        return output;
    }
    ::FmtLambda emit_params(const ::HIR::PathParams& params)
    {
        return FMT_CB(ss,
            if( params.m_types.size() > 0 )
            {
                ss << "$aL";
                for(unsigned int i = 0; i < params.m_types.size(); i ++)
                {
                    if(i != 0)  ss << "$C";
                    ss << Trans_Mangle( params.m_types[i] );
                }
                ss << "$aR";
            }
            );
    }
}


::FmtLambda Trans_Mangle(const ::HIR::SimplePath& path)
{
    return FMT_CB(ss,
        ss << "_ZN";
        {
            ::std::string   cn;
            for(auto c : path.m_crate_name)
            {
                if(c == '-') {
                    cn += "$$";
                }
                else if(  ('0' <= c && c <= '9')
                       || ('A' <= c && c <= 'Z')
                       || ('a' <= c && c <= 'z')
                       || c == '_'
                       )
                {
                    cn += c;
                }
                else {
                }
            }
            ss << cn.size() << cn;
        }
        for(const auto& comp : path.m_components) {
            auto v = escape_str(comp);
            ss << v.size() << v;
        }
        );
}
::FmtLambda Trans_Mangle(const ::HIR::GenericPath& path)
{
    return FMT_CB(ss,
        ss << Trans_Mangle(path.m_path);
        ss << emit_params(path.m_params);
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
        return FMT_CB(ss,
            ss << "_ZRK$aL";
            ss << Trans_Mangle(*pe.type);
            ss << "_as_";
            ss << Trans_Mangle(pe.trait);
            ss << "$aR";
            auto v = escape_str(pe.item);
            ss << v.size() << v;
            ss << emit_params(pe.params);
            );
        ),
    (UfcsInherent,
        return FMT_CB(ss,
            ss << "_ZRI$aL";
            ss << Trans_Mangle(*pe.type);
            ss << "$aR";
            auto v = escape_str(pe.item);
            ss << v.size() << v;
            ss << emit_params(pe.params);
            );
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
        return FMT_CB(ss,
            ss << "$pL";
            ss << Trans_Mangle(te.m_trait.m_path);
            for(const auto& bound : te.m_trait.m_type_bounds) {
                ss << "_" << bound.first << "$E" << Trans_Mangle(bound.second);
            }
            for(const auto& marker : te.m_markers) {
                ss << "$P" << Trans_Mangle(marker);
            }
            ss << "$pR";
            );
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
            ss << "$T" << te.size();
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
            ss << "$S";
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
                ss << "extern_" << escape_str(te.m_abi) << "_";
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

