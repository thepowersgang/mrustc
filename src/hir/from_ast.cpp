/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/from_ast.cpp
 * - Constructs the HIR module tree from the AST module tree
 */
#include "common.hpp"
#include "hir.hpp"
#include "main_bindings.hpp"
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include "from_ast.hpp"
#include "visitor.hpp"
#include <macro_rules/macro_rules.hpp>
#include <hir/item_path.hpp>

::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits = {});
::HIR::Function LowerHIR_Function(::HIR::ItemPath path, const ::AST::Function& f, const ::HIR::TypeRef& self_type);
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc);
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path);

::HIR::SimplePath path_Sized;
::std::string   g_core_crate;
::HIR::Crate*   g_crate_ptr = nullptr;

// --------------------------------------------------------------------
::HIR::GenericParams LowerHIR_GenericParams(const ::AST::GenericParams& gp, bool* self_is_sized)
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
                
                // TODO: Check for `Sized`
                
                rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({ mv$(type), LowerHIR_TraitPath(bound.span, e.trait) }));
                rv.m_bounds.back().as_TraitBound().trait.m_hrls = e.hrls;
                ),
            (MaybeTrait,
                if( ! e.type.m_data.is_Generic() )
                    BUG(bound.span, "MaybeTrait on non-param");
                const auto& ge = e.type.m_data.as_Generic();
                const auto& param_name = ge.name;
                unsigned param_idx;
                if( ge.index == 0xFFFF ) {
                    if( !self_is_sized ) {
                        BUG(bound.span, "MaybeTrait on parameter on Self when not allowed");
                    }
                    param_idx = 0xFFFF;
                }
                else {
                    param_idx = ::std::find_if( rv.m_types.begin(), rv.m_types.end(), [&](const auto& x) { return x.m_name == param_name; } ) - rv.m_types.begin();
                    if( param_idx >= rv.m_types.size() ) {
                        BUG(bound.span, "MaybeTrait on parameter not in parameter list (#" << ge.index << " " << param_name << ")");
                    }
                }
                
                // Compare with list of known default traits (just Sized atm) and set a marker
                auto trait = LowerHIR_GenericPath(bound.span, e.trait);
                if( trait.m_path == path_Sized ) {
                    if( param_idx == 0xFFFF ) {
                        assert( self_is_sized );
                        *self_is_sized = false;
                    }
                    else {
                        assert( param_idx < rv.m_types.size() );
                        rv.m_types[param_idx].m_is_sized = false;
                    }
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
        binding = ::HIR::PatternBinding(pat.binding().m_mutable, bt, pat.binding().m_name, pat.binding().m_slot);
    }
    
    struct H {
        static ::std::vector< ::HIR::Pattern>   lowerhir_patternvec(const ::std::vector< ::AST::Pattern>& sub_patterns) {
            ::std::vector< ::HIR::Pattern>  rv;
            for(const auto& sp : sub_patterns)
                rv.push_back( LowerHIR_Pattern(sp) );
            return rv;
        }
    };
    
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
        auto leading  = H::lowerhir_patternvec( e.start );
        auto trailing = H::lowerhir_patternvec( e.end   );
        
        if( e.has_wildcard )
        {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_SplitTuple({
                    mv$(leading), mv$(trailing)
                    })
                };
        }
        else
        {
            assert( trailing.size() == 0 );
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Tuple({
                    mv$(leading)
                    })
                };
        }
        ),
    
    (StructTuple,
        unsigned int leading_count  = e.tup_pat.start.size();
        unsigned int trailing_count = e.tup_pat.end  .size();
        TU_MATCH_DEF(::AST::PathBinding, (e.path.binding()), (pb),
        (
            BUG(pat.span(), "Encountered StructTuple pattern not pointing to a enum variant or a struct - " << e.path);
            ),
        (EnumVar,
            assert( pb.enum_ || pb.hir );
            unsigned int field_count;
            if( pb.enum_ ) {
                const auto& var = pb.enum_->variants()[pb.idx].m_data;
                field_count = var.as_Tuple().m_sub_types.size();
            }
            else {
                const auto& var = pb.hir->m_variants.at(pb.idx).second;
                field_count = var.as_Tuple().size();
            }
            ::std::vector<HIR::Pattern> sub_patterns;
        
            if( e.tup_pat.has_wildcard ) {
                sub_patterns.reserve( field_count );
                if( leading_count + trailing_count > field_count ) {
                    ERROR(pat.span(), E0000, "Enum variant pattern has too many fields - " << field_count << " max, got " << leading_count + trailing_count);
                }
                unsigned int padding_count = field_count - leading_count - trailing_count;
                for(const auto& subpat : e.tup_pat.start) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
                for(unsigned int i = 0; i < padding_count; i ++) {
                    sub_patterns.push_back( ::HIR::Pattern() );
                }
                for(const auto& subpat : e.tup_pat.end) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
            }
            else {
                assert( trailing_count == 0 );
                
                if( leading_count != field_count ) {
                    ERROR(pat.span(), E0000, "Enum variant pattern has a mismatched field count - " << field_count << " exp, got " << leading_count);
                }
                sub_patterns = H::lowerhir_patternvec( e.tup_pat.start );
            }
            
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
            assert( pb.struct_ || pb.hir );
            unsigned int field_count;
            if( pb.struct_ ) {
                field_count = pb.struct_->m_data.as_Tuple().ents.size();
            }
            else {
                field_count = pb.hir->m_data.as_Tuple().size();
            }
            ::std::vector<HIR::Pattern> sub_patterns;
        
            if( e.tup_pat.has_wildcard ) {
                sub_patterns.reserve( field_count );
                if( leading_count + trailing_count > field_count ) {
                    ERROR(pat.span(), E0000, "Struct pattern has too many fields - " << field_count << " max, got " << leading_count + trailing_count);
                }
                unsigned int padding_count = field_count - leading_count - trailing_count;
                for(const auto& subpat : e.tup_pat.start) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
                for(unsigned int i = 0; i < padding_count; i ++) {
                    sub_patterns.push_back( ::HIR::Pattern() );
                }
                for(const auto& subpat : e.tup_pat.end) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
            }
            else {
                assert( trailing_count == 0 );
                
                if( leading_count != field_count ) {
                    ERROR(pat.span(), E0000, "Struct pattern has a mismatched field count - " << field_count << " exp, got " << leading_count);
                }
                sub_patterns = H::lowerhir_patternvec( e.tup_pat.start );
            }
            
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
            BUG(pat.span(), "Encountered Struct pattern not pointing to a enum variant or a struct - " << e.path);
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
        (TypeAlias,
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Struct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
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
            static ::HIR::CoreType get_float_type(const Span& sp, const ::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY:  return ::HIR::CoreType::Str;
                case CORETYPE_F32:  return ::HIR::CoreType::F32;
                case CORETYPE_F64:  return ::HIR::CoreType::F64;
                default:
                    BUG(sp, "Unknown type for float literal in pattern - " << ct );
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
                (Float,
                    return ::HIR::Pattern::Value::make_Float({
                        H::get_float_type(sp, e.type),
                        e.value
                        });
                    ),
                (String,
                    return ::HIR::Pattern::Value::make_String(e);
                    ),
                (ByteString,
                    return ::HIR::Pattern::Value::make_ByteString({e.v});
                    ),
                (Named,
                    return ::HIR::Pattern::Value::make_Named( {LowerHIR_Path(sp, e), nullptr} );
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
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc)
{
    ::HIR::PathParams   params;
    
    // TODO: Lifetime params (not encoded in ::HIR::PathNode as yet)
    //for(const auto& param : src_params.m_lifetimes) {
    //}

    for(const auto& param : src_params.m_types) {
        params.m_types.push_back( LowerHIR_Type(param) );
    }
    
    // Leave 'm_assoc' alone?
    if( !allow_assoc && src_params.m_assoc.size() > 0 )
    {
        BUG(sp, "Encountered path parameters with associated type bounds where they are not allowed");
    }

    return params;
}
::HIR::GenericPath LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path, bool allow_assoc)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        auto simpepath = LowerHIR_SimplePath(sp, path, true);
        ::HIR::PathParams   params = LowerHIR_PathParams(sp, e.nodes.back().args(), allow_assoc);
        auto rv = ::HIR::GenericPath(mv$(simpepath), mv$(params));
        DEBUG(path << " => " << rv);
        return rv;
    )
    else {
        BUG(sp, "Encountered non-Absolute path when creating ::HIR::GenericPath - " << path);
    }
}
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path)
{
    ::HIR::TraitPath    rv {
        LowerHIR_GenericPath(sp, path, true),
        {},
        {},
        nullptr
        };
    
    for(const auto& assoc : path.nodes().back().args().m_assoc)
    {
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, LowerHIR_Type(assoc.second) ));
    }
    
    return rv;
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
        // - No associated type bounds allowed in UFCS paths
        auto params = LowerHIR_PathParams(sp, e.nodes.front().args(), false);
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
        BUG(ty.span(), "TypeData::None");
        ),
    (Bang,
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
        BUG(ty.span(), "TypeData::Macro");
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
            return ::HIR::TypeRef::new_array( LowerHIR_Type(*e.inner), LowerHIR_Expr( e.size ) );
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
        //if( e.hrls.size() > 0 )
        //    TODO(ty.span(), "TraitObjects with HRLS - " << ty);
        ::HIR::TypeRef::Data::Data_TraitObject  v;
        // TODO: Lifetime
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << t);
            const auto& tb = t.binding().as_Trait();
            assert( tb.trait_ || tb.hir );
            if( (tb.trait_ ? tb.trait_->is_marker() : tb.hir->m_is_marker) ) {
                if( tb.hir ) {
                    DEBUG(tb.hir->m_values.size());
                }
                v.m_markers.push_back( LowerHIR_GenericPath(ty.span(), t) );
            }
            else {
                // TraitPath -> GenericPath -> SimplePath
                if( v.m_trait.m_path.m_path.m_components.size() > 0 ) {
                    ERROR(ty.span(), E0000, "Multiple data traits in trait object - " << ty);
                }
                v.m_trait = LowerHIR_TraitPath(ty.span(), t);
            }
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_TraitObject( mv$(v) ) );
        ),
    (ErasedType,
        //if( e.hrls.size() > 0 )
        //    TODO(ty.span(), "ErasedType with HRLS - " << ty);
        ASSERT_BUG(ty.span(), e.traits.size() > 0, "ErasedType with no traits");
        
        ::std::vector< ::HIR::TraitPath>    traits;
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << t);
            traits.push_back( LowerHIR_TraitPath(ty.span(), t) );
        }
        // Leave `m_origin` until the bind pass
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_ErasedType(::HIR::TypeRef::Data::Data_ErasedType {
            ::HIR::Path(::HIR::SimplePath()),
            mv$(traits),
            ::HIR::LifetimeRef()    // TODO: Lifetime ref
            } ) );
        ),
    (Function,
        ::std::vector< ::HIR::TypeRef>  args;
        for(const auto& arg : e.info.m_arg_types)
            args.push_back( LowerHIR_Type(arg) );
        ::HIR::FunctionType f {
            e.info.is_unsafe,
            e.info.m_abi,
            box$( LowerHIR_Type(*e.info.m_rettype) ),
            mv$(args)   // TODO: e.info.is_variadic
            };
        if( f.m_abi == "" )
            f.m_abi = ABI_RUST;
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
        LowerHIR_GenericParams(ta.params(), nullptr),
        LowerHIR_Type(ta.type())
        };
}


