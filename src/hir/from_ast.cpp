
#include "common.hpp"
#include "hir.hpp"
#include <main_bindings.hpp>
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include "from_ast.hpp"
#include "visitor.hpp"

::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::SimplePath path);
::HIR::Function LowerHIR_Function(const ::AST::Function& f);
::HIR::SimplePath LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, bool allow_final_generic = false);
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params);

// --------------------------------------------------------------------
::HIR::GenericParams LowerHIR_GenericParams(const ::AST::GenericParams& gp)
{
    ::HIR::GenericParams    rv;
    
    if( gp.ty_params().size() > 0 )
    {
        for(const auto& tp : gp.ty_params())
        {
            rv.m_types.push_back({ tp.name(), LowerHIR_Type(tp.get_default()), true });
        }
    }
    if( gp.lft_params().size() > 0 )
    {
        for(const auto& lft_name : gp.lft_params())
            rv.m_lifetimes.push_back( lft_name );
    }
    if( gp.bounds().size() > 0 )
    {
        for(const auto& bound : gp.bounds())
        {
            TU_MATCH(::AST::GenericBound, (bound), (e),
            (Lifetime,
                rv.m_bounds.push_back(::HIR::GenericBound::make_Lifetime({
                    e.test,
                    e.bound
                    }));
                ),
            (TypeLifetime,
                rv.m_bounds.push_back(::HIR::GenericBound::make_TypeLifetime({
                    LowerHIR_Type(e.type),
                    e.bound
                    }));
                ),
            (IsTrait,
                auto type = LowerHIR_Type(e.type);
                // Iterate associated types
                for(const auto& assoc : e.trait.nodes().back().args().m_assoc) {
                    struct H {
                        static ::HIR::GenericPath get_trait_path_with_type(const Span& sp, const ::AST::Path& trait_p, const ::std::string& name) {
                            const auto& tr = *trait_p.binding().as_Trait().trait_;
                            auto it = ::std::find_if( tr.items().begin(), tr.items().end(), [&](const auto& x){return x.name == name;} );
                            if( it != tr.items().end() )
                                return LowerHIR_GenericPath(sp, trait_p);
                            
                            for(const auto& st : tr.supertraits())
                            {
                                auto rv = H::get_trait_path_with_type(sp, st.ent, name);
                                if( rv.m_path.m_components.size() ) {
                                    // TODO: Fix parameters based on self parameters
                                    // - HACK!
                                    rv.m_params = LowerHIR_PathParams(sp, trait_p.nodes().back().args());
                                    return rv;
                                }
                            }
                            
                            return ::HIR::GenericPath();
                        }
                    };
                    auto left_type = ::HIR::TypeRef( ::HIR::Path( ::HIR::Path::Data::make_UfcsKnown({
                            box$( type.clone() ),
                            H::get_trait_path_with_type(bound.span, e.trait, assoc.first),
                            assoc.first,
                            {}
                            }) ) );
                    rv.m_bounds.push_back(::HIR::GenericBound::make_TypeEquality({ mv$(left_type), LowerHIR_Type(assoc.second) }));
                }
                
                auto trait_path = LowerHIR_GenericPath(bound.span, e.trait);
                rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({ mv$(type), ::HIR::TraitPath { mv$(trait_path), e.hrls } }));
                ),
            (MaybeTrait,
                if( ! e.type.m_data.is_Generic() )
                    BUG(bound.span, "MaybeTrait on non-param");
                const auto& param_name = e.type.m_data.as_Generic().name;
                unsigned param_idx = ::std::find_if( rv.m_types.begin(), rv.m_types.end(), [&](const auto& x) { return x.m_name == param_name; } ) - rv.m_types.begin();
                if( param_idx >= rv.m_types.size() ) {
                    BUG(bound.span, "MaybeTrait on parameter not in parameter list");
                }
                
                // Compare with list of known default traits (just Sized atm) and set a marker
                const auto path_Sized = ::HIR::SimplePath("", {"marker", "Sized"});
                auto trait = LowerHIR_GenericPath(bound.span, e.trait);
                if( trait.m_path == path_Sized ) {
                    rv.m_types[param_idx].m_is_sized = false;
                }
                else {
                    ERROR(bound.span, E0000, "MaybeTrait on unknown trait " << trait.m_path);
                }
                ),
            (NotTrait,
                TODO(bound.span, "Negative trait bounds");
                ),
            
            (Equality,
                rv.m_bounds.push_back(::HIR::GenericBound::make_TypeEquality({
                    LowerHIR_Type(e.type),
                    LowerHIR_Type(e.replacement)
                    }));
                )
            )
        }
    }
    
    return rv;
}

