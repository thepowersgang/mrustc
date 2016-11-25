/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/markings.cpp
 * - Fills the TraitMarkings structure on types
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>

namespace {

class Visitor:
    public ::HIR::Visitor
{
    const ::HIR::Crate& m_crate;
    const ::HIR::SimplePath&    m_lang_Unsize;
    const ::HIR::SimplePath&    m_lang_CoerceUnsized;
    const ::HIR::SimplePath&    m_lang_Deref;
public:
    Visitor(const ::HIR::Crate& crate):
        m_crate(crate),
        m_lang_Unsize( crate.get_lang_item_path_opt("unsize") ),
        m_lang_CoerceUnsized( crate.get_lang_item_path_opt("coerce_unsized") ),
        m_lang_Deref( crate.get_lang_item_path_opt("deref") )
    {
    }
    
    void visit_struct(::HIR::ItemPath ip, ::HIR::Struct& str) override
    {
        ::HIR::Visitor::visit_struct(ip, str);
    }

    void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
    {
        static Span sp;
        
        ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        
        if( impl.m_type.m_data.is_Path() )
        {
            const auto& te = impl.m_type.m_data.as_Path();
            const ::HIR::TraitMarkings* markings_ptr = nullptr;
            TU_MATCHA( (te.binding), (tpb),
            (Unbound, BUG(sp, "Unbound type path in trait impl - " << impl.m_type); ),
            (Opaque, ),
            (Struct, markings_ptr = &tpb->m_markings; ),
            (Union , markings_ptr = &tpb->m_markings; ),
            (Enum  , markings_ptr = &tpb->m_markings; )
            )
            if( markings_ptr )
            {
                ::HIR::TraitMarkings& markings = *const_cast<::HIR::TraitMarkings*>(markings_ptr);
                if( trait_path == m_lang_Unsize ) {
                    DEBUG("Type " << impl.m_type << " can Unsize");
                    markings.can_unsize = true;
                }
                else if( trait_path == m_lang_CoerceUnsized ) {
                    DEBUG("Type " << impl.m_type << " can Coerce");
                    markings.can_coerce = true;
                }
                else if( trait_path == m_lang_Deref ) {
                    DEBUG("Type " << impl.m_type << " can Deref");
                    markings.has_a_deref = true;
                }
                // TODO: Marker traits (with conditions)
                else {
                }
            }
        }
    }
};

}   // namespace

void ConvertHIR_Markings(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
}