namespace {
    template<typename T>
    ::HIR::VisEnt<T> new_visent(bool pub, T v) {
        return ::HIR::VisEnt<T> { pub, mv$(v) };
    }
}

::HIR::Struct LowerHIR_Struct(::HIR::ItemPath path, const ::AST::Struct& ent)
{
    TRACE_FUNCTION_F(path);
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
            fields.push_back( ::std::make_pair( field.m_name, new_visent(field.m_is_public, LowerHIR_Type(field.m_type)) ) );
        data = ::HIR::Struct::Data::make_Named( mv$(fields) );
        )
    )

    return ::HIR::Struct {
        LowerHIR_GenericParams(ent.params(), nullptr),
        // TODO: Get repr from attributes
        ::HIR::Struct::Repr::Rust,
        mv$(data)
        };
}

::HIR::Enum LowerHIR_Enum(::HIR::ItemPath path, const ::AST::Enum& f)
{
    ::std::vector< ::std::pair< ::std::string, ::HIR::Enum::Variant> >  variants;
    
    for(const auto& var : f.variants())
    {
        TU_MATCH(::AST::EnumVariantData, (var.m_data), (e),
        (Value,
            if( e.m_value.is_valid() )
            {
                variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Value({
                    LowerHIR_Expr(e.m_value),
                    ::HIR::Literal {}
                    }) ) );
            }
            else
            {
                variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Unit({})) );
            }
            ),
        (Tuple,
            ::HIR::Enum::Variant::Data_Tuple    types;
            for(const auto& st : e.m_sub_types)
                types.push_back( new_visent(true, LowerHIR_Type(st)) );
            variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Tuple(mv$(types))) );
            ),
        (Struct,
            ::HIR::Enum::Variant::Data_Struct ents;
            for( const auto& ent : e.m_fields )
                ents.push_back( ::std::make_pair( ent.m_name, new_visent(true, LowerHIR_Type(ent.m_type)) ) );
            variants.push_back( ::std::make_pair(var.m_name, ::HIR::Enum::Variant::make_Struct(mv$(ents))) );
            )
        )
    }
    
    return ::HIR::Enum {
        LowerHIR_GenericParams(f.params(), nullptr),
        // TODO: Get repr from attributes
        ::HIR::Enum::Repr::Rust,
        mv$(variants)
        };
}
::HIR::Union LowerHIR_Union(::HIR::ItemPath path, const ::AST::Union& f, const ::AST::MetaItems& attrs)
{
    auto repr = ::HIR::Union::Repr::Rust;
    
    if( const auto* attr_repr = attrs.get("repr") )
    {
        const auto& repr_str = attr_repr->string();
        if( repr_str == "C" ) {
            repr = ::HIR::Union::Repr::C;
        }
        else {
            // TODO: Error?
        }
    }
    
    ::HIR::Struct::Data::Data_Named variants;
    for(const auto& field : f.m_variants)
        variants.push_back( ::std::make_pair( field.m_name, new_visent(field.m_is_public, LowerHIR_Type(field.m_type)) ) );
    
    return ::HIR::Union {
        LowerHIR_GenericParams(f.m_params, nullptr),
        repr,
        mv$(variants)
        };
}
::HIR::Trait LowerHIR_Trait(::HIR::SimplePath trait_path, const ::AST::Trait& f)
{
    TRACE_FUNCTION_F(trait_path);
    bool trait_reqires_sized = false;
    auto params = LowerHIR_GenericParams(f.params(), &trait_reqires_sized);
    
    ::std::string   lifetime;
    ::std::vector< ::HIR::TraitPath>    supertraits;
    for(const auto& st : f.supertraits()) {
        if( st.ent.is_valid() ) {
            supertraits.push_back( LowerHIR_TraitPath(st.sp, st.ent) );
        }
        else {
            lifetime = "static";
        }
    }
    ::HIR::Trait    rv {
        mv$(params),
        mv$(lifetime),
        mv$(supertraits)
        };

    {
        auto this_trait = ::HIR::GenericPath( trait_path );
        unsigned int i = 0;
        for(const auto& arg : rv.m_params.m_types) {
            this_trait.m_params.m_types.push_back( ::HIR::TypeRef(arg.m_name, i) );
            i ++;
        }
        rv.m_params.m_bounds.push_back( ::HIR::GenericBound::make_TraitBound({ ::HIR::TypeRef("Self",0xFFFF), { mv$(this_trait) } }) );
    }
    
    for(const auto& item : f.items())
    {
        auto trait_ip = ::HIR::ItemPath(trait_path);
        auto item_path = ::HIR::ItemPath( trait_ip, item.name.c_str() );
        
        TU_MATCH_DEF(::AST::Item, (item.data), (i),
        (
            BUG(item.data.span, "Encountered unexpected item type in trait");
            ),
        (Type,
            bool is_sized = true;
            ::std::vector< ::HIR::TraitPath>    trait_bounds;
            ::std::string   lifetime_bound;
            auto gps = LowerHIR_GenericParams(i.params(), &is_sized);
            for(auto& b : gps.m_bounds)
            {
                TU_MATCH(::HIR::GenericBound, (b), (be),
                (TypeLifetime,
                    ASSERT_BUG(item.data.span, be.type == ::HIR::TypeRef("Self", 0xFFFF), "Invalid lifetime bound on associated type");
                    lifetime_bound = mv$(be.valid_for);
                    ),
                (TraitBound,
                    ASSERT_BUG(item.data.span, be.type == ::HIR::TypeRef("Self", 0xFFFF), "Invalid trait bound on associated type");
                    trait_bounds.push_back( mv$(be.trait) );
                    ),
                (Lifetime,
                    BUG(item.data.span, "Unexpected lifetime-lifetime bound on associated type");
                    ),
                (TypeEquality,
                    BUG(item.data.span, "Unexpected type equality bound on associated type");
                    )
                )
            }
            rv.m_types.insert( ::std::make_pair(item.name, ::HIR::AssociatedType {
                is_sized,
                mv$(lifetime_bound),
                mv$(trait_bounds),
                LowerHIR_Type(i.type())
                }) );
            ),
        (Function,
            ::HIR::TypeRef  self_type {"Self", 0xFFFF};
            rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Function( LowerHIR_Function(item_path, i, self_type) )) );
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
    
    rv.m_is_marker = f.is_marker();
    
    return rv;
}
::HIR::Function LowerHIR_Function(::HIR::ItemPath p, const ::AST::Function& f, const ::HIR::TypeRef& self_type)
{
    static Span sp;
    
    TRACE_FUNCTION_F(p);
    
    ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef > >    args;
    for(const auto& arg : f.args())
        args.push_back( ::std::make_pair( LowerHIR_Pattern(arg.first), LowerHIR_Type(arg.second) ) );
    
    auto receiver = ::HIR::Function::Receiver::Free;
    
    if( args.size() > 0 && args.front().first.m_binding.m_name == "self" )
    {
        const auto& arg_self_ty = args.front().second;
        if( arg_self_ty == self_type ) {
            receiver = ::HIR::Function::Receiver::Value;
        }
        else TU_IFLET(::HIR::TypeRef::Data, arg_self_ty.m_data, Borrow, e,
            if( *e.inner == self_type )
            {
                switch(e.type)
                {
                case ::HIR::BorrowType::Owned:  receiver = ::HIR::Function::Receiver::BorrowOwned;  break;
                case ::HIR::BorrowType::Unique: receiver = ::HIR::Function::Receiver::BorrowUnique; break;
                case ::HIR::BorrowType::Shared: receiver = ::HIR::Function::Receiver::BorrowShared; break;
                }
            }
        )
        else TU_IFLET(::HIR::TypeRef::Data, arg_self_ty.m_data, Path, e,
            // Box - Compare with `owned_box` lang item
            TU_IFLET(::HIR::Path::Data, e.path.m_data, Generic, pe,
                if( pe.m_path == g_crate_ptr->get_lang_item_path(sp, "owned_box") )
                {
                    if( pe.m_params.m_types.size() == 1 && pe.m_params.m_types[0] == self_type )
                    {
                        receiver = ::HIR::Function::Receiver::Box;
                    }
                }
            )
        )
        else {
        }
        
        if( receiver == ::HIR::Function::Receiver::Free )
        {
            ERROR(sp, E0000, "Unknown receiver type - " << arg_self_ty);
        }
    }
    
    // TODO: ABI and unsafety/constness
    return ::HIR::Function {
        receiver,
        f.abi(), f.is_unsafe(), f.is_const(),
        LowerHIR_GenericParams(f.params(), nullptr),    // TODO: If this is a method, then it can add the Self: Sized bound
        mv$(args), f.is_variadic(),
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

::HIR::Module LowerHIR_Module(const ::AST::Module& ast_mod, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits)
{
    TRACE_FUNCTION_F("path = " << path);
    ::HIR::Module   mod { };
    
    mod.m_traits = mv$(traits);
    
    // Populate trait list
    for(const auto& item : ast_mod.m_type_items)
    {
        if( item.second.path.binding().is_Trait() ) {
            auto sp = LowerHIR_SimplePath(Span(), item.second.path);
            if( ::std::find(mod.m_traits.begin(), mod.m_traits.end(), sp) == mod.m_traits.end() )
                mod.m_traits.push_back( mv$(sp) ); 
        }
    }
    
    for( unsigned int i = 0; i < ast_mod.anon_mods().size(); i ++ )
    {
        const auto& submod_ptr = ast_mod.anon_mods()[i];
        if( submod_ptr )
        {
            auto& submod = *submod_ptr;
            ::std::string name = FMT("#" << i);
            auto item_path = ::HIR::ItemPath(path, name.c_str());
            _add_mod_ns_item( mod,  mv$(name), false, ::HIR::TypeItem::make_Module( LowerHIR_Module(submod, item_path, mod.m_traits) ) );
        }
    }

    for( const auto& item : ast_mod.items() )
    {
        const auto& sp = item.data.span;
        auto item_path = ::HIR::ItemPath(path, item.name.c_str());
        TU_MATCH(::AST::Item, (item.data), (e),
        (None,
            ),
        (MacroInv,
            // Valid.
            //BUG(sp, "Stray macro invocation in " << path);
            ),
        (ExternBlock,
            if( e.items().size() > 0 )
            {
                TODO(sp, "Expand ExternBlock");
            }
            // TODO: Insert a record of the `link` attribute
            ),
        (Impl,
            //TODO(sp, "Expand Item::Impl");
            ),
        (NegImpl,
            //TODO(sp, "Expand Item::NegImpl");
            ),
        (Use,
            // Ignore - The index is used to add `Import`s
            ),
        (Module,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Module(e, mv$(item_path)) );
            ),
        (Crate,
            // All 'extern crate' items should be normalised into a list in the crate root
            // - If public, add a namespace import here referring to the root of the imported crate
            _add_mod_ns_item( mod, item.name, item.is_pub, ::HIR::TypeItem::make_Import({ ::HIR::SimplePath(e.name, {}), false, 0} ) );
            ),
        (Type,
            _add_mod_ns_item( mod,  item.name, item.is_pub, ::HIR::TypeItem::make_TypeAlias( LowerHIR_TypeAlias(e) ) );
            ),
        (Struct,
            /// Add value reference
            TU_IFLET( ::AST::StructData, e.m_data, Tuple, e2,
                if( e2.ents.size() == 0 )
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstant({item_path.get_simple_path()}) );
                else
                    _add_mod_val_item( mod,  item.name, item.is_pub, ::HIR::ValueItem::make_StructConstructor({item_path.get_simple_path()}) );
            )
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Struct(item_path, e) );
            ),
        (Enum,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Enum(item_path, e) );
            ),
        (Union,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Union(item_path, e, item.data.attrs) );
            ),
        (Trait,
            _add_mod_ns_item( mod,  item.name, item.is_pub, LowerHIR_Trait(item_path.get_simple_path(), e) );
            ),
        (Function,
            _add_mod_val_item(mod, item.name, item.is_pub,  LowerHIR_Function(item_path, e, ::HIR::TypeRef{}));
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
    
    for( const auto& ie : ast_mod.m_namespace_items )
    {
        if( ie.second.is_import ) {
            auto hir_path = LowerHIR_SimplePath( Span(), ie.second.path );
            ::HIR::TypeItem ti;
            TU_MATCH_DEF( ::AST::PathBinding, (ie.second.path.binding()), (pb),
            (
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), false, 0 });
                ),
            (EnumVar,
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), true, pb.idx });
                )
            )
            _add_mod_ns_item(mod, ie.first, ie.second.is_pub, mv$(ti));
        }
    }
    for( const auto& ie : ast_mod.m_value_items )
    {
        if( ie.second.is_import ) {
            auto hir_path = LowerHIR_SimplePath( Span(), ie.second.path );
            ::HIR::ValueItem    vi;
            
            TU_MATCH_DEF( ::AST::PathBinding, (ie.second.path.binding()), (pb),
            (
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), false, 0 });
                ),
            (EnumVar,
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), true, pb.idx });
                )
            )
            _add_mod_val_item(mod, ie.first, ie.second.is_pub, mv$(vi));
        }
    }
    
    return mod;
}