::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat)
{
    TRACE_FUNCTION_F("@" << pat.span().filename << ":" << pat.span().start_line << " pat = " << pat);
    
    ::HIR::PatternBinding   binding;
    if( pat.binding().is_valid() )
    {
        ::HIR::PatternBinding::Type bt = ::HIR::PatternBinding::Type::Move;
        switch(pat.binding().m_type)
        {
        case ::AST::PatternBinding::Type::MOVE: bt = ::HIR::PatternBinding::Type::Move; break;
        case ::AST::PatternBinding::Type::REF:  bt = ::HIR::PatternBinding::Type::Ref;  break;
        case ::AST::PatternBinding::Type::MUTREF: bt = ::HIR::PatternBinding::Type::MutRef; break;
        }
        // TODO: Get bound slot
        binding = ::HIR::PatternBinding(pat.binding().m_mutable, bt, pat.binding().m_name, pat.binding().m_slot);
    }
    TU_MATCH(::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        BUG(pat.span(), "Encountered MaybeBind pattern");
        ),
    (Macro,
        BUG(pat.span(), "Encountered Macro pattern");
        ),
    (Any,
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Any({})
            };
        ),
    (Box,
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Box({
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
        ),
    (Ref,
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Ref({
                (e.mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared),
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
        ),
    (Tuple,
        ::std::vector< ::HIR::Pattern>  sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( LowerHIR_Pattern(sp) );
        
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Tuple({
                mv$(sub_patterns)
                })
            };
        ),
    
    (WildcardStructTuple,
        TU_MATCH_DEF(::AST::PathBinding, (e.path.binding()), (pb),
        (
            BUG(pat.span(), "Encountered StructTuple pattern not pointing to a enum variant or a struct - " << e.path);
            ),
        (EnumVar,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumTupleWildcard({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr, 0
                    })
                };
            ),
        (Struct,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_StructTupleWildcard({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr
                    })
                };
            )
        )
        ),
    (StructTuple,
        ::std::vector< ::HIR::Pattern>  sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( LowerHIR_Pattern(sp) );
        
        TU_MATCH_DEF(::AST::PathBinding, (e.path.binding()), (pb),
        (
            BUG(pat.span(), "Encountered StructTuple pattern not pointing to a enum variant or a struct - " << e.path);
            ),
        (EnumVar,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumTuple({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr, 0,
                    mv$(sub_patterns)
                    })
                };
            ),
        (Struct,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_StructTuple({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
                    mv$(sub_patterns)
                    })
                };
            )
        )
        ),
    (Struct,
        ::std::vector< ::std::pair< ::std::string, ::HIR::Pattern> > sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( ::std::make_pair(sp.first, LowerHIR_Pattern(sp.second)) );
        
        
        TU_MATCH_DEF(::AST::PathBinding, (e.path.binding()), (pb),
        (
            BUG(pat.span(), "Encountered Struct pattern not pointing to a enum variant or a struct");
            ),
        (EnumVar,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumStruct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr, 0,
                    mv$(sub_patterns),
                    e.is_exhaustive
                    })
                };
            ),
        (Struct,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Struct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
                    mv$(sub_patterns),
                    e.is_exhaustive
                    })
                };
            )
        )
        ),
    
    (Value,
        struct H {
            static ::HIR::CoreType get_int_type(const Span& sp, const ::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY:  return ::HIR::CoreType::Str;

                case CORETYPE_I8 :  return ::HIR::CoreType::I8;
                case CORETYPE_U8 :  return ::HIR::CoreType::U8;
                case CORETYPE_I16:  return ::HIR::CoreType::I16;
                case CORETYPE_U16:  return ::HIR::CoreType::U16;
                case CORETYPE_I32:  return ::HIR::CoreType::I32;
                case CORETYPE_U32:  return ::HIR::CoreType::U32;
                case CORETYPE_I64:  return ::HIR::CoreType::I64;
                case CORETYPE_U64:  return ::HIR::CoreType::U64;

                case CORETYPE_INT:  return ::HIR::CoreType::Isize;
                case CORETYPE_UINT: return ::HIR::CoreType::Usize;
                
                case CORETYPE_CHAR: return ::HIR::CoreType::Char;
                
                case CORETYPE_BOOL: return ::HIR::CoreType::Bool;
                
                default:
                    BUG(sp, "Unknown type for integer literal in pattern - " << ct );
                }
            }
            static ::HIR::Pattern::Value lowerhir_pattern_value(const Span& sp, const ::AST::Pattern::Value& v) {
                TU_MATCH(::AST::Pattern::Value, (v), (e),
                (Invalid,
                    BUG(sp, "Encountered Invalid value in Pattern");
                    ),
                (Integer,
                    return ::HIR::Pattern::Value::make_Integer({
                        H::get_int_type(sp, e.type),
                        e.value
                        });
                    ),
                (String,
                    return ::HIR::Pattern::Value::make_String(e);
                    ),
                (Named,
                    return ::HIR::Pattern::Value::make_Named( LowerHIR_Path(sp, e) );
                    )
                )
                throw "BUGCHECK: Reached end of LowerHIR_Pattern::H::lowerhir_pattern_value";
            }
        };
        if( e.end.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Value({
                    H::lowerhir_pattern_value(pat.span(), e.start)
                    })
                };
        }
        else {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Range({
                    H::lowerhir_pattern_value(pat.span(), e.start),
                    H::lowerhir_pattern_value(pat.span(), e.end)
                    })
                };
        }
        ),
    (Slice,
        ::std::vector< ::HIR::Pattern>  leading;
        for(const auto& sp : e.leading)
            leading.push_back( LowerHIR_Pattern(sp) );
        
        if( e.extra_bind != "" || e.trailing.size() > 0 ) {
            ::std::vector< ::HIR::Pattern>  trailing;
            for(const auto& sp : e.trailing)
                trailing.push_back( LowerHIR_Pattern(sp) );
            
            auto extra_bind = (e.extra_bind == "_" || e.extra_bind == "")
                ? ::HIR::PatternBinding()
                // TODO: Get slot name for `extra_bind`
                : ::HIR::PatternBinding(false, ::HIR::PatternBinding::Type::Ref, e.extra_bind, 0)
                ;
            
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_SplitSlice({
                    mv$(leading),
                    mv$(extra_bind),
                    mv$(trailing)
                    })
                };
        }
        else {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Slice({
                    mv$(leading)
                    })
                };
        }
        )
    )
    throw ::std::runtime_error("TODO: LowerHIR_Pattern");
}

