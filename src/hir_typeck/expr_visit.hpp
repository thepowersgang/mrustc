/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_visit.hpp
 * - Helpers for the HIR typecheck expression visiting
 */
#pragma once
#include <hir/item_path.hpp>

namespace typeck {
    struct ModuleState
    {
        const ::HIR::Crate& m_crate;

        const ::HIR::GenericPath*    m_current_trait;
        const ::HIR::GenericParams*   m_impl_generics;
        const ::HIR::GenericParams*   m_item_generics;

        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
        ::std::vector<HIR::SimplePath>  m_mod_paths;

        ModuleState(const ::HIR::Crate& crate):
            m_crate(crate),
            m_current_trait(nullptr),
            m_impl_generics(nullptr),
            m_item_generics(nullptr)
        {}

        template<typename T>
        class NullOnDrop {
            T*& ptr;
        public:
            NullOnDrop(T*& ptr):
                ptr(ptr)
            {}
            ~NullOnDrop() {
                ptr = nullptr;
            }
        };
        NullOnDrop<const ::HIR::GenericPath> set_current_trait(const ::HIR::GenericPath& p) {
            assert( !m_current_trait );
            m_current_trait = &p;
            return NullOnDrop<const ::HIR::GenericPath>(m_current_trait);
        }
        NullOnDrop<const ::HIR::GenericParams> set_impl_generics(const ::HIR::GenericParams& gps) {
            assert( !m_impl_generics );
            m_impl_generics = &gps;
            return NullOnDrop<const ::HIR::GenericParams>(m_impl_generics);
        }
        NullOnDrop<const ::HIR::GenericParams> set_item_generics(const ::HIR::GenericParams& gps) {
            assert( !m_item_generics );
            m_item_generics = &gps;
            return NullOnDrop<const ::HIR::GenericParams>(m_item_generics);
        }

        void prepare_from_path(const ::HIR::ItemPath& ip);

        void push_traits(::HIR::ItemPath p, const ::HIR::Module& mod) {
            auto sp = Span();
            m_mod_paths.push_back( p.get_simple_path() );
            DEBUG("Module has " << mod.m_traits.size() << " in-scope traits");
            // - Push a NULL entry to prevent parent module import lists being searched
            m_traits.push_back( ::std::make_pair(nullptr, nullptr) );
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("Push " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->m_crate.get_trait_by_path(sp, trait_path) ) );
            }
        }
        void pop_traits(const ::HIR::Module& mod) {
            DEBUG("Module has " << mod.m_traits.size() << " in-scope traits");
            for(unsigned int i = 0; i < mod.m_traits.size(); i ++ )
                m_traits.pop_back();
            m_traits.pop_back();
            m_mod_paths.pop_back();
        }
    };
}


typedef ::std::vector< ::std::pair<::HIR::Pattern, ::HIR::TypeRef> >    t_args;
// Needs to mutate the pattern
extern void Typecheck_Code(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
extern void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
extern void Typecheck_Code_Simple(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
