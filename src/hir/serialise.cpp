/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#include "hir.hpp"
#include "main_bindings.hpp"
#include <serialiser_texttree.hpp>
#include <macro_rules/macro_rules.hpp>
#include <mir/mir.hpp>

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
        void write_i64c(int64_t v) {
            // Convert from 2's completement
            bool sign = (v < 0);
            uint64_t va = (v < 0 ? -v : v);
            va <<= 1;
            va |= (sign ? 1 : 0);
            write_u64c(va);
        }
        void write_double(double v) {
            m_os.write(reinterpret_cast<const char*>(&v), sizeof v);
        }
        void write_tag(unsigned int t) {
            assert(t < 256);
            write_u8( static_cast<uint8_t>(t) );
        }
        void write_count(size_t c) {
            //DEBUG("c = " << c);
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
                write_u8( static_cast<uint8_t>(128 + (v.size() >> 16)) );
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
                DEBUG("- " << v.first);
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
        template<typename T>
        void serialise(const ::std::pair< ::std::string, T>& e) {
            write_string(e.first);
            serialise(e.second);
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
                DEBUG("-- Closure - " << ty);
                assert(!"Encountered closure type!");
                )
            )
        }
        void serialise_simplepath(const ::HIR::SimplePath& path)
        {
            //TRACE_FUNCTION_F("path="<<path);
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
            //TRACE_FUNCTION_F("path="<<path);
            serialise_simplepath(path.m_path);
            serialise_pathparams(path.m_params);
        }
        void serialise_traitpath(const ::HIR::TraitPath& path)
        {
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
                DEBUG("-- UfcsUnknown - " << path);
                assert(!"Unexpected UfcsUnknown");
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
                write_tag(0);
                ),
            (TypeLifetime,
                write_tag(1);
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
            
            write_count(crate.m_ext_crates.size());
            for(const auto& ext : crate.m_ext_crates)
                write_string(ext.first);
        }
        void serialise_module(const ::HIR::Module& mod)
        {
            TRACE_FUNCTION;
            
            // m_traits doesn't need to be serialised
            
            serialise_strmap(mod.m_value_items);
            serialise_strmap(mod.m_mod_items);
        }
        void serialise_typeimpl(const ::HIR::TypeImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            serialise_generics(impl.m_params);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                write_string(v.first);
                write_bool(v.second.is_pub);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                write_string(v.first);
                write_bool(v.second.is_pub);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise_traitimpl(const ::HIR::TraitImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " ?" << impl.m_trait_args << " for " << impl.m_type);
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);
            
            write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                DEBUG("fn " << v.first);
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                DEBUG("const " << v.first);
                write_string(v.first);
                write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            write_count(impl.m_types.size());
            for(const auto& v : impl.m_types) {
                DEBUG("type " << v.first);
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
            write_bool(impl.is_positive);
            serialise_type(impl.m_type);
        }
        
        void serialise(const ::HIR::TypeRef& ty) {
            serialise_type(ty);
        }
        void serialise(const ::HIR::SimplePath& p) {
            serialise_simplepath(p);
        }
        void serialise(const ::HIR::TraitPath& p) {
            serialise_traitpath(p);
        }
        void serialise(const ::std::string& v) {
            write_string(v);
        }

        void serialise(const ::MacroRulesPtr& mac)
        {
            serialise(*mac);
        }
        void serialise(const ::MacroRules& mac)
        {
            //m_exported: IGNORE, should be set
            serialise_vec(mac.m_rules);
            write_string(mac.m_source_crate);
        }
        void serialise(const ::MacroPatEnt& pe) {
            write_string(pe.name);
            write_count(pe.name_index);
            write_tag( static_cast<int>(pe.type) );
            if( pe.type == ::MacroPatEnt::PAT_TOKEN ) {
                serialise(pe.tok);
            }
            else if( pe.type == ::MacroPatEnt::PAT_LOOP ) {
                serialise(pe.tok);
                serialise_vec(pe.subpats);
            }
        }
        void serialise(const ::MacroRulesArm& arm) {
            serialise_vec(arm.m_param_names);
            serialise_vec(arm.m_pattern);
            serialise_vec(arm.m_contents);
        }
        void serialise(const ::MacroExpansionEnt& ent) {
            TU_MATCHA( (ent), (e),
            (Token,
                write_tag(0);
                serialise(e);
                ),
            (NamedValue,
                write_tag(1);
                write_u8(e >> 24);
                write_count(e & 0x00FFFFFF);
                ),
            (Loop,
                write_tag(2);
                serialise_vec(e.entries);
                serialise(e.joiner);
                // ::std::map<unsigned int,bool>
                write_count(e.variables.size());
                for(const auto& var : e.variables) {
                    write_count(var.first);
                    write_bool(var.second);
                }
                )
            )
        }
        void serialise(const ::Token& tok) {
            // HACK: Hand off to old serialiser code
            ::std::stringstream tmp;
            {
                Serialiser_TextTree ser(tmp);
                tok.serialise( ser );
            }
            
            write_string(tmp.str());
        }
        
        void serialise(const ::HIR::Literal& lit)
        {
            write_tag(lit.tag());
            TU_MATCHA( (lit), (e),
            (Invalid,
                //BUG(Span(), "Literal::Invalid encountered in HIR");
                ),
            (List,
                serialise_vec(e);
                ),
            (Variant,
                write_count(e.idx);
                serialise_vec(e.vals);
                ),
            (Integer,
                write_u64(e);
                ),
            (Float,
                write_double(e);
                ),
            (BorrowOf,
                serialise_path(e);
                ),
            (String,
                write_string(e);
                )
            )
        }
        
        void serialise(const ::HIR::ExprPtr& exp)
        {
            write_bool( (bool)exp.m_mir );
            if( exp.m_mir ) {
                serialise(*exp.m_mir);
            }
        }
        void serialise(const ::MIR::Function& mir)
        {
            // Write out MIR.
            serialise_vec( mir.named_variables );
            serialise_vec( mir.temporaries );
            serialise_vec( mir.blocks );
        }
        void serialise(const ::MIR::BasicBlock& block)
        {
            serialise_vec( block.statements );
            serialise(block.terminator);
        }
        void serialise(const ::MIR::Statement& stmt)
        {
            TU_MATCHA( (stmt), (e),
            (Assign,
                write_tag(0);
                serialise(e.dst);
                serialise(e.src);
                ),
            (Drop,
                write_tag(1);
                assert(e.kind == ::MIR::eDropKind::DEEP || e.kind == ::MIR::eDropKind::SHALLOW);
                write_bool(e.kind == ::MIR::eDropKind::DEEP);
                serialise(e.slot);
                )
            )
        }
        void serialise(const ::MIR::Terminator& term)
        {
            write_tag( static_cast<int>(term.tag()) );
            TU_MATCHA( (term), (e),
            (Incomplete,
                // NOTE: loops that diverge (don't break) leave a dangling bb
                //assert(!"Entountered Incomplete MIR block");
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                write_count(e);
                ),
            (Panic,
                write_count(e.dst);
                ),
            (If,
                serialise(e.cond);
                write_count(e.bb0);
                write_count(e.bb1);
                ),
            (Switch,
                serialise(e.val);
                write_count(e.targets.size());
                for(auto t : e.targets)
                    write_count(t);
                ),
            (Call,
                write_count(e.ret_block);
                write_count(e.panic_block);
                serialise(e.ret_val);
                serialise(e.fcn_val);
                serialise_vec(e.args);
                )
            )
        }
        void serialise(const ::MIR::LValue& lv)
        {
            TRACE_FUNCTION_F("LValue = "<<lv);
            write_tag( static_cast<int>(lv.tag()) );
            TU_MATCHA( (lv), (e),
            (Variable,
                write_count(e);
                ),
            (Temporary,
                write_count(e.idx);
                ),
            (Argument,
                write_count(e.idx);
                ),
            (Static,
                serialise_path(e);
                ),
            (Return,
                ),
            (Field,
                serialise(e.val);
                write_count(e.field_index);
                ),
            (Deref,
                serialise(e.val);
                ),
            (Index,
                serialise(e.val);
                serialise(e.idx);
                ),
            (Downcast,
                serialise(e.val);
                write_count(e.variant_index);
                )
            )
        }
        void serialise(const ::MIR::RValue& val)
        {
            TRACE_FUNCTION_F("RValue = "<<val);
            write_tag( val.tag() );
            TU_MATCHA( (val), (e),
            (Use,
                serialise(e);
                ),
            (Constant,
                serialise(e);
                ),
            (SizedArray,
                serialise(e.val);
                write_u64c(e.count);
                ),
            (Borrow,
                // TODO: Region?
                write_tag( static_cast<int>(e.type) );
                serialise(e.val);
                ),
            (Cast,
                serialise(e.val);
                serialise(e.type);
                ),
            (BinOp,
                serialise(e.val_l);
                write_tag( static_cast<int>(e.op) );
                serialise(e.val_r);
                ),
            (UniOp,
                serialise(e.val);
                write_tag( static_cast<int>(e.op) );
                ),
            (DstMeta,
                serialise(e.val);
                ),
            (MakeDst,
                serialise(e.ptr_val);
                serialise(e.meta_val);
                ),
            (Tuple,
                serialise_vec(e.vals);
                ),
            (Array,
                serialise_vec(e.vals);
                ),
            (Struct,
                serialise_genericpath(e.path);
                serialise_vec(e.vals);
                )
            )
        }
        void serialise(const ::MIR::Constant& v)
        {
            write_tag(v.tag());
            TU_MATCHA( (v), (e),
            (Int,
                write_i64c(e);
                ),
            (Uint,
                write_u64c(e);
                ),
            (Float,
                write_double(e);
                ),
            (Bool,
                write_bool(e);
                ),
            (Bytes,
                write_count(e.size());
                m_os.write( reinterpret_cast<const char*>(e.data()), e.size() );
                ),
            (StaticString,
                write_string(e);
                ),
            (Const,
                serialise_path(e.p);
                ),
            (ItemAddr,
                serialise_path(e);
                )
            )
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
                write_tag(3);
                serialise(e);
                ),
            (Struct,
                write_tag(4);
                serialise(e);
                ),
            (Trait,
                write_tag(5);
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
            TRACE_FUNCTION_F("_function:");
            
            write_tag( static_cast<int>(fcn.m_receiver) );
            write_string(fcn.m_abi);
            write_bool(fcn.m_unsafe);
            write_bool(fcn.m_const);
            
            serialise_generics(fcn.m_params);
            write_count(fcn.m_args.size());
            for(const auto& a : fcn.m_args)
                serialise(a.second);
            write_bool(fcn.m_variadic);
            serialise(fcn.m_return);
            DEBUG("m_args = " << fcn.m_args);
            
            serialise(fcn.m_code);
        }
        void serialise(const ::HIR::Constant& item)
        {
            TRACE_FUNCTION_F("_constant:");
            
            serialise_generics(item.m_params);
            serialise(item.m_type);
            serialise(item.m_value);
            serialise(item.m_value_res);
        }
        void serialise(const ::HIR::Static& item)
        {
            TRACE_FUNCTION_F("_static:");
            
            write_bool(item.m_is_mut);
            serialise(item.m_type);
            // NOTE: Omit the rest, not generic and emitted as part of the image.
        }
        
        // - Type items
        void serialise(const ::HIR::TypeAlias& ta)
        {
            serialise_generics(ta.m_params);
            serialise_type(ta.m_type);
        }
        void serialise(const ::HIR::Enum& ta)
        {
            serialise_generics(ta.m_params);
            write_tag( static_cast<int>(ta.m_repr) );
            serialise_vec( ta.m_variants );
        }
        void serialise(const ::HIR::Enum::Variant& v)
        {
            write_tag( v.tag() );
            TU_MATCHA( (v), (e),
            (Unit,
                ),
            (Value,
                // NOTE: e.expr skipped as it's not needed anymore
                serialise(e.val);
                ),
            (Tuple,
                serialise_vec(e);
                ),
            (Struct,
                serialise_vec(e);
                )
            )
        }

        void serialise(const ::HIR::Struct& item)
        {
            TRACE_FUNCTION;
            
            serialise_generics(item.m_params);
            write_tag( static_cast<int>(item.m_repr) );
            
            write_tag( item.m_data.tag() );
            TU_MATCHA( (item.m_data), (e),
            (Unit,
                ),
            (Tuple,
                serialise_vec(e);
                ),
            (Named,
                serialise_vec(e);
                )
            )
        }
        void serialise(const ::HIR::Trait& item)
        {
            TRACE_FUNCTION_F("_trait:");
            serialise_generics(item.m_params);
            //write_string(item.m_lifetime);    // TODO: Better type for lifetime
            serialise_vec( item.m_parent_traits );
            write_bool( item.m_is_marker );
            serialise_strmap( item.m_types );
            serialise_strmap( item.m_values );
        }
        void serialise(const ::HIR::TraitValueItem& tvi)
        {
            write_tag( tvi.tag() );
            TU_MATCHA( (tvi), (e),
            (Constant,
                DEBUG("Constant");
                serialise(e);
                ),
            (Static,
                DEBUG("Static");
                serialise(e);
                ),
            (Function,
                DEBUG("Function");
                serialise(e);
                )
            )
        }
        void serialise(const ::HIR::AssociatedType& at)
        {
            write_bool(at.is_sized);
            //write_string(at.m_lifetime_bound);  // TODO: better type for lifetime
            serialise_vec(at.m_trait_bounds);
            serialise_type(at.m_default);
        }
    };
}

void HIR_Serialise(const ::std::string& filename, const ::HIR::Crate& crate)
{
    ::std::ofstream out(filename);
    HirSerialiser  s { out };
    s.serialise_crate(crate);
}