::HIR::ExprPtr LowerHIR_Expr(const ::std::shared_ptr< ::AST::ExprNode>& e)
{
    if( e.get() ) {
        return LowerHIR_ExprNode(*e);
    }
    else {
        return ::HIR::ExprPtr();
    }
}
::HIR::ExprPtr LowerHIR_Expr(const ::AST::Expr& e)
{
    if( e.is_valid() ) {
        return LowerHIR_ExprNode(e.node());
    }
    else {
        return ::HIR::ExprPtr();
    }
}

::HIR::SimplePath LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, bool allow_final_generic)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        ::HIR::SimplePath   rv( e.crate );
        for( const auto& node : e.nodes )
        {
            if( ! node.args().is_empty() )
            {
                if( allow_final_generic && &node == &e.nodes.back() ) {
                    // Let it pass
                }
                else {
                    throw "BUG: Encountered path with parameters when creating ::HIR::GenericPath";
                }
            }
            
            rv.m_components.push_back( node.name() );
        }
        return rv;
    )
    else {
        throw "BUG: Encountered non-Absolute path when creating ::HIR::GenericPath";
    }
}
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params)
{
    ::HIR::PathParams   params;
    
    // TODO: Lifetime params (not encoded in ::HIR::PathNode as yet)
    //for(const auto& param : src_params.m_lifetimes) {
    //}

    for(const auto& param : src_params.m_types) {
        params.m_types.push_back( LowerHIR_Type(param) );
    }
    
    // Leave 'm_assoc' alone?

    return params;
}
::HIR::GenericPath LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        auto simpepath = LowerHIR_SimplePath(sp, path, true);
        ::HIR::PathParams   params = LowerHIR_PathParams(sp, e.nodes.back().args());
        auto rv = ::HIR::GenericPath(mv$(simpepath), mv$(params));
        DEBUG(path << " => " << rv);
        return rv;
    )
    else {
        BUG(sp, "Encountered non-Absolute path when creating ::HIR::GenericPath - " << path);
    }
}
::HIR::Path LowerHIR_Path(const Span& sp, const ::AST::Path& path)
{
    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        BUG(sp, "BUG: Encountered Invalid path in LowerHIR_Path");
        ),
    (Local,
        TODO(sp, "What to do wth Path::Class::Local in LowerHIR_Path - " << path);
        ),
    (Relative,
        BUG(sp, "Encountered `Relative` path in LowerHIR_Path - " << path);
        ),
    (Self,
        BUG(sp, "Encountered `Self` path in LowerHIR_Path - " << path);
        ),
    (Super,
        BUG(sp, "Encountered `Super` path in LowerHIR_Path - " << path);
        ),
    (Absolute,
        return ::HIR::Path( LowerHIR_GenericPath(sp, path) );
        ),
    (UFCS,
        if( e.nodes.size() != 1 )
            TODO(sp, "Handle UFCS with multiple nodes - " << path);
        auto params = LowerHIR_PathParams(sp, e.nodes.front().args());
        if( ! e.trait )
        {
            auto type = box$( LowerHIR_Type(*e.type) );
            if( type->m_data.is_Generic() ) {
                BUG(sp, "Generics can't be used with UfcsInherent - " << path);
            }
            return ::HIR::Path(::HIR::Path::Data::make_UfcsInherent({
                mv$(type),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else if( ! e.trait->is_valid() )
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsUnknown({
                box$( LowerHIR_Type(*e.type) ),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                box$(LowerHIR_Type(*e.type)),
                LowerHIR_GenericPath(sp, *e.trait),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Path";
}

::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        // Aka diverging
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Diverge({}) );
        ),
    (Any,
        return ::HIR::TypeRef();
        ),
    (Unit,
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple({}) );
        ),
    (Macro,
        BUG(ty.span(), "TypeData::None");
        ),
    (Primitive,
        switch(e.core_type)
        {
        case CORETYPE_BOOL: return ::HIR::TypeRef( ::HIR::CoreType::Bool );
        case CORETYPE_CHAR: return ::HIR::TypeRef( ::HIR::CoreType::Char );
        case CORETYPE_STR : return ::HIR::TypeRef( ::HIR::CoreType::Str );
        case CORETYPE_F32:  return ::HIR::TypeRef( ::HIR::CoreType::F32 );
        case CORETYPE_F64:  return ::HIR::TypeRef( ::HIR::CoreType::F64 );
        
        case CORETYPE_I8 :  return ::HIR::TypeRef( ::HIR::CoreType::I8 );
        case CORETYPE_U8 :  return ::HIR::TypeRef( ::HIR::CoreType::U8 );
        case CORETYPE_I16:  return ::HIR::TypeRef( ::HIR::CoreType::I16 );
        case CORETYPE_U16:  return ::HIR::TypeRef( ::HIR::CoreType::U16 );
        case CORETYPE_I32:  return ::HIR::TypeRef( ::HIR::CoreType::I32 );
        case CORETYPE_U32:  return ::HIR::TypeRef( ::HIR::CoreType::U32 );
        case CORETYPE_I64:  return ::HIR::TypeRef( ::HIR::CoreType::I64 );
        case CORETYPE_U64:  return ::HIR::TypeRef( ::HIR::CoreType::U64 );

        case CORETYPE_INT:  return ::HIR::TypeRef( ::HIR::CoreType::Isize );
        case CORETYPE_UINT: return ::HIR::TypeRef( ::HIR::CoreType::Usize );
        case CORETYPE_ANY:
            TODO(ty.span(), "TypeData::Primitive - CORETYPE_ANY");
        case CORETYPE_INVAL:
            BUG(ty.span(), "TypeData::Primitive - CORETYPE_INVAL");
        }
        ),
    (Tuple,
        ::HIR::TypeRef::Data::Data_Tuple v;
        for( const auto& st : e.inner_types )
        {
            v.push_back( LowerHIR_Type(st) );
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple(mv$(v)) );
        ),
    (Borrow,
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_borrow( cl, LowerHIR_Type(*e.inner) );
        ),
    (Pointer,
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_pointer( cl, LowerHIR_Type(*e.inner) );
        ),
    (Array,
        if( e.size ) {
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
                box$( LowerHIR_Type(*e.inner) ),
                LowerHIR_Expr( e.size ),
                ~0u
                }) );
        }
        else {
            return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({
                box$( LowerHIR_Type(*e.inner) )
                }) );
        }
        ),
    
    (Path,
        TU_IFLET(::AST::Path::Class, e.path.m_class, Local, l,
            unsigned int slot;
            // NOTE: TypeParameter is unused
            TU_IFLET(::AST::PathBinding, e.path.binding(), Variable, p,
                slot = p.slot;
            )
            else {
                BUG(ty.span(), "Unbound local encountered in " << e.path);
            }
            return ::HIR::TypeRef( l.name, slot );
        )
        else {
            return ::HIR::TypeRef( LowerHIR_Path(ty.span(), e.path) );
        }
        ),
    (TraitObject,
        if( e.hrls.size() > 0 )
            TODO(ty.span(), "TODO: TraitObjects with HRLS");
        ::HIR::TypeRef::Data::Data_TraitObject  v;
        for(const auto& t : e.traits)
        {
            v.m_traits.push_back( LowerHIR_GenericPath(ty.span(), t) );
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_TraitObject( mv$(v) ) );
        ),
    (Function,
        ::std::vector< ::HIR::TypeRef>  args;
        for(const auto& arg : e.info.m_arg_types)
            args.push_back( LowerHIR_Type(arg) );
        ::HIR::FunctionType f {
            e.info.is_unsafe,
            e.info.m_abi,
            box$( LowerHIR_Type(*e.info.m_rettype) ),
            mv$(args)
            };
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function( mv$(f) ) );
        ),
    (Generic,
        assert(e.index < 0x10000);
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Generic({ e.name, e.index }) );
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Type";
}

