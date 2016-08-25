/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#include "hir.hpp"

namespace {
    class HirSerialiser
    {
        ::std::ostream& m_os;
    public:
        HirSerialiser(::std::ostream& os):
            m_os(os)
        {}
        
        void write_u8(uint8_t v) {
            m_os.write(reinterpret_cast<const char*>(&v), 1);
        }
        void write_u16(uint16_t v) {
            write_u8(v & 0xFF);
            write_u8(v >> 8);
        }
        void write_tag(unsigned int t) {
            assert(t < 256);
            write_u8( static_cast<uint8_t>(t) );
        }
        void write_count(size_t c) {
            if(c < 255) {
                write_u8( static_cast<uint8_t>(c) );
            }
            else {
                assert(c < (1<<16));
                write_u16( static_cast<uint16_t>(c) );
            }
        }
        void write_string(const ::std::string& v) {
            if(v.size() < 128) {
                write_u8( static_cast<uint8_t>(v.size()) );
            }
            else {
                assert(v.size() < (1<<(16+7)));
                write_u16( static_cast<uint8_t>(128 + (v.size() >> 16)) );
                write_u16( static_cast<uint16_t>(v.size() & 0xFFFF) );
            }
            m_os.write(v.data(), v.size());
        }
        void write_bool(bool v) {
            write_u8(v ? 0xFF : 0x00);
        }
        
        template<typename V>
        void serialise_strmap(const ::std::map< ::std::string,V>& map)
        {
            write_count(map.size());
            for(const auto& v : map) {
                write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::unordered_map< ::std::string,V>& map)
        {
            write_count(map.size());
            for(const auto& v : map) {
                write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename T>
        void serialise(const ::HIR::VisEnt<T>& e)
        {
            write_bool(e.is_public);
            serialise(e.ent);
        }
        template<typename T>
        void serialise(const ::std::unique_ptr<T>& e) {
            serialise(*e);
        }
        
        void serialise_type(const ::HIR::TypeRef& ty)
        {
            // TODO:
        }
        void serialise_simplepath(const ::HIR::SimplePath& path)
        {
            write_string(path.m_crate_name);
            write_count(path.m_components.size());
            for(const auto& c : path.m_components)
                write_string(c);
        }
        void serialise_pathparams(const ::HIR::PathParams& pp)
        {
            write_count(pp.m_types.size());
            for(const auto& t : pp.m_types)
                serialise_type(t);
        }
        void serialise_genericpath(const ::HIR::GenericPath& path)
        {
            serialise_simplepath(path.m_path);
            serialise_pathparams(path.m_params);
        }
        void serialise_path(const ::HIR::Path& path)
        {
            TU_MATCHA( (path.m_data), (e),
            (Generic,
                write_tag(0);
                serialise_genericpath(e);
                ),
            (UfcsInherent,
                write_tag(1);
                serialise_type(*e.type);
                write_string(e.item);
                serialise_pathparams(e.params);
                ),
            (UfcsKnown,
                write_tag(2);
                serialise_type(*e.type);
                serialise_genericpath(e.trait);
                write_string(e.item);
                serialise_pathparams(e.params);
                ),
            (UfcsUnknown,
                throw "Unexpected UfcsUnknown";
                )
            )
        }

        void serialise_generics(const ::HIR::GenericParams& params)
        {
            // TODO:
        }
        
        
        void serialise_crate(const ::HIR::Crate& crate)
        {
            serialise_module(crate.m_root_module);
            
            write_count(crate.m_type_impls.size());
            for(const auto& impl : crate.m_type_impls) {
                serialise_typeimpl(impl);
            }
            write_count(crate.m_trait_impls.size());
            for(const auto& tr_impl : crate.m_trait_impls) {
                serialise_simplepath(tr_impl.first);
                serialise_traitimpl(tr_impl.second);
            }
            write_count(crate.m_marker_impls.size());
            for(const auto& tr_impl : crate.m_marker_impls) {
                serialise_simplepath(tr_impl.first);
                serialise_markerimpl(tr_impl.second);
            }
            
            serialise_strmap(crate.m_exported_macros);
            serialise_strmap(crate.m_lang_items);
        }
        void serialise_module(const ::HIR::Module& mod)
        {
            // m_traits doesn't need to be serialised
            
            serialise_strmap(mod.m_value_items);
            serialise_strmap(mod.m_mod_items);
        }
        void serialise_typeimpl(const ::HIR::TypeImpl& impl)
        {
            serialise_generics(impl.m_params);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                write_string(v.first);
                write_bool(v.second.is_pub);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise_traitimpl(const ::HIR::TraitImpl& impl)
        {
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_types.size());
            for(const auto& v : impl.m_types) {
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise_markerimpl(const ::HIR::MarkerImpl& impl)
        {
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);
        }
        
        void serialise(const ::HIR::TypeRef& ty) {
            serialise_type(ty);
        }
        void serialise(const ::HIR::SimplePath& p) {
            serialise_simplepath(p);
        }

        void serialise(const ::MacroRules& mac)
        {
            // TODO:
        }
        void serialise(const ::HIR::ExprPtr& exp)
        {
            // Write out MIR.
            // TODO
        }
        
        void serialise(const ::HIR::TypeItem& item)
        {
            TU_MATCHA( (item), (e),
            (Import,
                write_tag(0);
                serialise_simplepath(e);
                ),
            (Module,
                write_tag(1);
                serialise_module(e);
                ),
            (TypeAlias,
                write_tag(2);
                serialise(e);
                ),
            (Enum,
                write_tag(2);
                serialise(e);
                ),
            (Struct,
                write_tag(3);
                serialise(e);
                ),
            (Trait,
                write_tag(4);
                serialise(e);
                )
            )
        }
        void serialise(const ::HIR::ValueItem& item)
        {
            TU_MATCHA( (item), (e),
            (Import,
                write_tag(0);
                serialise_simplepath(e);
                ),
            (Constant,
                write_tag(1);
                serialise(e);
                ),
            (Static,
                write_tag(2);
                serialise(e);
                ),
            (StructConstant,
                write_tag(3);
                serialise_simplepath(e.ty);
                ),
            (Function,
                write_tag(4);
                serialise(e);
                ),
            (StructConstructor,
                write_tag(5);
                serialise_simplepath(e.ty);
                )
            )
        }
        
        // - Value items
        void serialise(const ::HIR::Function& fcn)
        {
            // TODO
        }
        void serialise(const ::HIR::Constant& con)
        {
            // TODO
        }
        void serialise(const ::HIR::Static& con)
        {
            // TODO
        }
        
        // - Type items
        void serialise(const ::HIR::TypeAlias& ta)
        {
            // TODO
        }
        void serialise(const ::HIR::Enum& ta)
        {
            // TODO
        }
        void serialise(const ::HIR::Struct& ta)
        {
            // TODO
        }
        void serialise(const ::HIR::Trait& ta)
        {
            // TODO
        }
    };
}

void HIR_Serialise(const ::std::string& filename, const ::HIR::Crate& crate)
{
    ::std::ofstream out(filename);
    HirSerialiser  s { out };
    s.serialise_crate(crate);
}

