/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/helpers.hpp
 * - MIR Manipulation helpers
 */
#include "helpers.hpp"

#include <hir/hir.hpp>
#include <hir/type.hpp>
#include <mir/mir.hpp>

void ::MIR::TypeResolve::fmt_pos(::std::ostream& os) const
{
    os << this->m_path << " BB" << this->bb_idx << "/";
    if( this->stmt_idx == STMT_TERM ) {
        os << "TERM";
    }
    else {
        os << this->stmt_idx;
    }
    os << ": ";
}
void ::MIR::TypeResolve::print_msg(const char* tag, ::std::function<void(::std::ostream& os)> cb) const
{
    auto& os = ::std::cerr;
    os << "MIR " << tag << ": ";
    fmt_pos(os);
    cb(os);
    os << ::std::endl;
    abort();
    //throw CheckFailure {};
}

const ::MIR::BasicBlock& ::MIR::TypeResolve::get_block(::MIR::BasicBlockId id) const
{
    MIR_ASSERT(*this, id < m_fcn.blocks.size(), "Block ID " << id << " out of range");
    return m_fcn.blocks[id];
}

const ::HIR::TypeRef& ::MIR::TypeResolve::get_static_type(::HIR::TypeRef& tmp, const ::HIR::Path& path) const
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        MIR_ASSERT(*this, pe.m_params.m_types.empty(), "Path params on static");
        const auto& s = m_crate.get_static_by_path(sp, pe.m_path);
        return s.m_type;
        ),
    (UfcsKnown,
        MIR_TODO(*this, "LValue::Static - UfcsKnown - " << path);
        ),
    (UfcsUnknown,
        MIR_BUG(*this, "Encountered UfcsUnknown in LValue::Static - " << path);
        ),
    (UfcsInherent,
        MIR_TODO(*this, "LValue::Static - UfcsInherent - " << path);
        )
    )
    throw "";
}
const ::HIR::TypeRef& ::MIR::TypeResolve::get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const
{
    TU_MATCH(::MIR::LValue, (val), (e),
    (Variable,
        MIR_ASSERT(*this, e < m_fcn.named_variables.size(), val << " out of range (" << m_fcn.named_variables.size() << ")");
        return m_fcn.named_variables.at(e);
        ),
    (Temporary,
        MIR_ASSERT(*this, e.idx < m_fcn.temporaries.size(), val << " out of range (" << m_fcn.temporaries.size() << ")");
        return m_fcn.temporaries.at(e.idx);
        ),
    (Argument,
        MIR_ASSERT(*this, e.idx < m_args.size(), val << " out of range (" << m_args.size() << ")");
        return m_args.at(e.idx).second;
        ),
    (Static,
        return get_static_type(tmp,  e);
        ),
    (Return,
        return m_ret_type;
        ),
    (Field,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Field access on unexpected type - " << ty);
            ),
        // Array and Slice use LValue::Field when the index is constant and known-good
        (Array,
            return *te.inner;
            ),
        (Slice,
            return *te.inner;
            ),
        (Tuple,
            MIR_ASSERT(*this, e.field_index < te.size(), "Field index out of range in tuple " << e.field_index << " >= " << te.size());
            return te[e.field_index];
            ),
        (Path,
            MIR_ASSERT(*this, te.binding.is_Struct(), "Field on non-Struct - " << ty);
            const auto& str = *te.binding.as_Struct();
            auto monomorph = [&](const auto& ty)->const auto& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, ty);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };
            TU_MATCHA( (str.m_data), (se),
            (Unit,
                MIR_BUG(*this, "Field on unit-like struct - " << ty);
                ),
            (Tuple,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in tuple-struct " << te.path);
                return monomorph(se[e.field_index].ent);
                ),
            (Named,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in struct " << te.path);
                return monomorph(se[e.field_index].second.ent);
                )
            )
            )
        )
        ),
    (Deref,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Deref on unexpected type - " << ty);
            ),
        (Path,
            if( const auto* inner_ptr = this->is_type_owned_box(ty) )
            {
                return *inner_ptr;
            }
            else {
                MIR_BUG(*this, "Deref on unexpected type - " << ty);
            }
            ),
        (Pointer,
            return *te.inner;
            ),
        (Borrow,
            return *te.inner;
            )
        )
        ),
    (Index,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Index on unexpected type - " << ty);
            ),
        (Slice,
            return *te.inner;
            ),
        (Array,
            return *te.inner;
            )
        )
        ),
    (Downcast,
        const auto& ty = this->get_lvalue_type(tmp, *e.val);
        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            MIR_BUG(*this, "Downcast on unexpected type - " << ty);
            ),
        (Path,
            MIR_ASSERT(*this, te.binding.is_Enum() || te.binding.is_Union(), "Downcast on non-Enum");
            if( te.binding.is_Enum() )
            {
                const auto& enm = *te.binding.as_Enum();
                const auto& variants = enm.m_variants;
                MIR_ASSERT(*this, e.variant_index < variants.size(), "Variant index out of range");
                const auto& variant = variants[e.variant_index];
                // TODO: Make data variants refer to associated types (unify enum and struct handling)
                TU_MATCHA( (variant.second), (ve),
                (Value,
                    ),
                (Unit,
                    ),
                (Tuple,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.ent) );
                    tmp = ::HIR::TypeRef( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                    ),
                (Struct,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.second.ent) );
                    tmp = ::HIR::TypeRef( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                    )
                )
            }
            else
            {
                const auto& unm = *te.binding.as_Union();
                MIR_ASSERT(*this, e.variant_index < unm.m_variants.size(), "Variant index out of range");
                const auto& variant = unm.m_variants[e.variant_index];
                const auto& var_ty = variant.second.ent;

                if( monomorphise_type_needed(var_ty) ) {
                    tmp = monomorphise_type(sp, unm.m_params, te.path.m_data.as_Generic().m_params, variant.second.ent);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return var_ty;
                }
            }
            )
        )
        )
    )
    throw "";
}

::HIR::TypeRef MIR::TypeResolve::get_const_type(const ::MIR::Constant& c) const
{
    TU_MATCHA( (c), (e),
    (Int,
        return e.t;
        ),
    (Uint,
        return e.t;
        ),
    (Float,
        return e.t;
        ),
    (Bool,
        return ::HIR::CoreType::Bool;
        ),
    (Bytes,
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_array( ::HIR::CoreType::U8, e.size() ) );
        ),
    (StaticString,
        return ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::CoreType::Str );
        ),
    (Const,
        MonomorphState  p;
        auto v = m_resolve.get_value(this->sp, e.p, p, /*signature_only=*/true);
        if( const auto* ve = v.opt_Constant() ) {
            const auto& ty = (*ve)->m_type;
            if( monomorphise_type_needed(ty) )
                MIR_TODO(*this, "get_const_type - Monomorphise type " << ty);
            else
                return ty.clone();
        }
        else {
            MIR_BUG(*this, "get_const_type - Not a constant");
        }
        ),
    (ItemAddr,
        MIR_TODO(*this, "get_const_type - Get type for constant `" << c << "`");
        )
    )
    throw "";
}
const ::HIR::TypeRef* ::MIR::TypeResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    return m_resolve.is_type_owned_box(ty);
}