::HIR::TypeAlias LowerHIR_TypeAlias(const ::AST::TypeAlias& ta)
{
    return ::HIR::TypeAlias {
        LowerHIR_GenericParams(ta.params()),
        LowerHIR_Type(ta.type())
        };
}

::HIR::Struct LowerHIR_Struct(const ::AST::Struct& ent)
{
    ::HIR::Struct::Data data;
    
    TU_MATCH(::AST::StructData, (ent.m_data), (e),
    (Tuple,
        if( e.ents.size() == 0 ) {
            data = ::HIR::Struct::Data::make_Unit({});
        }
        else {
            ::HIR::Struct::Data::Data_Tuple fields;
            
            for(const auto& field : e.ents)
                fields.push_back( { field.m_is_public, LowerHIR_Type(field.m_type) } );
            
            data = ::HIR::Struct::Data::make_Tuple( mv$(fields) );
        }
        ),
    (Struct,
        ::HIR::Struct::Data::Data_Named fields;
        for(const auto& field : e.ents)
            fields.push_back( ::std::make_pair( field.m_name, ::HIR::VisEnt< ::HIR::TypeRef> { field.m_is_public, LowerHIR_Type(field.m_type) } ) );
        data = ::HIR::Struct::Data::make_Named( mv$(fields) );
        )
    )

    return ::HIR::Struct {
        LowerHIR_GenericParams(ent.params()),
        // TODO: Get repr from attributes
        ::HIR::Struct::Repr::Rust,
        mv$(data)
        };
}

