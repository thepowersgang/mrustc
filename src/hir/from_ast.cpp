
#include "common.hpp"
#include "hir.hpp"
#include <main_bindings.hpp>
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include "from_ast.hpp"

::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::SimplePath path);
::HIR::Function LowerHIR_Function(const ::AST::Function& f);

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs desugaring of for/if-let/while-let/...
::HIR::CratePtr LowerHIR_FromAST(::AST::Crate crate)
{
    ::std::unordered_map< ::std::string, MacroRules >   macros;
    
    // - Extract macros from root module
    for( const auto& mac : crate.m_root_module.macros() ) {
        //if( mac.data.export ) {
        macros.insert( ::std::make_pair( mac.name, mac.data ) );
        //}
    }
    for( const auto& mac : crate.m_root_module.macro_imports_res() ) {
        //if( mac.data->export ) {
        macros.insert( ::std::make_pair( mac.name, *mac.data ) );
        //}
    }
    
    auto rootmod = LowerHIR_Module( crate.m_root_module, ::HIR::SimplePath("") );
    return ::HIR::CratePtr( ::HIR::Crate { mv$(rootmod), mv$(macros) } );
}

// --------------------------------------------------------------------
::HIR::GenericParams LowerHIR_GenericParams(const ::AST::GenericParams& gp)
{
    ::HIR::GenericParams    rv;
    
    if( gp.ty_params().size() > 0 )
    {
        for(const auto& tp : gp.ty_params())
        {
            rv.m_types.push_back({ tp.name(), LowerHIR_Type(tp.get_default()) });
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
                rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({
                    LowerHIR_Type(e.type),
                    ::HIR::TraitPath { LowerHIR_GenericPath(e.trait), e.hrls }
                    }));
                ),
            (MaybeTrait,
                rv.m_bounds.push_back(::HIR::GenericBound::make_TraitUnbound({
                    LowerHIR_Type(e.type),
                    LowerHIR_GenericPath(e.trait)
                    }));
                ),
            (NotTrait,
                TODO(Span(), "Negative trait bounds");
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
    ::HIR::PatternBinding   binding;
    if( pat.binding() != "" )
    {
        ::HIR::PatternBinding::Type bt = ::HIR::PatternBinding::Type::Move;
        switch(pat.binding_type())
        {
        case ::AST::Pattern::BIND_MOVE: bt = ::HIR::PatternBinding::Type::Move; break;
        case ::AST::Pattern::BIND_REF:  bt = ::HIR::PatternBinding::Type::Ref;  break;
        case ::AST::Pattern::BIND_MUTREF: bt = ::HIR::PatternBinding::Type::MutRef; break;
        }
        // TODO: Get bound slot
        binding = ::HIR::PatternBinding(pat.binding_mut(), bt, pat.binding(), 0);
    }
    TU_MATCH(::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        BUG(Span(), "Encountered MaybeBind pattern");
        ),
    (Macro,
        BUG(Span(), "Encountered Macro pattern");
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
    
    (StructTuple,
        ::std::vector< ::HIR::Pattern>  sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( LowerHIR_Pattern(sp) );
        
        TU_MATCH_DEF(::AST::PathBinding, (e.path.binding()), (pb),
        (
            BUG(Span(), "Encountered StructTuple pattern not pointing to a enum variant or a struct - " << e.path);
            ),
        (EnumVar,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumTuple({
                    LowerHIR_GenericPath(e.path),
                    mv$(sub_patterns)
                    })
                };
            ),
        (Struct,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_StructTuple({
                    LowerHIR_GenericPath(e.path),
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
            BUG(Span(), "Encountered Struct pattern not pointing to a enum variant or a struct");
            ),
        (EnumVar,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumStruct({
                    LowerHIR_GenericPath(e.path),
                    mv$(sub_patterns)
                    })
                };
            ),
        (Struct,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Struct({
                    LowerHIR_GenericPath(e.path),
                    mv$(sub_patterns)
                    })
                };
            )
        )
        ),
    
    (Value,
        struct H {
            static ::HIR::CoreType get_int_type(const ::eCoreType ct) {
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
                default:
                    BUG(Span(), "Unknown type for integer literal");
                }
            }
            static ::HIR::Pattern::Value lowerhir_pattern_value(const ::AST::Pattern::Value& v) {
                TU_MATCH(::AST::Pattern::Value, (v), (e),
                (Invalid,
                    BUG(Span(), "Encountered Invalid value in Pattern");
                    ),
                (Integer,
                    return ::HIR::Pattern::Value::make_Integer({
                        H::get_int_type(e.type),
                        e.value
                        });
                    ),
                (String,
                    return ::HIR::Pattern::Value::make_String(e);
                    ),
                (Named,
                    return ::HIR::Pattern::Value::make_Named( LowerHIR_Path(e) );
                    )
                )
                throw "BUGCHECK: Reached end of LowerHIR_Pattern::H::lowerhir_pattern_value";
            }
        };
        if( e.end.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Value({
                    H::lowerhir_pattern_value(e.start)
                    })
                };
        }
        else {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Range({
                    H::lowerhir_pattern_value(e.start),
                    H::lowerhir_pattern_value(e.end)
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

::HIR::SimplePath LowerHIR_SimplePath(const ::AST::Path& path, bool allow_final_generic = false)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        ::HIR::SimplePath   rv( e.crate );
        for( const auto& node : e.nodes )
        {
            if( node.args().size() > 0 )
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
::HIR::GenericPath LowerHIR_GenericPath(const ::AST::Path& path)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        auto sp = LowerHIR_SimplePath(path, true);
        ::HIR::PathParams   params;
        for(const auto& param : e.nodes.back().args()) {
            params.m_types.push_back( LowerHIR_Type(param) );
        }
        return ::HIR::GenericPath(mv$(sp), mv$(params));
    )
    else {
        BUG(Span(), "Encountered non-Absolute path when creating ::HIR::GenericPath - " << path);
    }
}
::HIR::Path LowerHIR_Path(const ::AST::Path& path)
{
    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        throw "BUG: Encountered Invalid path in LowerHIR_Path";
        ),
    (Local,
        BUG(Span(), "TODO: What to do wth Path::Class::Local in LowerHIR_Path - " << path);
        ),
    (Relative,
        throw "BUG: Encountered `Relative` path in LowerHIR_Path";
        ),
    (Self,
        throw "BUG: Encountered `Self` path in LowerHIR_Path";
        ),
    (Super,
        throw "BUG: Encountered `Super` path in LowerHIR_Path";
        ),
    (Absolute,
        return ::HIR::Path( LowerHIR_GenericPath(path) );
        ),
    (UFCS,
        if( e.nodes.size() != 1 )
            throw "TODO: Handle UFCS with multiple nodes";
        return ::HIR::Path(
            LowerHIR_Type(*e.type),
            LowerHIR_GenericPath(*e.trait),
            e.nodes[0].name(),
            {}
            );
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Path";
}

::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        TODO(ty.span(), "TypeData::None");
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
        case CORETYPE_CHAR: return ::HIR::TypeRef( ::HIR::CoreType::Str );
        case CORETYPE_STR : return ::HIR::TypeRef( ::HIR::CoreType::Char );
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
        return ::HIR::TypeRef( ::HIR::TypeRef::Data( ::HIR::TypeRef::Data::Data_Borrow { cl, box$(LowerHIR_Type(*e.inner)) } ) );
        ),
    (Pointer,
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Pointer({e.is_mut, box$(LowerHIR_Type(*e.inner))}) );
        ),
    (Array,
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
            box$( LowerHIR_Type(*e.inner) ),
            LowerHIR_Expr( e.size )
            }) );
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
            return ::HIR::TypeRef( LowerHIR_Path(e.path) );
        }
        ),
    (TraitObject,
        if( e.hrls.size() > 0 )
            TODO(ty.span(), "TODO: TraitObjects with HRLS");
        ::HIR::TypeRef::Data::Data_TraitObject  v;
        for(const auto& t : e.traits)
        {
            v.m_traits.push_back( LowerHIR_GenericPath(t) );
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_TraitObject( mv$(v) ) );
        ),
    (Function,
        TODO(ty.span(), "Function types");
        //::HIR::FunctionType f;
        //return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function( mv$(f) ) );
        ),
    (Generic,
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Generic({ e.name, 0 }) );
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Type";
}

::HIR::TypeAlias LowerHIR_TypeAlias(const ::AST::TypeAlias& ta)
{
    throw ::std::runtime_error("TODO: LowerHIR_TypeAlias");
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
::HIR::Trait LowerHIR_Trait(const ::AST::Trait& f)
{
    ::std::vector< ::HIR::GenericPath>    supertraits;
    for(const auto& st : f.supertraits())
        supertraits.push_back( LowerHIR_GenericPath(st) );
    ::HIR::Trait    rv {
        LowerHIR_GenericParams(f.params()),
        mv$(supertraits),
        {},
        {}
        };
    
    for(const auto& item : f.items())
    {
        TU_MATCH_DEF(::AST::Item, (item.data), (i),
        (
            BUG(Span(), "Encountered unexpected item type in trait");
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
                ::HIR::TypeRef ty = ::HIR::TypeRef( ::HIR::Path(mv$(item_path)) );
                if( e2.ents.size() == 0 )
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstant({mv$(ty)}) );
                else
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstructor({mv$(ty)}) );
            )
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Struct(e) );
            ),
        (Enum,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Enum(e) );
            ),
        (Trait,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Trait(e) );
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
    
    
    // TODO: Populate trait list
    
    return mod;
}


