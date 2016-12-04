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

#define MIR_ASSERT(...) do{}while(0)
#define MIR_BUG(...) do{}while(0)
#define MIR_TODO(_, v...)    TODO(sp, v)

const ::HIR::TypeRef& ::MIR::TypeResolve::get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const
{
    TU_MATCH(::MIR::LValue, (val), (e),
    (Variable,
        return m_fcn.named_variables.at(e);
        ),
    (Temporary,
        return m_fcn.temporaries.at(e.idx);
        ),
    (Argument,
        return m_args.at(e.idx).second;
        ),
    (Static,
        TU_MATCHA( (e.m_data), (pe),
        (Generic,
            MIR_ASSERT(*this, pe.m_params.m_types.empty(), "Path params on static");
            const auto& s = m_crate.get_static_by_path(sp, pe.m_path);
            return s.m_type;
            ),
        (UfcsKnown,
            MIR_TODO(*this, "LValue::Static - UfcsKnown - " << e);
            ),
        (UfcsUnknown,
            MIR_BUG(*this, "Encountered UfcsUnknown in LValue::Static - " << e);
            ),
        (UfcsInherent,
            MIR_TODO(*this, "LValue::Static - UfcsInherent - " << e);
            )
        )
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
            TU_MATCHA( (str.m_data), (se),
            (Unit,
                MIR_BUG(*this, "Field on unit-like struct - " << ty);
                ),
            (Tuple,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in tuple-struct");
                const auto& fld = se[e.field_index];
                if( monomorphise_type_needed(fld.ent) ) {
                    tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return fld.ent;
                }
                ),
            (Named,
                MIR_ASSERT(*this, e.field_index < se.size(), "Field index out of range in struct");
                const auto& fld = se[e.field_index].second;
                if( monomorphise_type_needed(fld.ent) ) {
                    tmp = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return fld.ent;
                }
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
const ::HIR::TypeRef* ::MIR::TypeResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( m_lang_Box )
    {
        if( ! ty.m_data.is_Path() ) {
            return nullptr;
        }
        const auto& te = ty.m_data.as_Path();
        
        if( ! te.path.m_data.is_Generic() ) {
            return nullptr;
        }
        const auto& pe = te.path.m_data.as_Generic();
        
        if( pe.m_path != *m_lang_Box ) {
            return nullptr;
        }
        // TODO: Properly assert?
        return &pe.m_params.m_types.at(0);
    }
    else
    {
        return nullptr;
    }
}