::HIR::Enum LowerHIR_Enum(const ::AST::Enum& f)
{
    ::std::vector< ::std::pair< ::std::string, ::HIR::Enum::Variant> >  variants;
    
    for(const auto& var : f.variants())
    {
        TU_MATCH(::AST::EnumVariantData, (var.m_data), (e),
        (Value,
            variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Value(LowerHIR_Expr(e.m_value)) ) );
            ),
        (Tuple,
            if( e.m_sub_types.size() == 0 ) {
                variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Unit({})) );
            }
            else {
                ::std::vector< ::HIR::TypeRef>   types;
                for(const auto& st : e.m_sub_types)
                    types.push_back( LowerHIR_Type(st) );
                variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Tuple(mv$(types))) );
            }
            ),
        (Struct,
            ::std::vector< ::std::pair< ::std::string, ::HIR::TypeRef> >    ents;
            for( const auto& ent : e.m_fields )
                ents.push_back( ::std::make_pair( ent.m_name, LowerHIR_Type(ent.m_type) ) );
            variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Struct(mv$(ents))) );
            )
        )
    }
    
    return ::HIR::Enum {
        LowerHIR_GenericParams(f.params()),
        // TODO: Get repr from attributes
        ::HIR::Enum::Repr::Rust,
        mv$(variants)
        };
}
::HIR::Trait LowerHIR_Trait(::HIR::SimplePath trait_path, const ::AST::Trait& f)
{
    ::std::string   lifetime;
    
    ::std::vector< ::HIR::GenericPath>    supertraits;
    for(const auto& st : f.supertraits()) {
        if( st.ent.is_valid() ) {
            supertraits.push_back( LowerHIR_GenericPath(st.sp, st.ent) );
        }
        else {
            lifetime = "static";
        }
    }
    ::HIR::Trait    rv {
        LowerHIR_GenericParams(f.params()),
        mv$(lifetime),
        mv$(supertraits)
        };

    {
        auto this_trait = ::HIR::GenericPath( mv$(trait_path) );
        unsigned int i = 0;
        for(const auto& arg : rv.m_params.m_types) {
            this_trait.m_params.m_types.push_back( ::HIR::TypeRef(arg.m_name, i) );
            i ++;
        }
        rv.m_params.m_bounds.push_back( ::HIR::GenericBound::make_TraitBound({ ::HIR::TypeRef("Self",0xFFFF), mv$(this_trait) }) );
    }
    
    for(const auto& item : f.items())
    {
        TU_MATCH_DEF(::AST::Item, (item.data), (i),
        (
            BUG(item.data.span, "Encountered unexpected item type in trait");
            ),
        (Type,
            rv.m_types.insert( ::std::make_pair(item.name, ::HIR::AssociatedType {
                LowerHIR_GenericParams(i.params()),
                LowerHIR_Type(i.type()) 
                }) );
            ),
        (Function,
            rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Function( LowerHIR_Function(i) )) );
            ),
        (Static,
            if( i.s_class() == ::AST::Static::CONST )
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Constant(::HIR::Constant {
                    ::HIR::GenericParams {},
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    })) );
            else {
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Static(::HIR::Static {
                    (i.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    })) );
            }
            )
        )
    }
    
    return rv;
}
::HIR::Function LowerHIR_Function(const ::AST::Function& f)
{
    ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef > >    args;
    for(const auto& arg : f.args())
        args.push_back( ::std::make_pair( LowerHIR_Pattern(arg.first), LowerHIR_Type(arg.second) ) );
    
    // TODO: ABI and unsafety/constness
    return ::HIR::Function {
        "rust", false, false,
        LowerHIR_GenericParams(f.params()),
        mv$(args),
        LowerHIR_Type( f.rettype() ),
        LowerHIR_Expr( f.code() )
        };
}

