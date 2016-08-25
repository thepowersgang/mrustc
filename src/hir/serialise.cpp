/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#include "hir.hpp"
#include "main_bindings.hpp"

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
        void write_u32(uint32_t v) {
            write_u8(v & 0xFF);
            write_u8(v >>  8);
            write_u8(v >> 16);
            write_u8(v >> 24);
        }
        void write_u64(uint64_t v) {
            write_u8(v & 0xFF);
            write_u8(v >>  8);
            write_u8(v >> 16);
            write_u8(v >> 24);
            write_u8(v >> 32);
            write_u8(v >> 40);
            write_u8(v >> 48);
            write_u8(v >> 56);
        }
        // Variable-length encoded u64 (for array sizes)
        void write_u64c(uint64_t v) {
            if( v < (1<<7) ) {
                write_u8(v);
            }
            else if( v < (1<<(6+16)) ) {
                write_u8( 0x80 + (v >> 16)); // 0x80 -- 0xB0
                write_u8(v >> 8);
                write_u8(v);
            }
            else if( v < (1ul << (5 + 32)) ) {
                write_u8( 0xC0 + (v >> 32)); // 0x80 -- 0xB0
                write_u8(v >> 24);
                write_u8(v >> 16);
                write_u8(v >> 8);
                write_u8(v);
            }
            else {
                write_u8(0xFF);
                write_u64(v);
            }
        }
        void write_tag(unsigned int t) {
            assert(t < 256);
            write_u8( static_cast<uint8_t>(t) );
        }
        void write_count(size_t c) {
            DEBUG("c = " << c);
            if(c < 0xFE) {
                write_u8( static_cast<uint8_t>(c) );
            }
            else if( c == ~0u ) {
                write_u8( 0xFF );
            }
            else {
                assert(c < (1u<<16));
                write_u8( 0xFE );
                write_u16( static_cast<uint16_t>(c) );
            }
        }
        void write_string(const ::std::string& v) {
            if(v.size() < 128) {
                write_u8( static_cast<uint8_t>(v.size()) );
            }
            else {
                assert(v.size() < (1u<<(16+7)));
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
        void serialise_vec(const ::std::vector<T>& vec)
        {
            write_count(vec.size());
            for(const auto& i : vec)
                serialise(i);
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
            write_tag( ty.m_data.tag() );
            TU_MATCHA( (ty.m_data), (e),
            (Infer,
                // BAAD
                ),
            (Diverge,
                ),
            (Primitive,
                write_tag( static_cast<int>(e) );
                ),
            (Path,
                serialise_path(e.path);
                ),
            (Generic,
                write_string(e.name);
                write_u16(e.binding);
                ),
            (TraitObject,
                serialise_traitpath(e.m_trait);
                write_count(e.m_markers.size());
                for(const auto& m : e.m_markers)
                    serialise_genericpath(m);
                //write_string(e.lifetime); // TODO: Need a better type
                ),
            (Array,
                assert(e.size_val != ~0u);
                serialise_type(*e.inner);
                write_u64c(e.size_val);
                ),
            (Slice,
                serialise_type(*e.inner);
                ),
            (Tuple,
                write_count(e.size());
                for(const auto& st : e)
                    serialise_type(st);
                ),
            (Borrow,
                write_tag(static_cast<int>(e.type));
                serialise_type(*e.inner);
                ),
            (Pointer,
                write_tag(static_cast<int>(e.type));
                serialise_type(*e.inner);
                ),
            (Function,
                write_bool(e.is_unsafe);
                write_string(e.m_abi);
                serialise_type(*e.m_rettype);
                serialise_vec(e.m_arg_types);
                ),
            (Closure,
                throw "";
                )
            )
        }
        void serialise_simplepath(const ::HIR::SimplePath& path)
        {
            TRACE_FUNCTION_F("path="<<path);
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
            TRACE_FUNCTION_F("path="<<path);
            serialise_simplepath(path.m_path);
            serialise_pathparams(path.m_params);
        }
        void serialise_traitpath(const ::HIR::TraitPath& path)
        {
            TRACE_FUNCTION_F("path="<<path);
            serialise_genericpath(path.m_path);
            // TODO: Lifetimes? (m_hrls)
            serialise_strmap(path.m_type_bounds);
        }
        void serialise_path(const ::HIR::Path& path)
        {
            TRACE_FUNCTION_F("path="<<path);
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
            DEBUG("params = " << params.fmt_args() << ", " << params.fmt_bounds());
            serialise_vec(params.m_types);
            serialise_vec(params.m_lifetimes);
            serialise_vec(params.m_bounds);
        }
        void serialise(const ::HIR::TypeParamDef& pd) {
            write_string(pd.m_name);
            serialise_type(pd.m_default);
            write_bool(pd.m_is_sized);
        }
        void serialise(const ::HIR::GenericBound& b) {
            TU_MATCHA( (b), (e),
            (Lifetime,
                //write_tag(0);
                ),
            (TypeLifetime,
                //write_tag(1);
                ),
            (TraitBound,
                write_tag(2);
                serialise_type(e.type);
                serialise_traitpath(e.trait);
                ),
            (TypeEquality,
                write_tag(3);
                serialise_type(e.type);
                serialise_type(e.other_type);
                )
            )
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
        void serialise(const ::std::string& v) {
            write_string(v);
        }

        void serialise(const ::MacroRules& mac)
        {
            //m_exported: IGNORE, should be set
            serialise(mac.m_pattern);
            serialise_vec(mac.m_rules);
        }
        void serialise(const ::MacroRulesPatFrag& pat) {
            serialise_vec(pat.m_pats_ents);
            write_count(pat.m_pattern_end);
            serialise_vec(pat.m_next_frags);
        }
        void serialise(const ::MacroPatEnt& pe) {
            write_string(pe.name);
            write_count(pe.name_index);
            serialise(pe.tok);
            serialise_vec(pe.subpats);
            write_tag( static_cast<int>(pe.type) );
        }
        void serialise(const ::MacroRulesArm& arm) {
            serialise_vec(arm.m_param_names);
            serialise_vec(arm.m_contents);
        }
        void serialise(const ::MacroExpansionEnt& ent) {
            TU_MATCHA( (ent), (e),
            (Token,
                write_tag(0);
                serialise(e);
                ),
            (NamedValue,
                DEBUG("NamedValue " << e);
                write_tag(1);
                write_u8(e >> 24);
                write_count(e & 0x00FFFFFF);
                ),
            (Loop,
                write_tag(2);
                serialise_vec(e.entries);
                serialise(e.joiner);
                // ::std::set<unsigned int>
                write_count(e.variables.size());
                for(const auto& var : e.variables)
                    write_count(var);
                )
            )
        }
        void serialise(const ::Token& tok) {
            // TODO
            write_tag(tok.type());
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