void LowerHIR_Module_Impls(const ::AST::Module& ast_mod,  ::HIR::Crate& hir_crate)
{
    DEBUG(ast_mod.path());
    
    // Sub-modules
    for( const auto& item : ast_mod.items() )
    {
        TU_IFLET(::AST::Item, item.data, Module, e,
            LowerHIR_Module_Impls(e,  hir_crate);
        )
    }
    for( const auto& submod_ptr : ast_mod.anon_mods() )
    {
        if( submod_ptr ) {
            LowerHIR_Module_Impls(*submod_ptr,  hir_crate);
        }
    }
    
    // 
    for( const auto& i : ast_mod.items() )
    {
        if( !i.data.is_Impl() ) continue;
        const auto& impl = i.data.as_Impl();
        auto params = LowerHIR_GenericParams(impl.def().params(), nullptr);
        
        TRACE_FUNCTION_F("IMPL " << impl.def());
        
        if( impl.def().trait().ent.is_valid() )
        {
            const auto& pb = impl.def().trait().ent.binding();
            ASSERT_BUG(Span(), pb.is_Trait(), "Binding for trait path in impl isn't a Trait - " << impl.def().trait().ent);
            ASSERT_BUG(Span(), pb.as_Trait().trait_ || pb.as_Trait().hir, "Trait pointer for trait path in impl isn't set");
            bool is_marker = (pb.as_Trait().trait_ ? pb.as_Trait().trait_->is_marker() : pb.as_Trait().hir->m_is_marker);
            auto trait_path = LowerHIR_GenericPath(impl.def().trait().sp, impl.def().trait().ent);
            auto trait_name = mv$(trait_path.m_path);
            auto trait_args = mv$(trait_path.m_params);
            
            if( !is_marker )
            {
                auto type = LowerHIR_Type(impl.def().type());
                
                ::HIR::ItemPath    path(type, trait_name, trait_args);
                DEBUG(path);
                
                ::std::map< ::std::string, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> > methods;
                ::std::map< ::std::string, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> > constants;
                ::std::map< ::std::string, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> > types;
                
                for(const auto& item : impl.items())
                {
                    ::HIR::ItemPath    item_path(path, item.name.c_str());
                    TU_MATCH_DEF(::AST::Item, (*item.data), (e),
                    (
                        BUG(item.data->span, "Unexpected item type in trait impl - " << item.data->tag_str());
                        ),
                    (None,
                        ),
                    (MacroInv,
                        ),
                    (Static,
                        if( e.s_class() == ::AST::Static::CONST ) {
                            // TODO: Check signature against the trait?
                            constants.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> { item.is_specialisable, ::HIR::Constant {
                                ::HIR::GenericParams {},
                                LowerHIR_Type( e.type() ),
                                LowerHIR_Expr( e.value() )
                                } }) );
                        }
                        else {
                            TODO(item.data->span, "Associated statics in trait impl");
                        }
                        ),
                    (Type,
                        DEBUG("- type " << item.name);
                        types.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { item.is_specialisable, LowerHIR_Type(e.type()) }) );
                        ),
                    (Function,
                        DEBUG("- method " << item.name);
                        methods.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { item.is_specialisable, LowerHIR_Function(item_path, e, type) }) );
                        )
                    )
                }
                
                hir_crate.m_trait_impls.insert( ::std::make_pair(mv$(trait_name), ::HIR::TraitImpl {
                    mv$(params),
                    mv$(trait_args),
                    mv$(type),
                    
                    mv$(methods),
                    mv$(constants),
                    mv$(types),
                    
                    LowerHIR_SimplePath(Span(), ast_mod.path())
                    }) );
            }
            else if( impl.def().type().m_data.is_None() )
            {
                // Ignore - These are encoded in the 'is_marker' field of the trait
            }
            else
            {
                auto type = LowerHIR_Type(impl.def().type());
                hir_crate.m_marker_impls.insert( ::std::make_pair( mv$(trait_name), ::HIR::MarkerImpl {
                    mv$(params),
                    mv$(trait_args),
                    true,
                    mv$(type),
                    
                    LowerHIR_SimplePath(Span(), ast_mod.path())
                    } ) );
            }
        }
        else
        {
            // Inherent impls
            auto type = LowerHIR_Type(impl.def().type());
            ::HIR::ItemPath    path(type);
            
            ::std::map< ::std::string, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> > methods;
            ::std::map< ::std::string, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> > constants;
            
            for(const auto& item : impl.items())
            {
                ::HIR::ItemPath    item_path(path, item.name.c_str());
                TU_MATCH_DEF(::AST::Item, (*item.data), (e),
                (
                    BUG(item.data->span, "Unexpected item type in inherent impl - " << item.data->tag_str());
                    ),
                (None,
                    ),
                (MacroInv,
                    ),
                (Static,
                    if( e.s_class() == ::AST::Static::CONST ) {
                        constants.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> { item.is_pub, item.is_specialisable, ::HIR::Constant {
                            ::HIR::GenericParams {},
                            LowerHIR_Type( e.type() ),
                            LowerHIR_Expr( e.value() )
                            } }) );
                    }
                    else {
                        TODO(item.data->span, "Associated statics in inherent impl");
                    }
                    ),
                (Function,
                    methods.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> { item.is_pub, item.is_specialisable, LowerHIR_Function(item_path, e, type) } ) );
                    )
                )
            }
            
            hir_crate.m_type_impls.push_back( ::HIR::TypeImpl {
                mv$(params),
                mv$(type),
                mv$(methods),
                mv$(constants),
                
                LowerHIR_SimplePath(Span(), ast_mod.path())
                } );
        }
    }
    for( const auto& i : ast_mod.items() )
    {
        if( !i.data.is_NegImpl() ) continue;
        const auto& impl = i.data.as_NegImpl();
        
        auto params = LowerHIR_GenericParams(impl.params(), nullptr);
        auto type = LowerHIR_Type(impl.type());
        auto trait = LowerHIR_GenericPath(impl.trait().sp, impl.trait().ent);
        auto trait_name = mv$(trait.m_path);
        auto trait_args = mv$(trait.m_params);
        
        hir_crate.m_marker_impls.insert( ::std::make_pair( mv$(trait_name), ::HIR::MarkerImpl {
            mv$(params),
            mv$(trait_args),
            false,
            mv$(type),
            
            LowerHIR_SimplePath(Span(), ast_mod.path())
            } ) );
    }
}