void _add_mod_ns_item(::HIR::Module& mod, ::std::string name, bool is_pub,  ::HIR::TypeItem ti) {
    mod.m_mod_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::TypeItem> { is_pub, mv$(ti) }) ) );
}
void _add_mod_val_item(::HIR::Module& mod, ::std::string name, bool is_pub,  ::HIR::ValueItem ti) {
    mod.m_value_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::ValueItem> { is_pub, mv$(ti) }) ) );
}

::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::SimplePath path)
{
    TRACE_FUNCTION_F("path = " << path);
    ::HIR::Module   mod { };

    for( const auto& item : module.items() )
    {
        auto item_path = path + item.name;
        TU_MATCH(::AST::Item, (item.data), (e),
        (None,
            ),
        (Module,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Module(e, mv$(item_path)) );
            ),
        (Crate,
            // TODO: All 'extern crate' items should be normalised into a list in the crate root
            // - If public, add a namespace import here referring to the root of the imported crate
            ),
        (Type,
            _add_mod_ns_item( mod,  item.name, item.is_pub, ::HIR::TypeItem::make_TypeAlias( LowerHIR_TypeAlias(e) ) );
            ),
        (Struct,
            /// Add value reference
            TU_IFLET( ::AST::StructData, e.m_data, Struct, e2,
                if( e2.ents.size() == 0 )
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstant({mv$(item_path)}) );
                else
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstructor({mv$(item_path)}) );
            )
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Struct(e) );
            ),
        (Enum,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Enum(e) );
            ),
        (Trait,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Trait(item_path, e) );
            ),
        (Function,
            _add_mod_val_item(mod, item.name, item.is_pub,  LowerHIR_Function(e));
            ),
        (Static,
            if( e.s_class() == ::AST::Static::CONST )
                _add_mod_val_item(mod, item.name, item.is_pub,  ::HIR::ValueItem::make_Constant(::HIR::Constant {
                    ::HIR::GenericParams {},
                    LowerHIR_Type( e.type() ),
                    LowerHIR_Expr( e.value() )
                    }));
            else {
                _add_mod_val_item(mod, item.name, item.is_pub,  ::HIR::ValueItem::make_Static(::HIR::Static {
                    (e.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( e.type() ),
                    LowerHIR_Expr( e.value() )
                    }));
            }
            )
        )
    }
    
    for( unsigned int i = 0; i < module.anon_mods().size(); i ++ )
    {
        auto& submod = *module.anon_mods()[i];
        ::std::string name = FMT("#" << i);
        auto item_path = path + name;
        _add_mod_ns_item( mod,  mv$(name), false, ::HIR::TypeItem::make_Module( LowerHIR_Module(submod, mv$(item_path)) ) );
    }
    
    // TODO: Impl blocks
    
    // TODO: Populate trait list
    for(const auto& item : module.m_type_items)
    {
        if( item.second.path.binding().is_Trait() ) {
            mod.m_traits.push_back( LowerHIR_SimplePath(Span(), item.second.path) ); 
        }
    }
    
    return mod;
}

