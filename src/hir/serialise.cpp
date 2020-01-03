/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise.cpp
 * - HIR (De)Serialisation for crate metadata
 */
#include "hir.hpp"
#include "main_bindings.hpp"
#include <macro_rules/macro_rules.hpp>
#include <mir/mir.hpp>
#include "serialise_lowlevel.hpp"

//namespace {
    class HirSerialiser
    {
        ::HIR::serialise::Writer&   m_out;
    public:
        HirSerialiser(::HIR::serialise::Writer& out):
            m_out( out )
        {}

        template<typename V>
        void serialise_strmap(const ::std::map<RcString,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::map< ::std::string,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_pathmap(const ::std::map< ::HIR::SimplePath,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                DEBUG("- " << v.first);
                serialise(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::unordered_map<RcString,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                DEBUG("- " << v.first);
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::unordered_map< ::std::string,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                DEBUG("- " << v.first);
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::unordered_multimap<RcString,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                DEBUG("- " << v.first);
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename V>
        void serialise_strmap(const ::std::unordered_multimap< ::std::string,V>& map)
        {
            m_out.write_count(map.size());
            for(const auto& v : map) {
                DEBUG("- " << v.first);
                m_out.write_string(v.first);
                serialise(v.second);
            }
        }
        template<typename T>
        void serialise_vec(const ::std::vector<T>& vec)
        {
            TRACE_FUNCTION_F("size=" << vec.size());
            m_out.write_count(vec.size());
            for(const auto& i : vec)
                serialise(i);
        }
        template<typename T>
        void serialise(const ::std::vector<T>& vec)
        {
            serialise_vec(vec);
        }
        template<typename T>
        void serialise(const ::HIR::VisEnt<T>& e)
        {
            m_out.write_bool(e.publicity.is_global());  // At this stage, we only care if the item is visible outside the crate or not
            serialise(e.ent);
        }
        template<typename T>
        void serialise(const ::std::unique_ptr<T>& e) {
            serialise(*e);
        }
        template<typename T>
        void serialise(const ::std::pair< ::std::string, T>& e) {
            m_out.write_string(e.first);
            serialise(e.second);
        }
        template<typename T>
        void serialise(const ::std::pair< RcString, T>& e) {
            m_out.write_string(e.first);
            serialise(e.second);
        }
        template<typename T>
        void serialise(const ::std::pair<unsigned int, T>& e) {
            m_out.write_count(e.first);
            serialise(e.second);
        }
        //void serialise(::MIR::BasicBlockId val) {
        //    m_out.write_count(val);
        //}

        void serialise(bool v) { m_out.write_bool(v); };
        void serialise(unsigned int v) { m_out.write_count(v); };
        void serialise(uint64_t v) { m_out.write_u64c(v); };
        void serialise(int64_t v) { m_out.write_i64c(v); };

        void serialise(const ::HIR::LifetimeDef& ld)
        {
            m_out.write_string(ld.m_name);
        }
        void serialise(const ::HIR::LifetimeRef& lr)
        {
            m_out.write_count(lr.binding);
        }
        void serialise_type(const ::HIR::TypeRef& ty)
        {
            m_out.write_tag( ty.m_data.tag() );
            TU_MATCHA( (ty.m_data), (e),
            (Infer,
                // BAAD
                ),
            (Diverge,
                ),
            (Primitive,
                m_out.write_tag( static_cast<int>(e) );
                ),
            (Path,
                serialise_path(e.path);
                ),
            (Generic,
                m_out.write_string(e.name);
                m_out.write_u16(e.binding);
                ),
            (TraitObject,
                serialise_traitpath(e.m_trait);
                m_out.write_count(e.m_markers.size());
                for(const auto& m : e.m_markers)
                    serialise_genericpath(m);
                serialise(e.m_lifetime);
                ),
            (ErasedType,
                serialise_path(e.m_origin);
                m_out.write_count(e.m_index);

                m_out.write_count(e.m_traits.size());
                for(const auto& t : e.m_traits)
                    serialise_traitpath(t);
                serialise(e.m_lifetime);
                ),
            (Array,
                assert(e.size_val != ~0u);
                serialise_type(*e.inner);
                m_out.write_u64c(e.size_val);
                ),
            (Slice,
                serialise_type(*e.inner);
                ),
            (Tuple,
                m_out.write_count(e.size());
                for(const auto& st : e)
                    serialise_type(st);
                ),
            (Borrow,
                serialise(e.lifetime);
                m_out.write_tag(static_cast<int>(e.type));
                serialise_type(*e.inner);
                ),
            (Pointer,
                m_out.write_tag(static_cast<int>(e.type));
                serialise_type(*e.inner);
                ),
            (Function,
                m_out.write_bool(e.is_unsafe);
                m_out.write_string(e.m_abi);
                serialise_type(*e.m_rettype);
                serialise_vec(e.m_arg_types);
                ),
            (Closure,
                DEBUG("-- Closure - " << ty);
                BUG(Span(), "Encountered closure type when serialising - " << ty);
                )
            )
        }
        void serialise_simplepath(const ::HIR::SimplePath& path)
        {
            TRACE_FUNCTION_F(path);
            m_out.write_string(path.m_crate_name);
            serialise_vec(path.m_components);
        }
        void serialise_pathparams(const ::HIR::PathParams& pp)
        {
            m_out.write_count(pp.m_types.size());
            for(const auto& t : pp.m_types)
                serialise_type(t);
        }
        void serialise_genericpath(const ::HIR::GenericPath& path)
        {
            TRACE_FUNCTION_F(path);
            serialise_simplepath(path.m_path);
            serialise_pathparams(path.m_params);
        }
        void serialise(const ::HIR::GenericPath& path) { serialise_genericpath(path); }
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
                m_out.write_tag(0);
                serialise_genericpath(e);
                ),
            (UfcsInherent,
                m_out.write_tag(1);
                serialise_type(*e.type);
                m_out.write_string(e.item);
                serialise_pathparams(e.params);
                serialise_pathparams(e.impl_params);
                ),
            (UfcsKnown,
                m_out.write_tag(2);
                serialise_type(*e.type);
                serialise_genericpath(e.trait);
                m_out.write_string(e.item);
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
            m_out.write_string(pd.m_name);
            serialise_type(pd.m_default);
            m_out.write_bool(pd.m_is_sized);
        }
        void serialise(const ::HIR::GenericBound& b) {
            TRACE_FUNCTION_F(b);
            TU_MATCHA( (b), (e),
            (Lifetime,
                m_out.write_tag(0);
                ),
            (TypeLifetime,
                m_out.write_tag(1);
                ),
            (TraitBound,
                m_out.write_tag(2);
                serialise_type(e.type);
                serialise_traitpath(e.trait);
                ),
            (TypeEquality,
                m_out.write_tag(3);
                serialise_type(e.type);
                serialise_type(e.other_type);
                )
            )
        }


        void serialise(const ::HIR::ProcMacro& pm)
        {
            TRACE_FUNCTION_F("pm = ProcMacro { " << pm.name << ", " << pm.path << ", [" << pm.attributes << "] }");
            serialise(pm.name);
            serialise(pm.path);
            serialise_vec(pm.attributes);
        }
        template<typename T>
        void serialise(const ::HIR::Crate::ImplGroup<T>& ig)
        {
            serialise_pathmap(ig.named);
            serialise_vec(ig.non_named);
            serialise_vec(ig.generic);
        }
        void serialise(const ::HIR::Crate::MacroImport& e)
        {
            serialise(e.path);
        }

        void serialise_crate(const ::HIR::Crate& crate)
        {
            m_out.write_string(crate.m_crate_name);
            serialise_module(crate.m_root_module);

            serialise(crate.m_type_impls);
            serialise_pathmap(crate.m_trait_impls);
            serialise_pathmap(crate.m_marker_impls);

            serialise_strmap(crate.m_exported_macros);
            serialise_strmap(crate.m_proc_macro_reexports);
            {
                decltype(crate.m_lang_items)    lang_items_filtered;
                for(const auto& ent : crate.m_lang_items)
                {
                    if(ent.second.m_crate_name == "" || ent.second.m_crate_name == crate.m_crate_name)
                    {
                        lang_items_filtered.insert(ent);
                    }
                }
                serialise_strmap(lang_items_filtered);
            }

            m_out.write_count(crate.m_ext_crates.size());
            for(const auto& ext : crate.m_ext_crates)
            {
                m_out.write_string(ext.first);
                //m_out.write_string(ext.second.m_basename);
                m_out.write_string(ext.second.m_path);
            }
            serialise_vec(crate.m_ext_libs);
            serialise_vec(crate.m_link_paths);

            serialise_vec(crate.m_proc_macros);
        }
        void serialise(const ::HIR::ExternLibrary& lib)
        {
            m_out.write_string(lib.name);
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

            m_out.write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                m_out.write_string(v.first);
                m_out.write_bool(v.second.publicity.is_global());
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            m_out.write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                m_out.write_string(v.first);
                m_out.write_bool(v.second.publicity.is_global());
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise(const ::HIR::TypeImpl& impl)
        {
            serialise_typeimpl(impl);
        }
        void serialise_traitimpl(const ::HIR::TraitImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " ?" << impl.m_trait_args << " for " << impl.m_type);
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            serialise_type(impl.m_type);

            m_out.write_count(impl.m_methods.size());
            for(const auto& v : impl.m_methods) {
                DEBUG("fn " << v.first);
                m_out.write_string(v.first);
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            m_out.write_count(impl.m_constants.size());
            for(const auto& v : impl.m_constants) {
                DEBUG("const " << v.first);
                m_out.write_string(v.first);
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            m_out.write_count(impl.m_statics.size());
            for(const auto& v : impl.m_statics) {
                DEBUG("static " << v.first);
                m_out.write_string(v.first);
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            m_out.write_count(impl.m_types.size());
            for(const auto& v : impl.m_types) {
                DEBUG("type " << v.first);
                m_out.write_string(v.first);
                m_out.write_bool(v.second.is_specialisable);
                serialise(v.second.data);
            }
            // m_src_module doesn't matter after typeck
        }
        void serialise(const ::HIR::TraitImpl& impl)
        {
            serialise_traitimpl(impl);
        }
        void serialise_markerimpl(const ::HIR::MarkerImpl& impl)
        {
            serialise_generics(impl.m_params);
            serialise_pathparams(impl.m_trait_args);
            m_out.write_bool(impl.is_positive);
            serialise_type(impl.m_type);
        }
        void serialise(const ::HIR::MarkerImpl& impl)
        {
            serialise_markerimpl(impl);
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
            m_out.write_string(v);
        }
        void serialise(const RcString& v) {
            m_out.write_string(v);
        }

        void serialise(const ::MacroRulesPtr& mac)
        {
            serialise(*mac);
        }
        void serialise(const ::MacroRules& mac)
        {
            //m_exported: IGNORE, should be set
            assert(mac.m_rules.size() > 0);
            serialise_vec(mac.m_rules);
            m_out.write_string(mac.m_source_crate);
        }
        void serialise(const ::MacroPatEnt& pe) {
            m_out.write_string(pe.name);
            m_out.write_count(pe.name_index);
            m_out.write_tag( static_cast<int>(pe.type) );
            if( pe.type == ::MacroPatEnt::PAT_TOKEN ) {
                serialise(pe.tok);
            }
            else if( pe.type == ::MacroPatEnt::PAT_LOOP ) {
                serialise(pe.tok);
                serialise_vec(pe.subpats);
            }
        }
        void serialise(const ::SimplePatIfCheck& e) {
            m_out.write_tag( static_cast<int>(e.ty) );
            serialise(e.tok);
        }
        void serialise(const ::SimplePatEnt& pe) {
            m_out.write_tag( pe.tag() );
            TU_MATCH_HDRA( (pe), { )
            TU_ARMA(End, _e) {}
            TU_ARMA(LoopStart, _e) {}
            TU_ARMA(LoopNext, _e) {}
            TU_ARMA(LoopEnd, _e) {}
            TU_ARMA(Jump, e) {
                m_out.write_count(e.jump_target);
                }
            TU_ARMA(ExpectTok, e) {
                serialise(e);
                }
            TU_ARMA(ExpectPat, e) {
                m_out.write_tag( static_cast<int>(e.type) );
                m_out.write_count(e.idx);
                }
            TU_ARMA(If, e) {
                m_out.write_bool(e.is_equal);
                m_out.write_count(e.jump_target);
                serialise_vec(e.ents);
                }
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
                m_out.write_tag(0);
                serialise(e);
                ),
            (NamedValue,
                m_out.write_tag(1);
                m_out.write_u8(e >> 24);
                m_out.write_count(e & 0x00FFFFFF);
                ),
            (Loop,
                m_out.write_tag(2);
                serialise_vec(e.entries);
                serialise(e.joiner);
                // ::std::map<unsigned int,bool>
                m_out.write_count(e.variables.size());
                for(const auto& var : e.variables) {
                    m_out.write_count(var.first);
                    m_out.write_bool(var.second);
                }
                )
            )
        }
        void serialise(const ::Token& tok) {
            m_out.write_tag(tok.m_type);
            serialise(tok.m_data);
            // TODO: Position information.
        }
        void serialise(const ::Token::Data& td) {
            m_out.write_tag(td.tag());
            switch(td.tag())
            {
            case ::Token::Data::TAGDEAD:    throw "";
            TU_ARM(td, None, _e) {
                } break;
            TU_ARM(td, String, e) {
                m_out.write_string(e);
                } break;
            TU_ARM(td, IString, e) {
                m_out.write_string(e);
                } break;
            TU_ARM(td, Integer, e) {
                m_out.write_tag(e.m_datatype);
                m_out.write_u64c(e.m_intval);
                } break;
            TU_ARM(td, Float, e) {
                m_out.write_tag(e.m_datatype);
                m_out.write_double(e.m_floatval);
                } break;
            TU_ARM(td, Fragment, e)
                assert(!"Serialising interpolated macro fragment");
            }
        }

        void serialise(const ::HIR::Literal& lit)
        {
            m_out.write_tag(lit.tag());
            TU_MATCHA( (lit), (e),
            (Invalid,
                //BUG(Span(), "Literal::Invalid encountered in HIR");
                ),
            (Defer,
                ),
            (List,
                serialise_vec(e);
                ),
            (Variant,
                m_out.write_count(e.idx);
                serialise(*e.val);
                ),
            (Integer,
                m_out.write_u64(e);
                ),
            (Float,
                m_out.write_double(e);
                ),
            (BorrowPath,
                serialise_path(e);
                ),
            (BorrowData,
                serialise(*e);
                ),
            (String,
                m_out.write_string(e);
                )
            )
        }

        void serialise(const ::HIR::ExprPtr& exp, bool save_mir=true)
        {
            m_out.write_bool( (bool)exp.m_mir && save_mir );
            if( exp.m_mir && save_mir ) {
                serialise(*exp.m_mir);
            }
            serialise_vec( exp.m_erased_types );
        }
        void serialise(const ::MIR::Function& mir)
        {
            // Write out MIR.
            serialise_vec( mir.locals );
            //serialise_vec( mir.slot_names );
            serialise_vec( mir.drop_flags );
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
                m_out.write_tag(0);
                serialise(e.dst);
                serialise(e.src);
                ),
            (Drop,
                m_out.write_tag(1);
                assert(e.kind == ::MIR::eDropKind::DEEP || e.kind == ::MIR::eDropKind::SHALLOW);
                m_out.write_bool(e.kind == ::MIR::eDropKind::DEEP);
                serialise(e.slot);
                m_out.write_count(e.flag_idx);
                ),
            (Asm,
                m_out.write_tag(2);
                m_out.write_string(e.tpl);
                serialise_vec(e.outputs);
                serialise_vec(e.inputs);
                serialise_vec(e.clobbers);
                serialise_vec(e.flags);
                ),
            (SetDropFlag,
                m_out.write_tag(3);
                m_out.write_count(e.idx);
                m_out.write_bool(e.new_val);
                m_out.write_count(e.other);
                ),
            (ScopeEnd,
                m_out.write_tag(4);
                serialise_vec(e.slots);
                )
            )
        }
        void serialise(const ::MIR::Terminator& term)
        {
            m_out.write_tag( static_cast<int>(term.tag()) );
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
                m_out.write_count(e);
                ),
            (Panic,
                m_out.write_count(e.dst);
                ),
            (If,
                serialise(e.cond);
                m_out.write_count(e.bb0);
                m_out.write_count(e.bb1);
                ),
            (Switch,
                serialise(e.val);
                m_out.write_count(e.targets.size());
                for(auto t : e.targets)
                    m_out.write_count(t);
                ),
            (SwitchValue,
                serialise(e.val);
                m_out.write_count(e.def_target);
                serialise_vec(e.targets);
                serialise(e.values);
                ),
            (Call,
                m_out.write_count(e.ret_block);
                m_out.write_count(e.panic_block);
                serialise(e.ret_val);
                serialise(e.fcn);
                serialise_vec(e.args);
                )
            )
        }
        void serialise(const ::MIR::SwitchValues& sv)
        {
            m_out.write_tag( static_cast<int>(sv.tag()) );
            TU_MATCHA( (sv), (e),
            (Unsigned,
                serialise_vec(e);
                ),
            (Signed,
                serialise_vec(e);
                ),
            (String,
                serialise_vec(e);
                )
            )
        }
        void serialise(const ::MIR::CallTarget& ct)
        {
            m_out.write_tag( static_cast<int>(ct.tag()) );
            TU_MATCHA( (ct), (e),
            (Value,
                serialise(e);
                ),
            (Path,
                serialise_path(e);
                ),
            (Intrinsic,
                m_out.write_string(e.name);
                serialise_pathparams(e.params);
                )
            )
        }
        void serialise(const ::MIR::Param& p)
        {
            TRACE_FUNCTION_F("Param = "<<p);
            m_out.write_tag( static_cast<int>(p.tag()) );
            TU_MATCHA( (p), (e),
            (LValue, serialise(e);),
            (Constant, serialise(e);)
            )
        }
        void serialise(const ::MIR::LValue& lv)
        {
            TRACE_FUNCTION_F("LValue = "<<lv);
            if( lv.m_root.is_Static() ) {
                m_out.write_count(3);
                serialise_path(lv.m_root.as_Static());
            }
            else {
                m_out.write_count( lv.m_root.get_inner() );
            }
            serialise_vec(lv.m_wrappers);
        }
        void serialise(const ::MIR::LValue::Wrapper& w)
        {
            m_out.write_count(w.get_inner());
        }
        void serialise(const ::MIR::RValue& val)
        {
            TRACE_FUNCTION_F("RValue = "<<val);
            m_out.write_tag( val.tag() );
            TU_MATCHA( (val), (e),
            (Use,
                serialise(e);
                ),
            (Constant,
                serialise(e);
                ),
            (SizedArray,
                serialise(e.val);
                m_out.write_u64c(e.count);
                ),
            (Borrow,
                // TODO: Region?
                m_out.write_tag( static_cast<int>(e.type) );
                serialise(e.val);
                ),
            (Cast,
                serialise(e.val);
                serialise(e.type);
                ),
            (BinOp,
                serialise(e.val_l);
                m_out.write_tag( static_cast<int>(e.op) );
                serialise(e.val_r);
                ),
            (UniOp,
                serialise(e.val);
                m_out.write_tag( static_cast<int>(e.op) );
                ),
            (DstMeta,
                serialise(e.val);
                ),
            (DstPtr,
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
            (Variant,
                serialise_genericpath(e.path);
                m_out.write_count(e.index);
                serialise(e.val);
                ),
            (Struct,
                serialise_genericpath(e.path);
                serialise_vec(e.vals);
                )
            )
        }
        void serialise(const ::MIR::Constant& v)
        {
            m_out.write_tag(v.tag());
            TU_MATCHA( (v), (e),
            (Int,
                m_out.write_i64c(e.v);
                m_out.write_tag(static_cast<unsigned>(e.t));
                ),
            (Uint,
                m_out.write_u64c(e.v);
                m_out.write_tag(static_cast<unsigned>(e.t));
                ),
            (Float,
                m_out.write_double(e.v);
                m_out.write_tag(static_cast<unsigned>(e.t));
                ),
            (Bool,
                m_out.write_bool(e.v);
                ),
            (Bytes,
                m_out.write_count(e.size());
                m_out.write( e.data(), e.size() );
                ),
            (StaticString,
                m_out.write_string(e);
                ),
            (Const,
                serialise_path(*e.p);
                ),
            (Generic,
                m_out.write_string(e.name);
                m_out.write_count(e.binding);
                ),
            (ItemAddr,
                serialise_path(*e);
                )
            )
        }

        void serialise(const ::HIR::TypeItem& item)
        {
            TU_MATCHA( (item), (e),
            (Import,
                m_out.write_tag(0);
                serialise_simplepath(e.path);
                m_out.write_bool(e.is_variant);
                m_out.write_count(e.idx);
                ),
            (Module,
                m_out.write_tag(1);
                serialise_module(e);
                ),
            (TypeAlias,
                m_out.write_tag(2);
                serialise(e);
                ),
            (Enum,
                m_out.write_tag(3);
                serialise(e);
                ),
            (Struct,
                m_out.write_tag(4);
                serialise(e);
                ),
            (Trait,
                m_out.write_tag(5);
                serialise(e);
                ),
            (Union,
                m_out.write_tag(6);
                serialise(e);
                ),
            (ExternType,
                m_out.write_tag(7);
                serialise(e);
                )
            )
        }
        void serialise(const ::HIR::ValueItem& item)
        {
            TU_MATCHA( (item), (e),
            (Import,
                m_out.write_tag(0);
                serialise_simplepath(e.path);
                m_out.write_bool(e.is_variant);
                m_out.write_count(e.idx);
                ),
            (Constant,
                m_out.write_tag(1);
                serialise(e);
                ),
            (Static,
                m_out.write_tag(2);
                serialise(e);
                ),
            (StructConstant,
                m_out.write_tag(3);
                serialise_simplepath(e.ty);
                ),
            (Function,
                m_out.write_tag(4);
                serialise(e);
                ),
            (StructConstructor,
                m_out.write_tag(5);
                serialise_simplepath(e.ty);
                )
            )
        }

        void serialise(const ::HIR::Linkage& linkage)
        {
            //m_out.write_tag( static_cast<int>(linkage.type) );
            m_out.write_string( linkage.name );
        }

        // - Value items
        void serialise(const ::HIR::Function& fcn)
        {
            TRACE_FUNCTION_F("_function:");

            serialise(fcn.m_linkage);

            m_out.write_tag( static_cast<int>(fcn.m_receiver) );
            m_out.write_string(fcn.m_abi);
            m_out.write_bool(fcn.m_unsafe);
            m_out.write_bool(fcn.m_const);

            serialise_generics(fcn.m_params);
            m_out.write_count(fcn.m_args.size());
            for(const auto& a : fcn.m_args)
                serialise(a.second);
            m_out.write_bool(fcn.m_variadic);
            serialise(fcn.m_return);
            DEBUG("m_args = " << fcn.m_args);

            serialise(fcn.m_code, fcn.m_save_code || fcn.m_const);
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

            serialise(item.m_linkage);

            m_out.write_bool(item.m_is_mut);
            serialise(item.m_type);

            // NOTE: Value not stored (What if the static is generic? It can't be.)
        }

        // - Type items
        void serialise(const ::HIR::TypeAlias& ta)
        {
            serialise_generics(ta.m_params);
            serialise_type(ta.m_type);
        }
        void serialise(const ::HIR::Enum& item)
        {
            serialise_generics(item.m_params);
            serialise( item.m_data );

            serialise(item.m_markings);
        }
        void serialise(const ::HIR::Enum::Class& v)
        {
            m_out.write_tag( v.tag() );
            TU_MATCHA( (v), (e),
            (Value,
                m_out.write_tag( static_cast<int>(e.repr) );
                serialise_vec(e.variants);
                ),
            (Data,
                serialise_vec(e);
                )
            )
        }
        void serialise(const ::HIR::Enum::ValueVariant& v)
        {
            m_out.write_string(v.name);
            // NOTE: No expr, no longer needed
            m_out.write_u64(v.val);
        }
        void serialise(const ::HIR::Enum::DataVariant& v)
        {
            m_out.write_string(v.name);
            m_out.write_bool(v.is_struct);
            serialise(v.type);
        }

        void serialise(const ::HIR::TraitMarkings& m)
        {
            uint8_t bitflag_1 = 0;
            #define BIT(i,fld)  if(fld) bitflag_1 |= 1 << (i);
            BIT(0, m.has_a_deref)
            BIT(1, m.is_copy)
            BIT(2, m.has_drop_impl)
            #undef BIT
            m_out.write_u8(bitflag_1);

            // TODO: auto_impls
        }
        void serialise(const ::HIR::StructMarkings& m)
        {
            uint8_t bitflag_1 = 0;
            #define BIT(i,fld)  if(fld) bitflag_1 |= 1 << (i);
            BIT(0, m.can_unsize)
            #undef BIT
            m_out.write_u8(bitflag_1);

            m_out.write_tag( static_cast<unsigned int>(m.dst_type) );
            m_out.write_tag( static_cast<unsigned int>(m.coerce_unsized) );
            m_out.write_count( m.coerce_unsized_index );
            m_out.write_count( m.coerce_param );
            m_out.write_count( m.unsized_field );
            m_out.write_count( m.unsized_param );
            // TODO: auto_impls
        }

        void serialise(const ::HIR::Struct& item)
        {
            TRACE_FUNCTION_F("Struct");

            serialise_generics(item.m_params);
            m_out.write_tag( static_cast<int>(item.m_repr) );

            m_out.write_tag( item.m_data.tag() );
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

            m_out.write_u64c(item.m_forced_alignment);
            serialise(item.m_markings);
            serialise(item.m_struct_markings);
        }
        void serialise(const ::HIR::Union& item)
        {
            TRACE_FUNCTION_F("Union");

            serialise_generics(item.m_params);
            m_out.write_tag( static_cast<int>(item.m_repr) );

            serialise_vec(item.m_variants);

            serialise(item.m_markings);
        }
        void serialise(const ::HIR::ExternType& item)
        {
            TRACE_FUNCTION_F("ExternType");
            serialise(item.m_markings);
        }
        void serialise(const ::HIR::Trait& item)
        {
            TRACE_FUNCTION_F("_trait:");
            serialise_generics(item.m_params);
            //m_out.write_string(item.m_lifetime);    // TODO: Better type for lifetime
            m_out.write_bool( item.m_is_marker );
            serialise_strmap( item.m_types );
            serialise_strmap( item.m_values );
            serialise_strmap( item.m_value_indexes );
            serialise_strmap( item.m_type_indexes );
            serialise_vec( item.m_all_parent_traits );
            serialise( item.m_vtable_path );
        }
        void serialise(const ::HIR::TraitValueItem& tvi)
        {
            m_out.write_tag( tvi.tag() );
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
            m_out.write_bool(at.is_sized);
            serialise(at.m_lifetime_bound);
            serialise_vec(at.m_trait_bounds);
            serialise_type(at.m_default);
        }
    };
//}

void HIR_Serialise(const ::std::string& filename, const ::HIR::Crate& crate)
{
    ::HIR::serialise::Writer    out;
    HirSerialiser  s { out };
    s.serialise_crate(crate);
    out.open(filename);
    s.serialise_crate(crate);
}