class IndexVisitor:
    public ::HIR::Visitor
{
    const ::HIR::Crate& crate;
    Span    null_span;
public:
    IndexVisitor(const ::HIR::Crate& crate):
        crate(crate)
    {}
    
    void visit_params(::HIR::GenericParams& params) override
    {
        for( auto& bound : params.m_bounds )
        {
            TU_IFLET(::HIR::GenericBound, bound, TraitBound, e,
                e.trait.m_trait_ptr = &this->crate.get_trait_by_path(null_span, e.trait.m_path.m_path);
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
    g_crate_ptr = &rv;
    g_core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
    auto& macros = rv.m_exported_macros;
    
    // - Extract macros from root module
    for( /*const*/ auto& mac : crate.m_root_module.macros() ) {
        if( mac.data->m_exported ) {
            auto res = macros.insert( ::std::make_pair( mac.name, mv$(mac.data) ) );
            if( res.second )
                DEBUG("- Define " << mac.name << "!");
        }
        else {
            DEBUG("- Non-exported " << mac.name << "!");
        }
    }
    for( auto& mac : crate.m_root_module.macro_imports_res() ) {
        if( mac.data->m_exported && mac.name != "" ) {
            auto v = ::std::make_pair( mac.name, MacroRulesPtr(new MacroRules( mv$(*const_cast<MacroRules*>(mac.data)) )) );
            auto it = macros.find(mac.name);
            if( it == macros.end() )
            {
                auto res = macros.insert( mv$(v) );
                DEBUG("- Import " << mac.name << "! (from \"" << res.first->second->m_source_crate << "\")");
            }
            else {
                DEBUG("- Replace " << mac.name << "! (from \"" << it->second->m_source_crate << "\") with one from \"" << v.second->m_source_crate << "\"");
                it->second = mv$( v.second );
            }
        }
    }
    
    auto sp = Span();
    // - Store the lang item paths so conversion code can use them.
    for( const auto& lang_item_path : crate.m_lang_items )
    {
        rv.m_lang_items.insert( ::std::make_pair(lang_item_path.first, LowerHIR_SimplePath(sp, lang_item_path.second)) );
    }
    for(auto& ext_crate : crate.m_extern_crates)
    {
        // Populate m_lang_items from loaded crates too
        for( const auto& lang : ext_crate.second.m_hir->m_lang_items )
        {
            const auto& name = lang.first;
            const auto& path = lang.second;
            auto irv = rv.m_lang_items.insert( ::std::make_pair(name, path) );
            if( irv.second == false && irv.first->second != path )
            {
                ERROR(sp, E0000, "Conflicting definitions of lang item '" << name << "'. " << path << " and " << irv.first->second);
            }
        }
        rv.m_ext_crates.insert( ::std::make_pair( ext_crate.first, mv$(ext_crate.second.m_hir) ) );
    }
    path_Sized = rv.get_lang_item_path(sp, "sized");
    
    rv.m_root_module = LowerHIR_Module( crate.m_root_module, ::HIR::ItemPath() );
    
    LowerHIR_Module_Impls(crate.m_root_module,  rv);
    
    // Set all pointers in the HIR to the correct (now fixed) locations
    IndexVisitor(rv).visit_crate( rv );
    
    g_crate_ptr = nullptr;
    return ::HIR::CratePtr( mv$(rv) );
}