void LowerHIR_Module_Impls(const ::AST::Module& ast_mod,  ::HIR::Crate& hir_crate)
{
    // Sub-modules
    for( const auto& item : ast_mod.items() )
    {
        TU_IFLET(::AST::Item, item.data, Module, e,
            LowerHIR_Module_Impls(e,  hir_crate);
        )
    }
    for( const auto& submod_ptr : ast_mod.anon_mods() )
    {
        LowerHIR_Module_Impls(*submod_ptr,  hir_crate);
    }
    
    // 
    for( const auto& impl : ast_mod.impls() )
    {
        auto params = LowerHIR_GenericParams(impl.def().params());
        auto type = LowerHIR_Type(impl.def().type());
        
        if( impl.def().trait().ent.is_valid() )
        {
            const auto& trait_def = *impl.def().trait().ent.binding().as_Trait().trait_;
            bool is_marker = trait_def.is_marker();
            auto trait_path = LowerHIR_GenericPath(impl.def().trait().sp, impl.def().trait().ent);
            auto trait_name = mv$(trait_path.m_path);
            auto trait_args = mv$(trait_path.m_params);
            
            // TODO: Determine if a trait is a marker (i.e. is a OIBIT)
            
            if( is_marker )
            {
                hir_crate.m_marker_impls.insert( ::std::make_pair( mv$(trait_name), ::HIR::MarkerImpl {
                    mv$(params),
                    mv$(trait_args),
                    true,
                    mv$(type)
                    } ) );
            }
            else
            {
                ::std::map< ::std::string, ::HIR::Function> methods;
                ::std::map< ::std::string, ::HIR::ExprPtr> constants;
                ::std::map< ::std::string, ::HIR::TypeRef> types;
                
                for(const auto& item : impl.items())
                {
                    TU_MATCH_DEF(::AST::Item, (item.data), (e),
                    (
                        ERROR(item.data.span, E0000, "Unexpected item type in trait impl");
                        ),
                    // TODO: Associated constants
                    (Type,
                        types.insert( ::std::make_pair(item.name,  LowerHIR_Type(e.type())) );
                        ),
                    (Function,
                        methods.insert( ::std::make_pair(item.name,  LowerHIR_Function(e)) );
                        )
                    )
                }
                
                hir_crate.m_trait_impls.insert( ::std::make_pair(mv$(trait_name), ::HIR::TraitImpl {
                    mv$(params),
                    mv$(trait_args),
                    mv$(type),
                    
                    mv$(methods),
                    mv$(constants),
                    mv$(types)
                    }) );
            }
        }
        else
        {
            // Inherent impls
            ::std::map< ::std::string, ::HIR::Function> methods;
            
            for(const auto& item : impl.items())
            {
                TU_MATCH_DEF(::AST::Item, (item.data), (e),
                (
                    ERROR(item.data.span, E0000, "Unexpected item type in inherent impl");
                    ),
                (Function,
                    methods.insert( ::std::make_pair(item.name,  LowerHIR_Function(e)) );
                    )
                )
            }
            
            hir_crate.m_type_impls.push_back( ::HIR::TypeImpl {
                mv$(params),
                mv$(type),
                mv$( methods )
                } );
        }
    }
    for( const auto& impl : ast_mod.neg_impls() )
    {
        auto params = LowerHIR_GenericParams(impl.params());
        auto type = LowerHIR_Type(impl.type());
        auto trait = LowerHIR_GenericPath(impl.trait().sp, impl.trait().ent);
        auto trait_name = mv$(trait.m_path);
        auto trait_args = mv$(trait.m_params);
        
        hir_crate.m_marker_impls.insert( ::std::make_pair( mv$(trait_name), ::HIR::MarkerImpl {
            mv$(params),
            mv$(trait_args),
            false,
            mv$(type)
            } ) );
    }
}


