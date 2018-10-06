/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_visit.hpp
 * - Helpers for the HIR typecheck expression visiting
 */

namespace typeck {
    struct ModuleState
    {
        const ::HIR::Crate& m_crate;

        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;

        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;

        ModuleState(const ::HIR::Crate& crate):
            m_crate(crate),
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
        NullOnDrop< ::HIR::GenericParams> set_impl_generics(::HIR::GenericParams& gps) {
            assert( !m_impl_generics );
            m_impl_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_impl_generics);
        }
        NullOnDrop< ::HIR::GenericParams> set_item_generics(::HIR::GenericParams& gps) {
            assert( !m_item_generics );
            m_item_generics = &gps;
            return NullOnDrop< ::HIR::GenericParams>(m_item_generics);
        }

        void push_traits(const ::HIR::Module& mod) {
            auto sp = Span();
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
        }
    };
}


typedef ::std::vector< ::std::pair<::HIR::Pattern, ::HIR::TypeRef> >    t_args;
// Needs to mutate the pattern
extern void Typecheck_Code(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
extern void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
extern void Typecheck_Code_Simple(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr);