class IndexVisitor:
    public ::HIR::Visitor
{
    const ::HIR::Crate& crate;
public:
    IndexVisitor(const ::HIR::Crate& crate):
        crate(crate)
    {}
    
    void visit_params(::HIR::GenericParams& params) override
    {
        for( auto& bound : params.m_bounds )
        {
            TU_IFLET(::HIR::GenericBound, bound, TraitBound, e,
                e.trait.m_trait_ptr = &this->crate.get_trait_by_path(Span(), e.trait.m_path.m_path);
            )
        }
    }
};

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs desugaring of for/if-let/while-let/...
::HIR::CratePtr LowerHIR_FromAST(::AST::Crate crate)
{
    ::HIR::Crate    rv;
    auto& macros = rv.m_exported_macros;
    
    // - Extract macros from root module
    for( const auto& mac : crate.m_root_module.macros() ) {
        //if( mac.data.export ) {
        macros.insert( ::std::make_pair( mac.name, mv$(*mac.data) ) );
        //}
    }
    for( const auto& mac : crate.m_root_module.macro_imports_res() ) {
        //if( mac.data->export ) {
        macros.insert( ::std::make_pair( mac.name, *mac.data ) );
        //}
    }
    
    rv.m_root_module = LowerHIR_Module( crate.m_root_module, ::HIR::SimplePath("") );
    
    LowerHIR_Module_Impls(crate.m_root_module,  rv);
    
    // Set all pointers in the HIR to the correct (now fixed) locations
    IndexVisitor(rv).visit_crate( rv );
    
    return ::HIR::CratePtr( mv$(rv) );
}


