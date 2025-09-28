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
#include <ast/expr.hpp> // For shortcut in array size handling
#include <ast/crate.hpp>
#include "from_ast.hpp"
#include "visitor.hpp"
#include <macro_rules/macro_rules.hpp>
#include <hir/item_path.hpp>
#include <limits.h>
#include <hir_typeck/helpers.hpp>   // monomorph
#include <trans/target.hpp>

::HIR::ExprPtr LowerHIR_Expr(const ::AST::Expr& e);
::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits = {});
::HIR::Function LowerHIR_Function(::HIR::ItemPath path, const ::AST::AttributeList& attrs, const ::AST::Function& f, const ::HIR::TypeRef& self_type);
::HIR::ValueItem LowerHIR_Static(::HIR::ItemPath p, const ::AST::AttributeList& attrs, const ::AST::Static& e, const Span& sp, const RcString& name);
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc);
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path, const AST::HigherRankedBounds& hrbs, bool allow_bounds=false);
::HIR::GenericParams LowerHIR_HigherRankedBounds(const AST::HigherRankedBounds& hrbs);

::HIR::SimplePath path_Sized;
::HIR::SimplePath path_PointeeSized;
::HIR::SimplePath path_MetadataSized;
RcString    g_core_crate;
RcString    g_crate_name;
::HIR::Crate*   g_crate_ptr = nullptr;
const ::AST::Crate* g_ast_crate_ptr;

namespace {
    const ::HIR::TypeRef ty_Self = HIR::TypeRef::new_self();
}

// --------------------------------------------------------------------
HIR::LifetimeRef LowerHIR_LifetimeRef(const ::AST::LifetimeRef& r)
{
    assert(r.binding() >= 0xFFF0 || r.binding() < 1024);
    return HIR::LifetimeRef(
        // TODO: names?
        r.binding()
        );
}

::HIR::Publicity LowerHIR_Vis(const ::HIR::SimplePath& mod_path, const AST::Visibility& vis)
{
    if( vis.is_global() ) {
        return ::HIR::Publicity::new_global();
    }
    const auto* ap = &vis.vis_path();
    return ::HIR::Publicity::new_priv( ::HIR::SimplePath( (ap->crate == "" ? g_crate_name : ap->crate), ap->nodes ) );
}

::HIR::GenericParams LowerHIR_GenericParams(const ::AST::GenericParams& gp, bool* self_is_sized)
{
    ::HIR::GenericParams    rv;

    for(const auto& param : gp.m_params)
    {
        TU_MATCH_HDRA( (param), {)
        TU_ARMA(None, _) {
            }
        TU_ARMA(Lifetime, lft_def) {
            rv.m_lifetimes.push_back( HIR::LifetimeDef { lft_def.name().name } );
            }
        TU_ARMA(Type, tp) {
            rv.m_types.push_back({ tp.name(), LowerHIR_Type(tp.get_default()), true });
            }
        TU_ARMA(Value, tp) {
            rv.m_values.push_back(HIR::ValueParamDef { tp.name().name, LowerHIR_Type(tp.type()), tp.default_value() ? LowerHIR_Expr(tp.default_value()) : HIR::ExprPtr() });
            }
        }
    }

    for(const auto& bound : gp.m_bounds )
    {
        TU_MATCH_HDRA( (bound), {)
        TU_ARMA(None, e) {
            }
        TU_ARMA(Lifetime, e) {
            rv.m_bounds.push_back(::HIR::GenericBound::make_Lifetime({
                LowerHIR_LifetimeRef(e.test),
                LowerHIR_LifetimeRef(e.bound)
                }));
            }
        TU_ARMA(TypeLifetime, e) {
            rv.m_bounds.push_back(::HIR::GenericBound::make_TypeLifetime({
                LowerHIR_Type(e.type),
                LowerHIR_LifetimeRef(e.bound)
                }));
            }
        TU_ARMA(IsTrait, e) {
            //const auto& sp = e.span;
            auto type = LowerHIR_Type(e.type);

            // TODO: Check if this trait is `Sized` and ignore if it is? (It's a useless bound)

            if( !e.outer_hrbs.empty() && !e.inner_hrbs.empty() ) {
                // NOTE: rustc doesn't allow this (E0316)
                TODO(bound.span, "Handle two layers of HRBs in a bound");
            }

            auto bound_trait_path = LowerHIR_TraitPath(bound.span, e.trait, e.inner_hrbs, /*allow_bounds=*/true);
            auto tp_bounds = mv$(bound_trait_path.m_trait_bounds);
            bound_trait_path.m_trait_bounds.clear();

            // 1.90 added some traits that imply ?Sized
            if( bound_trait_path.m_path.m_path == path_PointeeSized || bound_trait_path.m_path.m_path == path_MetadataSized ) {
                if( const auto* ge = type.data().opt_Generic() )
                {
                    if( ge->binding == GENERIC_Self ) {
                        *self_is_sized = false;
                    }
                    else {
                        auto idx = ge->idx();
                        ASSERT_BUG(bound.span, idx < rv.m_types.size(), "Bounded type out of bounds: " << ge->binding << " " << type);
                        rv.m_types[idx].m_is_sized = false;
                    }
                }
            }

            rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({
                box$(LowerHIR_HigherRankedBounds(e.outer_hrbs)),
                type.clone(),
                mv$(bound_trait_path)
                }));

            for(auto& bound : tp_bounds)
            {
                const auto& name = bound.first;
                const auto& src_trait = bound.second.source_trait;
                const auto& params = bound.second.aty_params;
                for(auto& trait : bound.second.traits)
                {
                    rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({
                        box$(LowerHIR_HigherRankedBounds(e.outer_hrbs)),
                        ::HIR::TypeRef::new_path( ::HIR::Path(type.clone(), src_trait.clone(), name, params.clone()), {} ),
                        std::move(trait)
                        }));
                }
                bound.second.traits.clear();
            }
            }
        TU_ARMA(MaybeTrait, e) {
            auto type = LowerHIR_Type(e.type);
            if( ! type.data().is_Generic() )
                BUG(bound.span, "MaybeTrait on non-param - " << type);
            const auto& ge = type.data().as_Generic();
            const auto& param_name = ge.name;
            unsigned param_idx;
            if( ge.binding == 0xFFFF ) {
                if( !self_is_sized ) {
                    BUG(bound.span, "MaybeTrait on parameter on Self when not allowed");
                }
                param_idx = 0xFFFF;
            }
            else {
                param_idx = ::std::find_if( rv.m_types.begin(), rv.m_types.end(), [&](const auto& x) { return x.m_name == param_name; } ) - rv.m_types.begin();
                if( param_idx >= rv.m_types.size() ) {
                    BUG(bound.span, "MaybeTrait on parameter not in parameter list (#" << ge.binding << " " << param_name << ")");
                }
            }

            // Compare with list of known default traits (just Sized atm) and set a marker
            auto trait = LowerHIR_GenericPath(bound.span, e.trait, FromAST_PathClass::Type);
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
            }
        TU_ARMA(NotTrait, e) {
            TODO(bound.span, "Negative trait bounds");
            }
        TU_ARMA(Equality, e) {
            rv.m_bounds.push_back(::HIR::GenericBound::make_TypeEquality({
                LowerHIR_Type(e.type),
                LowerHIR_Type(e.replacement)
                }));
            }
        }
    }

    return rv;
}

::HIR::Path LowerHIR_Pattern_Path(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc)
{
    if(const auto* be = path.m_bindings.type.binding.opt_TypeParameter())
    {
        if( be->slot == GENERIC_Self )
        {
            // HACK: Return `<Self>::` (to be expanded later on)
            return ::HIR::Path(HIR::TypeRef::new_self(), "");
        }
    }
    return LowerHIR_Path(sp, path, pc);
}

namespace {
    ::HIR::PatternBinding::Type convert_binding_type(::AST::PatternBinding::Type pbt)
    {
        switch(pbt)
        {
        case ::AST::PatternBinding::Type::MOVE:     return ::HIR::PatternBinding::Type::Move;
        case ::AST::PatternBinding::Type::REF:      return ::HIR::PatternBinding::Type::Ref;
        case ::AST::PatternBinding::Type::MUTREF:   return ::HIR::PatternBinding::Type::MutRef;
        }
        throw "";
    }
}
::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat)
{
    TRACE_FUNCTION_F("@" << pat.span() << " pat = " << pat);

    std::vector<::HIR::PatternBinding>  bindings;
    for(const auto& pb : pat.bindings())
    {
        bindings.push_back( ::HIR::PatternBinding(pb.m_mutable, convert_binding_type(pb.m_type), pb.m_name.name, pb.m_slot) );
    }

    struct H {
        static ::std::vector< ::HIR::Pattern>   lowerhir_patternvec(const ::std::vector< ::AST::Pattern>& sub_patterns) {
            ::std::vector< ::HIR::Pattern>  rv;
            for(const auto& sp : sub_patterns)
                rv.push_back( LowerHIR_Pattern(sp) );
            return rv;
        }
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
            case CORETYPE_F16:  return ::HIR::CoreType::F16;
            case CORETYPE_F32:  return ::HIR::CoreType::F32;
            case CORETYPE_F64:  return ::HIR::CoreType::F64;
            case CORETYPE_F128: return ::HIR::CoreType::F128;
            default:
                BUG(sp, "Unknown type for float literal in pattern - " << ct );
            }
        }
        static ::HIR::Pattern::Value lowerhir_pattern_value(const Span& sp, const ::AST::Pattern::Value& v) {
            TU_MATCH_HDRA((v), {)
            TU_ARMA(Invalid, e) {
                BUG(sp, "Encountered Invalid value in Pattern");
            }
            TU_ARMA(Integer, e) {
                return ::HIR::Pattern::Value::make_Integer({
                    H::get_int_type(sp, e.type),
                    e.value
                    });
            }
            TU_ARMA(Float, e) {
                return ::HIR::Pattern::Value::make_Float({
                    H::get_float_type(sp, e.type),
                    e.value
                    });
            }
            TU_ARMA(String, e) {
                return ::HIR::Pattern::Value::make_String(e);
            }
            TU_ARMA(ByteString, e) {
                return ::HIR::Pattern::Value::make_ByteString({e.v});
            }
            TU_ARMA(Named, e) {
                return ::HIR::Pattern::Value::make_Named({ LowerHIR_Pattern_Path(sp, e, FromAST_PathClass::Value), nullptr });
            }
            }
            throw "BUGCHECK: Reached end of LowerHIR_Pattern::H::lowerhir_pattern_value";
        }
    };

    TU_MATCH_HDRA( (pat.data()), {)
    TU_ARMA(MaybeBind, e) {
        BUG(pat.span(), "Encountered MaybeBind pattern");
        }
    TU_ARMA(Macro, e) {
        BUG(pat.span(), "Encountered Macro pattern");
        }
    TU_ARMA(Any, e)
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Any({})
            };
    TU_ARMA(Box, e)
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Box({
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
    TU_ARMA(Ref, e)
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Ref({
                (e.mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared),
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
    TU_ARMA(Tuple, e) {
        auto leading  = H::lowerhir_patternvec( e.start );
        auto trailing = H::lowerhir_patternvec( e.end   );

        if( e.has_wildcard )
        {
            return ::HIR::Pattern(
                mv$(bindings),
                ::HIR::Pattern::Data::make_SplitTuple({
                    mv$(leading), mv$(trailing)
                    })
                );
        }
        else
        {
            assert( trailing.size() == 0 );
            return ::HIR::Pattern(
                mv$(bindings),
                ::HIR::Pattern::Data::make_Tuple({
                    mv$(leading)
                    })
                );
        }
        }
    ///
    /// Named tuple pattern
    /// 
    TU_ARMA(StructTuple, e) {
        auto leading  = H::lowerhir_patternvec( e.tup_pat.start );
        auto trailing = H::lowerhir_patternvec( e.tup_pat.end   );

        if( !e.tup_pat.has_wildcard ) {
            assert( trailing.size() == 0 );
        }

        return ::HIR::Pattern(
            mv$(bindings),
            ::HIR::Pattern::Data::make_PathTuple({
                LowerHIR_Pattern_Path(pat.span(), e.path, FromAST_PathClass::Value),
                ::HIR::Pattern::PathBinding(),
                mv$(leading),
                e.tup_pat.has_wildcard,
                mv$(trailing),
                0 // Total size unknown still
                })
            );
        }
    ///
    /// Struct pattern
    /// 
    TU_ARMA(Struct, e) {
        ::std::vector< ::std::pair< RcString, ::HIR::Pattern> > sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( ::std::make_pair(sp.name, LowerHIR_Pattern(sp.pat)) );

        // No sub-patterns, no `..`, and the VALUE binding points to an enum variant
        if( e.sub_patterns.empty() /*&& !e.is_exhaustive*/ ) {
            if( /*const auto* pbp =*/ e.path.m_bindings.value.binding.opt_EnumVar() ) {
                return ::HIR::Pattern {
                    mv$(bindings),
                    ::HIR::Pattern::Data::make_PathNamed({
                        LowerHIR_GenericPath(pat.span(), e.path, FromAST_PathClass::Value),
                        //::HIR::Pattern::PathBinding::make_Enum({ pbp->hir, pbp->idx }),
                        ::HIR::Pattern::PathBinding(),
                        mv$(sub_patterns),
                        e.is_exhaustive
                        })
                    };
            }
        }

        return ::HIR::Pattern(
            mv$(bindings),
            ::HIR::Pattern::Data::make_PathNamed({
                LowerHIR_Pattern_Path(pat.span(), e.path, FromAST_PathClass::Type),
                ::HIR::Pattern::PathBinding(),
                mv$(sub_patterns),
                e.is_exhaustive
                })
            );
        }

    TU_ARMA(Value, e) {
        if( e.end.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(bindings),
                ::HIR::Pattern::Data::make_Value({
                    H::lowerhir_pattern_value(pat.span(), e.start)
                    })
                };
        }
        else if( e.start.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(bindings),
                ::HIR::Pattern::Data::make_Range({
                    {},
                    box$(H::lowerhir_pattern_value(pat.span(), e.end)),
                    true
                    })
                };
        }
        else {
            return ::HIR::Pattern {
                mv$(bindings),
                ::HIR::Pattern::Data::make_Range({
                    box$(H::lowerhir_pattern_value(pat.span(), e.start)),
                    box$(H::lowerhir_pattern_value(pat.span(), e.end)),
                    true
                    })
                };
        }
        }
    TU_ARMA(ValueLeftInc, e) {
        if( e.end.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(bindings),
                ::HIR::Pattern::Data::make_Range({
                    box$(H::lowerhir_pattern_value(pat.span(), e.start)),
                    {},
                    false
                    })
            };
        }
        if( e.start.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(bindings),
                ::HIR::Pattern::Data::make_Range({
                    {},
                    box$(H::lowerhir_pattern_value(pat.span(), e.end)),
                    false
                    })
            };
        }
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Range({
                box$(H::lowerhir_pattern_value(pat.span(), e.start)),
                box$(H::lowerhir_pattern_value(pat.span(), e.end)),
                false
                })
        };
        }
    TU_ARMA(Slice, e) {
        ::std::vector< ::HIR::Pattern>  leading;
        for(const auto& sp : e.sub_pats)
            leading.push_back( LowerHIR_Pattern(sp) );
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Slice({
                mv$(leading)
                })
            };
        }
    TU_ARMA(SplitSlice, e) {
        ::std::vector< ::HIR::Pattern>  leading;
        for(const auto& sp : e.leading)
            leading.push_back( LowerHIR_Pattern(sp) );

        ::std::vector< ::HIR::Pattern>  trailing;
        for(const auto& sp : e.trailing)
            trailing.push_back( LowerHIR_Pattern(sp) );

        auto extra_bind = e.extra_bind.is_valid()
            ? ::HIR::PatternBinding(false, convert_binding_type(e.extra_bind.m_type), e.extra_bind.m_name.name, e.extra_bind.m_slot)
            : ::HIR::PatternBinding()
            ;

        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_SplitSlice({
                mv$(leading),
                mv$(extra_bind),
                mv$(trailing)
                })
            };
        }
    TU_ARMA(Or, e) {
        ::std::vector< ::HIR::Pattern>  subpats;
        for(const auto& sp : e)
            subpats.push_back( LowerHIR_Pattern(sp) );
        return ::HIR::Pattern {
            mv$(bindings),
            ::HIR::Pattern::Data::make_Or(mv$(subpats))
            };
        }
    }
    throw "unreachable";
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

::HIR::SimplePath LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc, bool allow_final_generic)
{
    if(!allow_final_generic) {
        ASSERT_BUG(sp, path.m_class.is_Absolute(), "Encountered non-Absolute path when creating ::HIR::SimplePath");
        if( path.m_class.as_Absolute().nodes.size() > 0 )
        {
            ASSERT_BUG(sp, path.m_class.as_Absolute().nodes.back().args().is_empty(), "Encountered path with parameters when creating ::HIR::SimplePath");
        }
    }
    else {
        ASSERT_BUG(sp, path.m_class.is_Absolute(), "Encountered non-Absolute path when creating ::HIR::GenericPath");
    }

    const AST::AbsolutePath* ap = nullptr;
    switch(pc)
    {
    case FromAST_PathClass::Value:
        ASSERT_BUG(sp, !path.m_bindings.value.is_Unbound(), "Encountered unbound value path - " << path);
        ap = &path.m_bindings.value.path;
        break;
    case FromAST_PathClass::Type:
        ASSERT_BUG(sp, !path.m_bindings.type.is_Unbound(), "Encountered unbound type path - " << path);
        ap = &path.m_bindings.type.path;
        break;
    case FromAST_PathClass::Macro:
        ASSERT_BUG(sp, !path.m_bindings.macro.is_Unbound(), "Encountered unbound macro path - " << path);
        ap = &path.m_bindings.macro.path;
        break;
    }
    assert(ap);
    return ::HIR::SimplePath( (ap->crate == "" ? g_crate_name : ap->crate), ap->nodes );
}
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc)
{
    ::HIR::PathParams   params;

    size_t num_lft = 0;
    size_t num_ty = 0;
    size_t num_val = 0;
    
    for(const auto& param : src_params.m_entries) {
        TU_MATCH_HDRA( (param), {)
        TU_ARMA(Null, ty) {
            }
        TU_ARMA(Lifetime, lft) {
            num_lft ++;
            }
        TU_ARMA(Type, ty) {
            num_ty ++;
            }
        TU_ARMA(Value, iv) {
            num_val ++;
            }
        TU_ARMA(AssociatedTyEqual, ty) {}
        TU_ARMA(AssociatedTyBound, ty) {}
        }
    }

    params.m_lifetimes.reserve_init(num_lft);
    params.m_types.reserve_init(num_ty);
    params.m_values.reserve_init(num_val);
    for(const auto& param : src_params.m_entries) {
        TU_MATCH_HDRA( (param), {)
        TU_ARMA(Null, ty) {
            }
        TU_ARMA(Lifetime, lft) {
            params.m_lifetimes.push_back(LowerHIR_LifetimeRef(lft));
            }
        TU_ARMA(Type, ty) {
            params.m_types.push_back( LowerHIR_Type(ty) );
            }
        TU_ARMA(Value, iv) {
            const AST::ExprNode*    node = &*iv;
            if( const auto* e = dynamic_cast<const AST::ExprNode_Block*>(node) ) {
                if( e->m_yields_final_value && e->m_nodes.size() == 1 ) {
                    node = e->m_nodes.back().get();
                }
            }
            // TODO: Explicitly handle each expected variant... or add a proper consteval expression
            if( const auto* e = dynamic_cast<const AST::ExprNode_NamedValue*>(node) ) {
                if( e->m_path.is_trivial() ) {
                    const auto& b = e->m_path.m_bindings.value.binding;
                    ASSERT_BUG(sp, b.is_Generic(), "Trivial path not type parameter - " << e->m_path << " - " << b.tag_str());
                    const auto& param = b.as_Generic();
                    params.m_values.push_back( HIR::GenericRef(e->m_path.as_trivial(), param.index) );
                    break ;
                }
            }
            params.m_values.push_back( std::make_unique<HIR::ConstGeneric_Unevaluated>(LowerHIR_ExprNode(*iv)) );
            }
        TU_ARMA(AssociatedTyEqual, ty) {
            if( !allow_assoc )
                BUG(sp, "Encountered path parameters with associated type bounds where they are not allowed");
            }
        TU_ARMA(AssociatedTyBound, ty) {
            if( !allow_assoc )
                BUG(sp, "Encountered path parameters with associated type bounds where they are not allowed");
            }
        }
    }

    return params;
}
::HIR::GenericPath LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc, bool allow_assoc)
{
    if(const auto* e = path.m_class.opt_Absolute())
    {
        auto simpepath = LowerHIR_SimplePath(sp, path, pc, /*allow_params*/true);
        ::HIR::PathParams   params = LowerHIR_PathParams(sp, e->nodes.back().args(), allow_assoc);
        auto rv = ::HIR::GenericPath(mv$(simpepath), mv$(params));
        DEBUG(path << " => " << rv);
        return rv;
    }
    else {
        if(const auto* e = path.m_class.opt_UFCS()) {
            DEBUG(path);
            if( !e->type ) {
            }
            //else if( e->trait ) {
            //}
            else if( ! e->nodes.empty() ) {
            }
            else if( !e->type->m_data.is_Path() ) {
            }
            else {
                // HACK: `Self` replacement
                ASSERT_BUG(sp, pc == FromAST_PathClass::Type, "`Self` used in value context");
                return LowerHIR_GenericPath(sp, *e->type->m_data.as_Path(), pc, false);
            }
        }

        BUG(sp, "Encountered non-Absolute path when creating ::HIR::GenericPath - " << path);
    }
}

::HIR::GenericParams LowerHIR_HigherRankedBounds(const AST::HigherRankedBounds& hrbs)
{
    HIR::GenericParams  params;
    for(const auto& lft_def : hrbs.m_lifetimes)
        params.m_lifetimes.push_back(HIR::LifetimeDef { lft_def.name().name });
    return params;
}
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path, const AST::HigherRankedBounds& hrbs, bool ignore_bounds/*=false*/)
{
    DEBUG(hrbs << " " << path);
    ::HIR::TraitPath    rv {
        hrbs.empty() ? nullptr : box$(LowerHIR_HigherRankedBounds(hrbs)), // m_hrtbs
        LowerHIR_GenericPath(sp, path, FromAST_PathClass::Type, /*allow_assoc=*/true),
        {},
        {},
        nullptr
        };
    // HACK: If the path is from `Fn(Foo)` flag it for lifetime elision.
    // - Matching hack in `lifetime_elision.cpp` `visit_traitpath`
    if( !rv.m_hrtbs && path.nodes().back().args().m_is_paren ) {
        HIR::GenericParams  params;
        rv.m_hrtbs = box$(params);
    }
    if( rv.m_hrtbs && path.nodes().back().args().m_is_paren ) {
        rv.m_hrtbs->m_lifetimes.push_back(HIR::LifetimeDef { "#apply_elision" });
    }

    if(rv.m_hrtbs) {
        DEBUG("HRLS = " << rv.m_hrtbs->fmt_args());
    }
    else {
        DEBUG("No HRLS");
    }

    struct H {
        static ::HIR::GenericPath find_source_trait(
            const Span& sp,
            const ::HIR::GenericPath& path, const AST::PathBinding_Type::Data_Trait& pbe, const RcString& name,
            const Monomorphiser& ms
        )
        {
            TRACE_FUNCTION_F(path);
            if(pbe.hir)
            {
                assert(pbe.hir);
                const auto& trait = *pbe.hir;

                auto it = trait.m_types.find(name);
                if(it != trait.m_types.end()) {
                    return ms.monomorph_genericpath(sp, path, /*allow_infer=*/false);
                }
                auto cb = MonomorphStatePtr(&ty_Self, &path.m_params, nullptr);
                for(const auto& st : trait.m_all_parent_traits)
                {
                    // NOTE: st.m_trait_ptr isn't populated yet
                    const auto& t = g_crate_ptr->get_trait_by_path(sp, st.m_path.m_path);

                    auto it = t.m_types.find(name);
                    if(it != t.m_types.end()) {
                        // Monomorphse into outer scope, then run the outer monomorph
                        auto p = cb.monomorph_genericpath(sp, st.m_path, /*allow_infer=*/false);
                        return ms.monomorph_genericpath(sp, p, /*allow_infer=*/false);
                    }
                }
            }
            else if( pbe.trait_ )
            {
                assert(pbe.trait_);
                const auto& trait = *pbe.trait_;
                for(const auto& i : trait.items())
                {
                    if( i.data.is_Type() && i.name == name ) {
                        // Return current path.
                        return ms.monomorph_genericpath(sp, path, /*allow_infer=*/false);
                    }
                }

                auto cb = MonomorphStatePtr(&ty_Self, &path.m_params, nullptr);
                for( const auto& st : trait.supertraits() )
                {
                    auto b = LowerHIR_TraitPath(sp, *st.ent.path, st.ent.hrbs, true);
                    auto rv = H::find_source_trait(sp, b.m_path, st.ent.path->m_bindings.type.binding.as_Trait(), name, cb);
                    if(rv != HIR::GenericPath())
                        return ms.monomorph_genericpath(sp, rv, /*allow_infer=*/false);
                }
            }
            else
            {
                BUG(sp, "Unbound path");
            }
            return ::HIR::GenericPath();
        }
        static std::pair<RcString,HIR::PathParams> get_aty_node(const Span& sp, const ::AST::PathNode& pn)
        {
            auto args = LowerHIR_PathParams(sp, pn.args(), false);
            if( args.has_params() ) {
                TODO(sp, "Handle ATYs with args");
            }
            return std::make_pair(pn.name(), std::move(args));
        }
    };

    for(const auto& e : path.nodes().back().args().m_entries)
    {
        ThinVector<HIR::LifetimeRef>   lfts;
        TU_MATCH_HDRA( (e), {)
        TU_ARMA(Null, _) {}
        TU_ARMA(Lifetime, _) {}
        TU_ARMA(Type, _) {}
        TU_ARMA(Value, _) {}
        TU_ARMA(AssociatedTyEqual, assoc) {
            auto name_args = H::get_aty_node(sp, assoc.first);
            auto src_trait = H::find_source_trait(sp, rv.m_path, path.m_bindings.type.binding.as_Trait(), name_args.first, MonomorphiserNop());
            DEBUG("src_trait = " << src_trait << " for " << assoc.first);
            rv.m_type_bounds.insert(::std::make_pair( name_args.first, ::HIR::TraitPath::AtyEqual {
                std::move(src_trait), std::move(name_args.second), LowerHIR_Type(assoc.second)
            } ));
            }
        TU_ARMA(AssociatedTyBound, assoc) {
            if( !ignore_bounds )
            {
                ERROR(sp, E0000, "Associated type trait bounds not allowed here - " << path);
            }
            else
            {
                auto name_args = H::get_aty_node(sp, assoc.first);
                auto src_trait = H::find_source_trait(sp, rv.m_path, path.m_bindings.type.binding.as_Trait(), name_args.first, MonomorphiserNop());
                DEBUG("src_trait = " << src_trait << " for " << assoc.first);
                //if(src_trait == ::HIR::GenericPath())
                //    ERROR(sp, E0000, "Unable to find source trait for " << b->first << " in " << bound_trait_path.m_path);
                auto it = rv.m_trait_bounds.insert(std::make_pair(name_args.first, ::HIR::TraitPath::AtyBound { std::move(src_trait), std::move(name_args.second), {} }));
                for(const auto& trait : assoc.second) {
                    it.first->second.traits.push_back( LowerHIR_TraitPath(sp, trait, {}, /*ignore_bounds*/false) );
                }

            }
            }
        }
    }

    return rv;
}
::HIR::Path LowerHIR_Path(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc)
{
    TU_MATCH_HDRA( (path.m_class), {)
    TU_ARMA(Invalid, e) {
        BUG(sp, "BUG: Encountered Invalid path in LowerHIR_Path");
        }
    TU_ARMA(Local, e) {
        TODO(sp, "What to do with Path::Class::Local in LowerHIR_Path - " << path);
        }
    TU_ARMA(Relative, e) {
        BUG(sp, "Encountered `Relative` path in LowerHIR_Path - " << path);
        }
    TU_ARMA(Self, e) {
        BUG(sp, "Encountered `Self` path in LowerHIR_Path - " << path);
        }
    TU_ARMA(Super, e) {
        BUG(sp, "Encountered `Super` path in LowerHIR_Path - " << path);
        }
    TU_ARMA(Absolute, e) {
        return ::HIR::Path( LowerHIR_GenericPath(sp, path, pc) );
        }
    TU_ARMA(UFCS, e) {
        if( e.nodes.size() == 0 )
        {
            if( !(!e.trait || e.trait->is_valid()) )
                TODO(sp, "Handle UFCS w/ trait and no nodes - " << path);
            auto type = LowerHIR_Type(*e.type);
            ASSERT_BUG(sp, type.data().is_Path(), "No nodes and non-Path type - " << path);
            return mv$(type.get_unique().as_Path().path);
        }
        if( e.nodes.size() > 1 )
            TODO(sp, "Handle UFCS with multiple nodes - " << path);
        // - No associated type bounds allowed in UFCS paths
        auto params = LowerHIR_PathParams(sp, e.nodes.front().args(), /*allow_assoc*/false);
        /*if( ! e.trait )
        {
            auto type = LowerHIR_Type(*e.type);
            if( type.data().is_Generic() ) {
                BUG(sp, "Generics can't be used with UfcsInherent - " << path);
            }
            return ::HIR::Path(::HIR::Path::Data::make_UfcsInherent({
                mv$(type),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else*/ if( !e.trait || !e.trait->is_valid() )
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsUnknown({
                LowerHIR_Type(*e.type),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                LowerHIR_Type(*e.type),
                LowerHIR_GenericPath(sp, *e.trait, FromAST_PathClass::Type),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        }
    }
    throw "BUGCHECK: Reached end of LowerHIR_Path";
}

namespace {
    struct ImplTraitSource {
        const ::HIR::ItemPath*  path;
        const ::HIR::GenericParams* params_outer;
        const ::HIR::GenericParams* params_inner = nullptr;

        ImplTraitSource(const ::HIR::ItemPath* path, const ::HIR::GenericParams* params_outer, const ::HIR::GenericParams* params_inner=nullptr)
            : path(path)
            , params_outer(params_outer)
            , params_inner(params_inner)
        {}
        ImplTraitSource(): path(nullptr), params_outer(nullptr) {}
    } g_impl_trait_source;
}

::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty)
{
    TU_MATCH_HDRA( (ty.m_data), {)
    TU_ARMA(None, e) {
        BUG(ty.span(), "TypeData::None");
        }
    TU_ARMA(Bang, e) {
        return ::HIR::TypeRef::new_diverge();
        }
    TU_ARMA(Any, e) {
        return ::HIR::TypeRef();
        }
    TU_ARMA(Unit, e) {
        return ::HIR::TypeRef::new_unit();
        }
    TU_ARMA(Macro, e) {
        BUG(ty.span(), "TypeData::Macro");
        }
    TU_ARMA(Primitive, e) {
        switch(e.core_type)
        {
        case CORETYPE_BOOL: return ::HIR::TypeRef( ::HIR::CoreType::Bool );
        case CORETYPE_CHAR: return ::HIR::TypeRef( ::HIR::CoreType::Char );
        case CORETYPE_STR : return ::HIR::TypeRef( ::HIR::CoreType::Str );
        case CORETYPE_F16:  return ::HIR::TypeRef( ::HIR::CoreType::F16 );
        case CORETYPE_F32:  return ::HIR::TypeRef( ::HIR::CoreType::F32 );
        case CORETYPE_F64:  return ::HIR::TypeRef( ::HIR::CoreType::F64 );
        case CORETYPE_F128: return ::HIR::TypeRef( ::HIR::CoreType::F128 );

        case CORETYPE_I8 :  return ::HIR::TypeRef( ::HIR::CoreType::I8 );
        case CORETYPE_U8 :  return ::HIR::TypeRef( ::HIR::CoreType::U8 );
        case CORETYPE_I16:  return ::HIR::TypeRef( ::HIR::CoreType::I16 );
        case CORETYPE_U16:  return ::HIR::TypeRef( ::HIR::CoreType::U16 );
        case CORETYPE_I32:  return ::HIR::TypeRef( ::HIR::CoreType::I32 );
        case CORETYPE_U32:  return ::HIR::TypeRef( ::HIR::CoreType::U32 );
        case CORETYPE_I64:  return ::HIR::TypeRef( ::HIR::CoreType::I64 );
        case CORETYPE_U64:  return ::HIR::TypeRef( ::HIR::CoreType::U64 );

        case CORETYPE_I128: return ::HIR::TypeRef( ::HIR::CoreType::I128 );
        case CORETYPE_U128: return ::HIR::TypeRef( ::HIR::CoreType::U128 );

        case CORETYPE_INT:  return ::HIR::TypeRef( ::HIR::CoreType::Isize );
        case CORETYPE_UINT: return ::HIR::TypeRef( ::HIR::CoreType::Usize );
        case CORETYPE_ANY:
            TODO(ty.span(), "TypeData::Primitive - CORETYPE_ANY");
        case CORETYPE_INVAL:
            BUG(ty.span(), "TypeData::Primitive - CORETYPE_INVAL");
        }
        }
    TU_ARMA(Tuple, e) {
        ::HIR::TypeData::Data_Tuple v;
        for( const auto& st : e.inner_types )
        {
            v.push_back( LowerHIR_Type(st) );
        }
        return ::HIR::TypeRef::new_tuple(mv$(v));
        }
    TU_ARMA(Borrow, e) {
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_borrow( cl, LowerHIR_Type(*e.inner), LowerHIR_LifetimeRef(e.lifetime) );
        }
    TU_ARMA(Pointer, e) {
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_pointer( cl, LowerHIR_Type(*e.inner) );
        }
    TU_ARMA(Array, e) {
        auto inner = LowerHIR_Type(*e.inner);
        if( e.size ) {
            // If the size expression is an unannotated or usize integer literal, don't bother converting the expression
            if( const auto* ptr = dynamic_cast<const ::AST::ExprNode_Integer*>(&*e.size) )
            {
                if( ptr->m_datatype == CORETYPE_UINT || ptr->m_datatype == CORETYPE_ANY )
                {
                    // TODO: Chage the HIR format to support very large arrays
                    if( ptr->m_value >= U128(UINT64_MAX) ) {
                        ERROR(ty.span(), E0000, "Array size out of bounds - 0x" << ::std::hex << ptr->m_value << " > 0x" << UINT64_MAX << " in " << ::std::dec << ty);
                    }
                    return ::HIR::TypeRef::new_array( mv$(inner), ptr->m_value.truncate_u64() );
                }
            }
            if( const auto* ptr = dynamic_cast<const ::AST::ExprNode_NamedValue*>(&*e.size) )
            {
                if( ptr->m_path.is_trivial() )
                {
                    auto gr = HIR::GenericRef(ptr->m_path.as_trivial(), ptr->m_path.m_bindings.value.binding.as_Generic().index);
                    return ::HIR::TypeRef::new_array( mv$(inner), HIR::ConstGeneric(mv$(gr)) );
                }
            }

            return ::HIR::TypeRef::new_array( mv$(inner), HIR::ConstGeneric::make_Unevaluated(std::make_unique<HIR::ConstGeneric_Unevaluated>(LowerHIR_Expr(e.size))) );
        }
        else {
            return ::HIR::TypeRef::new_array( mv$(inner), HIR::ConstGeneric::make_Infer({}) );
        }
        }
    TU_ARMA(Slice, e) {
        auto inner = LowerHIR_Type(*e.inner);
        return ::HIR::TypeRef::new_slice( mv$(inner) );
        }
    TU_ARMA(Path, e) {
        if(const auto* l = e->m_class.opt_Local()) {
            unsigned int slot;
            // NOTE: TypeParameter is unused
            if( const auto* p = e->m_bindings.type.binding.opt_TypeParameter() ) {
                slot = p->slot;
            }
            else {
                BUG(ty.span(), "Unbound local encountered in " << *e);
            }
            return ::HIR::TypeRef( l->name, slot );
        }
        else if( e->m_bindings.type.path.crate == CRATE_BUILTINS ) {
            return LowerHIR_Type(TypeRef(ty.span(), coretype_fromstring(e->m_bindings.type.path.nodes.back().c_str()) ));
        }
        else {
            return ::HIR::TypeRef::new_path( LowerHIR_Path(ty.span(), *e, FromAST_PathClass::Type), {} );
        }
        }
    TU_ARMA(TraitObject, e) {
        ::HIR::TypeData::Data_TraitObject  v;
        if( e.lifetimes.empty() ) {
            // Lifetime elision should have handled this?
        }
        else if( e.lifetimes.size() == 1 ) {
            v.m_lifetime = LowerHIR_LifetimeRef(e.lifetimes[0]);
        }
        else {
            BUG(ty.span(), "Handle multiple lifetimes on a trait object - " << ty);
        }
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << *t.path);
            const auto& tb = t.path->m_bindings.type.binding.as_Trait();
            assert( tb.trait_ || tb.hir );
            if( (tb.trait_ ? tb.trait_->is_marker() : tb.hir->m_is_marker) )
            {
                if( tb.hir ) {
                    DEBUG(tb.hir->m_values.size());
                }
                // TODO: If this has HRBs, what?
                v.m_markers.push_back( LowerHIR_GenericPath(ty.span(), *t.path, FromAST_PathClass::Type) );
            }
            else {
                // TraitPath -> GenericPath -> SimplePath
                if( !v.m_trait.m_path.m_path.components().empty() ) {
                    ERROR(ty.span(), E0000, "Multiple data traits in trait object - " << ty);
                }
                v.m_trait = LowerHIR_TraitPath(ty.span(), *t.path, t.hrbs);
            }
        }
        // Sort markers so downstream can compare properly
        ::std::sort(v.m_markers.begin(), v.m_markers.end());
        return ::HIR::TypeRef( ::HIR::TypeData::make_TraitObject( mv$(v) ) );
        }
    TU_ARMA(ErasedType, e) {
        ASSERT_BUG(ty.span(), e.traits.size() > 0, "ErasedType with no traits");

        // TODO: There can be associated type bounds, those need to be propagated

        ::std::vector< ::HIR::TraitPath>    traits;
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << *t.path);
            // TODO: Handle ATY bounds
            traits.push_back( LowerHIR_TraitPath(ty.span(), *t.path, t.hrbs, /*allow_aty_trait_bounds=*/true) );
        }
        bool is_sized = true;
        for(const auto& t : e.maybe_traits) {
            auto tp = LowerHIR_TraitPath(ty.span(), *t.path, t.hrbs, /*allow_aty_trait_bounds=*/true);
            if( tp.m_path.m_path == path_Sized ) {
                is_sized = false;
            }
            else {
                TODO(ty.span(), "Optional trait (not Sized) - " << ty);
            }
        }
        std::vector<::HIR::LifetimeRef>  lfts;
        if( e.lifetimes.size() == 0 )
        {
            // NOTE: This signals to the lifetime elision code
            //lfts.push_back(::HIR::LifetimeRef());
        }
        else
        {
            for(const auto& lft : e.lifetimes) {
                lfts.push_back( LowerHIR_LifetimeRef(lft) );
            }
        }
        ::HIR::TypeData_ErasedType_Inner    inner;
        if( g_impl_trait_source.path ) {
            if( g_impl_trait_source.params_inner && g_impl_trait_source.params_inner->is_generic() ) {
                TODO(ty.span(), "Handle multi-layered generic erased type (used in a GAT)");
            }
            inner = ::HIR::TypeData_ErasedType_Inner(::HIR::TypeData_ErasedType_Inner::Data_Alias {
                g_impl_trait_source.params_outer->make_nop_params(0),
                std::make_shared<HIR::TypeData_ErasedType_AliasInner>(*g_impl_trait_source.path, *g_impl_trait_source.params_outer)
                });
        }
        else {
            inner = ::HIR::TypeData_ErasedType_Inner::Data_Fcn { ::HIR::Path(::HIR::SimplePath()), 0 };  // Populated in bind, could be populated now?
        }
        return ::HIR::TypeRef( ::HIR::TypeData::Data_ErasedType {
            is_sized,
            mv$(traits),
            mv$(lfts),
            mv$(inner)
            } );
        }
    TU_ARMA(Function, e) {
        HIR::GenericParams  params;
        for(const auto& lft_def : e.info.hrbs.m_lifetimes)
            params.m_lifetimes.push_back(HIR::LifetimeDef { lft_def.name().name });
        ::std::vector< ::HIR::TypeRef>  args;
        for(const auto& arg : e.info.m_arg_types)
            args.push_back( LowerHIR_Type(arg) );
        ::HIR::TypeData_FunctionPointer f {
            mv$(params),
            e.info.is_unsafe,
            e.info.is_variadic,
            RcString::new_interned(e.info.m_abi),
            LowerHIR_Type(*e.info.m_rettype),
            mv$(args)
            };
        if( f.m_abi == "" )
            f.m_abi = RcString::new_interned(ABI_RUST);
        return ::HIR::TypeRef( mv$(f) );
        }
    TU_ARMA(Generic, e) {
        assert(e.index < 0x10000);
        return ::HIR::TypeRef(e.name, e.index);
        }
    }
    throw "BUGCHECK: Reached end of LowerHIR_Type";
}

::HIR::TypeAlias LowerHIR_TypeAlias(const HIR::ItemPath& p, const ::AST::TypeAlias& ta)
{
    assert(!g_impl_trait_source.path);
    auto params = LowerHIR_GenericParams(ta.params(), nullptr);
    g_impl_trait_source = ImplTraitSource(&p, &params);
    auto ty = LowerHIR_Type(ta.type());
    //if( auto* e = ty.data_mut().opt_ErasedType() ) {
    //    DEBUG("Flag type alias - " << &ty.data());
    //    e->m_inner = std::make_shared<HIR::TypeData_ErasedType_AliasInner>(p);
    //}
    g_impl_trait_source = ImplTraitSource();
    return ::HIR::TypeAlias { std::move(params), ::std::move(ty) };
}


namespace {
    template<typename T>
    ::HIR::VisEnt<T> new_visent(HIR::Publicity pub, T v) {
        return ::HIR::VisEnt<T> { pub, mv$(v) };
    }

    ::HIR::SimplePath get_parent_module(const ::HIR::ItemPath& p) {
        const ::HIR::ItemPath*  parent_ip = p.parent;
        assert(parent_ip);
        while(parent_ip->name && parent_ip->name[0] == '#')
        {
            parent_ip = parent_ip->parent;
            assert(parent_ip);
        }
        return parent_ip->get_simple_path();
    }
}

::HIR::Struct LowerHIR_Struct(const Span& sp, ::HIR::ItemPath path, const ::AST::Struct& ent, const ::AST::AttributeList& attrs)
{
    TRACE_FUNCTION_F(path);
    ::HIR::Struct::Data data;

    auto mod_path = get_parent_module(path);
    auto get_vis = [&](const AST::Visibility& vis) { return LowerHIR_Vis(mod_path, vis); };

    TU_MATCH_HDRA( (ent.m_data), {)
    TU_ARMA(Unit, e) {
        data = ::HIR::Struct::Data::make_Unit({});
        }
    TU_ARMA(Tuple, e) {
        ::HIR::Struct::Data::Data_Tuple fields;

        for(const auto& field : e.ents)
            fields.push_back( { get_vis(field.m_vis), LowerHIR_Type(field.m_type) } );

        data = ::HIR::Struct::Data::make_Tuple( mv$(fields) );
    }
    TU_ARMA(Struct, e) {
        ::HIR::Struct::Data::Data_Named fields;
        for(const auto& field : e.ents)
            fields.push_back( ::std::make_pair( field.m_name, new_visent( get_vis(field.m_vis), LowerHIR_Type(field.m_type)) ) );
        data = ::HIR::Struct::Data::make_Named( mv$(fields) );
        }
    }

    auto rv = ::HIR::Struct {
        LowerHIR_GenericParams(ent.params(), nullptr),
        ::HIR::Struct::Repr::Rust,
        mv$(data)
        };

    // Determine the repr
    {
        switch(ent.m_markings.repr)
        {
        case AST::Struct::Markings::Repr::Rust:
            rv.m_repr = ::HIR::Struct::Repr::Rust;
            break;
        case AST::Struct::Markings::Repr::C:
            rv.m_repr = ::HIR::Struct::Repr::C;
            break;
        case AST::Struct::Markings::Repr::Simd:
            rv.m_repr = ::HIR::Struct::Repr::Simd;
            //ASSERT_BUG(sp, ent.m_markings.max_field_align == 0, "packed() on simd?");
            break;
        case AST::Struct::Markings::Repr::Transparent:
            rv.m_repr = ::HIR::Struct::Repr::Transparent;
            ASSERT_BUG(sp, ent.m_markings.max_field_align == 0, "packed() on transparent?");
            break;
        }
        rv.m_forced_alignment = ent.m_markings.align_value;
        rv.m_max_field_alignment = ent.m_markings.max_field_align;
    }

    // #[rustc_nonnull_optimization_guaranteed]
    // TODO: OR, it's equal to the `non_zero` lang item
    if(attrs.get("rustc_nonnull_optimization_guaranteed"))
    {
        //ent.m_markings.scalar_valid_start_set = true;
        rv.m_struct_markings.is_nonzero = true;
    }
    if(ent.m_markings.scalar_valid_start_set)
    {
        if( ent.m_markings.scalar_valid_start == U128(1) ) {
            rv.m_struct_markings.is_nonzero = true;
        }
        else {
            //TODO(sp, "Handle #[rustc_layout_scalar_valid_range_start(" << ent.m_markings.scalar_valid_start << ")]");
        }
    }
    // TODO: Store the scalar valid range information for downstream
    if( ent.m_markings.scalar_valid_start_set || ent.m_markings.scalar_valid_end_set )
    {
        const HIR::TypeRef* ty = nullptr;
        const HIR::TypeRef* ty2 = nullptr;
        if( const auto* d = rv.m_data.opt_Named() ) {
            switch(d->size())
            {
            case 2:
                ty2 = &(*d)[1].second.ent;
            case 1:
                ty = &(*d)[0].second.ent;
                break;
            }
        }
        else if( const auto* d = rv.m_data.opt_Tuple() ) {
            if( d->size() == 1 )
                ty = &(*d)[0].ent;
            //TODO: Ensure that the other fields are ZSTs
        }
        else {
            // Invalid
        }
        if(!ty)
            ERROR(sp, E0000, "Invalid use of #[rustc_layout_scalar_valid_range_start] or #[rustc_layout_scalar_valid_range_end] on invalid struct");
        if(ty2)
        {
            //TODO: Ensure that this second field is PhantomData
        }

        uint64_t TGT_PTR_MAX = Target_GetPointerBits() == 64 ? UINT64_MAX : UINT32_MAX;
        U128    min = U128(0), max = U128(UINT64_MAX,UINT64_MAX);
        bool ignore = false;
        if( ty->data().is_Pointer() )
        {
            min = U128(0);
            max = U128(TGT_PTR_MAX);
        }
        else
        {
            // Check the type
            ::HIR::CoreType ct = HIR::CoreType::Str;
            if( ty->data().is_Primitive() )
                ct = ty->data().as_Primitive();
            switch(ct)
            {
            case ::HIR::CoreType::U8:   max = U128(0xFF);     break;
            case ::HIR::CoreType::U16:  max = U128(UINT16_MAX);   break;
            case ::HIR::CoreType::U32:  max = U128(UINT32_MAX);   break;
            case ::HIR::CoreType::U64:  max = U128(UINT64_MAX);   break;
            case ::HIR::CoreType::U128: break;
            case ::HIR::CoreType::Usize:  max = U128(TGT_PTR_MAX);   break;

            case ::HIR::CoreType::I8:   //max = 0x7F;     break;
            case ::HIR::CoreType::I16:  //max = INT16_MAX;   break;
            case ::HIR::CoreType::I32:  //max = INT32_MAX; break;
            case ::HIR::CoreType::I64:  //max = INT64_MAX;   break;
            case ::HIR::CoreType::I128: //ignore = true;  break;
            case ::HIR::CoreType::Isize:  //max = TGT_PTR_MAX/2+1;   break;
                // Downstream treats this as unsigned
                ignore = true;
                break;

            default:
                ignore = true;
                //ERROR(sp, E0000, "Invalid use of #[rustc_layout_scalar_valid_range_start] or #[rustc_layout_scalar_valid_range_end] on invalid type (must be an integer or pointer) - " << *ty);
                break;
            }
        }

        if(!ignore)
        {
            if( ent.m_markings.scalar_valid_start_set ) {
                if( ent.m_markings.scalar_valid_start < min ) {
                }
                //rv.m_struct_markings.bounded_min = true;
                //rv.m_struct_markings.bounded_min_value = ent.m_markings.scalar_valid_start;
            }
            if( ent.m_markings.scalar_valid_end_set ) {
                if( ent.m_markings.scalar_valid_end > max ) {
                }
                rv.m_struct_markings.bounded_max = true;
                rv.m_struct_markings.bounded_max_value = ent.m_markings.scalar_valid_end;
            }
        }
    }

    return rv;
}

::HIR::Enum LowerHIR_Enum(::HIR::ItemPath path, const ::AST::Enum& ent, const ::AST::AttributeList& attrs, ::std::function<void(RcString, ::HIR::Struct)> push_struct)
{
    // 1. Figure out what sort of enum this is (value or data)
    bool has_value = false;
    bool has_data = false;
    for(const auto& var : ent.variants())
    {
        if( TU_TEST1(var.m_data, Value, .m_value.is_valid()) )
        {
            has_value = true;
        }
        else if( var.m_data.is_Tuple() || var.m_data.is_Struct() )
        {
            has_data = true;
        }
        else
        {
            // Unit-like
            assert(var.m_data.is_Value());
        }
    }

    if( has_value && has_data )
    {
        ERROR(Span(), E0000, "Enum " << path << " has both value and data variants");
    }

    bool is_repr_c = ent.m_markings.is_repr_c;
    auto repr = ::HIR::Enum::Repr::Auto;
    switch(ent.m_markings.repr)
    {
    case ::AST::Enum::Markings::Repr::Rust: repr = ::HIR::Enum::Repr::Auto; break;
    case ::AST::Enum::Markings::Repr::U8 : repr = ::HIR::Enum::Repr::U8 ; break;
    case ::AST::Enum::Markings::Repr::U16: repr = ::HIR::Enum::Repr::U16; break;
    case ::AST::Enum::Markings::Repr::U32: repr = ::HIR::Enum::Repr::U32; break;
    case ::AST::Enum::Markings::Repr::U64: repr = ::HIR::Enum::Repr::U64; break;
    case ::AST::Enum::Markings::Repr::Usize: repr = ::HIR::Enum::Repr::Usize; break;
    case ::AST::Enum::Markings::Repr::I8 : repr = ::HIR::Enum::Repr::I8 ; break;
    case ::AST::Enum::Markings::Repr::I16: repr = ::HIR::Enum::Repr::I16; break;
    case ::AST::Enum::Markings::Repr::I32: repr = ::HIR::Enum::Repr::I32; break;
    case ::AST::Enum::Markings::Repr::I64: repr = ::HIR::Enum::Repr::I64; break;
    case ::AST::Enum::Markings::Repr::Isize: repr = ::HIR::Enum::Repr::Isize; break;
    }

    auto params = LowerHIR_GenericParams(ent.params(), nullptr);

    ::HIR::Enum::Class  data;
    if( ent.variants().size() > 0 && !has_data )
    {
        ::std::vector<::HIR::Enum::ValueVariant>    variants;
        for(const auto& var : ent.variants())
        {
            const auto& ve = var.m_data.as_Value();
            // TODO: Quick consteval on the expression?
            variants.push_back({
                var.m_name, LowerHIR_Expr(ve.m_value), 0
                });
        }

        data = ::HIR::Enum::Class::make_Value({ mv$(variants), false });
    }
    // NOTE: empty enums are encoded as empty Data enums
    else
    {
        ::std::vector<::HIR::Enum::DataVariant>    variants;
        for(const auto& var : ent.variants())
        {
            if( var.m_data.is_Value() )
            {
                // TODO: Should this make its own unit-like struct?
                variants.push_back({ var.m_name, false, ::HIR::TypeRef::new_unit() });
            }
            //else if( TU_TEST1(var.m_data, Tuple, m_sub_types.size() == 0) )
            //{
            //    variants.push_back({ var.m_name, false, ::HIR::TypeRef::new_unit() });
            //}
            //else if( TU_TEST1(var.m_data, Tuple, m_sub_types.size() == 1) )
            //{
            //    const auto& ty = var.m_data.as_Tuple().m_sub_types[0];
            //    variants.push_back({ var.m_name, false, LowerHIR_Type(ty) });
            //}
            else
            {
                ::HIR::Struct::Data data;
                if( const auto* ve = var.m_data.opt_Tuple() )
                {
                    ::HIR::Struct::Data::Data_Tuple fields;
                    for(const auto& field : ve->m_items)
                        fields.push_back( new_visent(::HIR::Publicity::new_global(), LowerHIR_Type(field.m_type)) );
                    data = ::HIR::Struct::Data::make_Tuple( mv$(fields) );
                }
                else if( const auto* ve = var.m_data.opt_Struct() )
                {
                    ::HIR::Struct::Data::Data_Named fields;
                    for(const auto& field : ve->m_fields)
                        fields.push_back( ::std::make_pair( field.m_name, new_visent(::HIR::Publicity::new_global(), LowerHIR_Type(field.m_type)) ) );
                    data = ::HIR::Struct::Data::make_Named( mv$(fields) );
                }
                else
                {
                    throw "";
                }

                auto ty_name = RcString::new_interned(FMT(path.name << "#" << var.m_name));
                push_struct(
                    ty_name,
                    ::HIR::Struct {
                        LowerHIR_GenericParams(ent.params(), nullptr),
                        ::HIR::Struct::Repr::Rust,
                        mv$(data)
                        }
                    );
                auto ty_ipath = path;
                ty_ipath.name = ty_name.c_str();
                auto ty_path = ty_ipath.get_full_path();
                // Add type params
                ty_path.m_data.as_Generic().m_params = params.make_nop_params(0);
                variants.push_back({ var.m_name, var.m_data.is_Struct(), ::HIR::TypeRef::new_path( mv$(ty_path), {} ) });
            }
        }

        switch(repr)
        {
        case ::HIR::Enum::Repr::Auto:
            break;
        default:
            // NOTE:
            // - librustc_llvm has `#[repr(C)] enum AttributePlace { Argument(u32), Function }`
            // - `rustc-1.19.0-src\src\vendor\idna\src\uts46.rs:33` has `#[repr(u16)]`
            //ERROR(Span(), E0000, "#[repr] not allowed on enums with data");

            // TODO: Save the repr for use in `trans/target.cpp`
            // https://github.com/rust-lang/rfcs/blob/master/text/2195-really-tagged-unions.md
            // - `repr(int)` packs the tag into the variants (which can be more efficient for alignment, with `Variant(u8, u16)`)
            // - `repr(C,int)` has the tag before variants (so will be less alignment efficient)
            break;
        }

        data = ::HIR::Enum::Class::make_Data( mv$(variants) );
    }

    return ::HIR::Enum {
        mv$(params),
        is_repr_c,
        repr,
        mv$(data)
        };
}
::HIR::Union LowerHIR_Union(::HIR::ItemPath path, const ::AST::Union& f, const ::AST::AttributeList& attrs)
{
    auto mod_path = get_parent_module(path);
    auto get_vis = [&](const AST::Visibility& vis) { return LowerHIR_Vis(mod_path, vis); };

    auto repr = ::HIR::Union::Repr::Rust;
    switch(f.m_markings.repr)
    {
    case ::AST::Union::Markings::Repr::Rust:    repr = ::HIR::Union::Repr::Rust;    break;
    case ::AST::Union::Markings::Repr::C   :    repr = ::HIR::Union::Repr::C   ;    break;
    case ::AST::Union::Markings::Repr::Transparent:    repr = ::HIR::Union::Repr::Transparent;    break;
    }

    ::HIR::Struct::Data::Data_Named variants;
    for(const auto& field : f.m_variants)
        variants.push_back( ::std::make_pair( field.m_name, new_visent(get_vis(field.m_vis), LowerHIR_Type(field.m_type)) ) );

    return ::HIR::Union {
        LowerHIR_GenericParams(f.m_params, nullptr),
        repr,
        mv$(variants)
        };
}
::HIR::Trait LowerHIR_Trait(::HIR::SimplePath trait_path, const ::AST::Trait& f)
{
    TRACE_FUNCTION_F(trait_path);
    trait_path.update_crate_name( g_crate_name );

    bool trait_reqires_sized = false;
    auto params = LowerHIR_GenericParams(f.params(), &trait_reqires_sized);

    ::HIR::LifetimeRef  lifetime;
    if( !f.lifetimes().empty() ) {
        ASSERT_BUG(f.lifetimes()[0].sp, f.lifetimes().size() == 1, "");
        lifetime = LowerHIR_LifetimeRef(f.lifetimes()[0].ent);
        DEBUG("Lifetime " << lifetime << " (" << f.lifetimes()[0].ent << " " << f.lifetimes()[0].ent.binding() << ")");
    }
    ::std::vector< ::HIR::TraitPath>    supertraits;
    for(const auto& st : f.supertraits()) {
        supertraits.push_back( LowerHIR_TraitPath(st.sp, *st.ent.path, st.ent.hrbs, true) );
        DEBUG("Supertrait " << supertraits.back());
    }
    ::HIR::Trait    rv {
        mv$(params),
        mv$(lifetime),
        mv$(supertraits)
        };

    // HACK: Add a bound of Self: ThisTrait for parts of typeck (TODO: Remove this, it's evil)
    {
        auto this_trait = ::HIR::GenericPath( trait_path );
        this_trait.m_params = rv.m_params.make_nop_params(0);
        rv.m_params.m_bounds.push_back( ::HIR::GenericBound::make_TraitBound({ {}, ty_Self.clone(), { {}, mv$(this_trait) } }) );
    }

    for(const auto& item : f.items())
    {
        auto trait_ip = ::HIR::ItemPath(trait_path);
        auto item_path = ::HIR::ItemPath( trait_ip, item.name.c_str() );

        TU_MATCH_HDRA( (item.data), {)
        default:
            BUG(item.span, "Encountered unexpected item type in trait");
        TU_ARMA(None, i) {
            // Ignore.
            }
        TU_ARMA(MacroInv, i) {
            // Ignore.
            }
        TU_ARMA(Type, i) {
            bool is_sized = true;
            ::std::vector< ::HIR::TraitPath>    trait_bounds;
            ::HIR::LifetimeRef  lifetime_bound;
            auto gps = LowerHIR_GenericParams(i.params(), &is_sized);

            auto self_bounds = LowerHIR_GenericParams(i.m_self_bounds, &is_sized);
            for(auto& b : self_bounds.m_bounds)
            {
                TU_MATCH_HDRA( (b), {)
                TU_ARMA(TypeLifetime, be) {
                    ASSERT_BUG(item.span, be.type.data().as_Generic().binding == GENERIC_Self, be.type);
                    lifetime_bound = mv$(be.valid_for);
                    }
                TU_ARMA(TraitBound, be) {
                    ASSERT_BUG(item.span, be.type.data().as_Generic().binding == GENERIC_Self, be.type);
                    trait_bounds.push_back( mv$(be.trait) );
                    }
                TU_ARMA(Lifetime, be) {
                    BUG(item.span, "Unexpected lifetime-lifetime bound on associated type");
                    }
                TU_ARMA(TypeEquality, be) {
                    BUG(item.span, "Unexpected type equality bound on associated type");
                    }
                }
            }
            rv.m_types.insert( ::std::make_pair(item.name, ::HIR::AssociatedType {
                mv$(gps),
                is_sized,
                mv$(lifetime_bound),
                mv$(trait_bounds),
                LowerHIR_Type(i.type())
                }) );
            }
        TU_ARMA(Function, i) {
            auto fcn = LowerHIR_Function(item_path, item.attrs, i, ty_Self);
            fcn.m_save_code = true;
            rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Function( mv$(fcn) )) );
            }
        TU_ARMA(Static, i) {
            if( i.s_class() == ::AST::Static::CONST )
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Constant(::HIR::Constant(
                    ::HIR::GenericParams {},
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    ))) );
            else {
                ::HIR::Linkage  linkage;
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Static(::HIR::Static(
                    mv$(linkage),
                    (i.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    ))) );
            }
            }
        }
    }

    rv.m_is_marker = f.is_marker();

    return rv;
}
::HIR::TraitAlias LowerHIR_TraitAlias(const Span& sp, ::HIR::ItemPath p, const ::AST::TraitAlias& f)
{
    bool trait_reqires_sized = false;

    HIR::TraitAlias ta;
    ta.m_params = LowerHIR_GenericParams(f.params, &trait_reqires_sized);
    for(const auto& t : f.traits)
    {
        ta.m_traits.push_back( LowerHIR_TraitPath(t.sp, *t.ent.path, t.ent.hrbs) );
    }

    return ta;
}
::HIR::Function LowerHIR_Function(::HIR::ItemPath p, const ::AST::AttributeList& attrs, const ::AST::Function& f, const ::HIR::TypeRef& real_self_type)
{
    static Span sp;

    TRACE_FUNCTION_F(p);

    ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef > >    args;
    for(const auto& arg : f.args())
        args.push_back( ::std::make_pair( LowerHIR_Pattern(arg.pat), LowerHIR_Type(arg.ty) ) );

    auto receiver = ::HIR::Function::Receiver::Free;

    if( args.size() > 0 && args.front().first.m_bindings.size() > 0 && args.front().first.m_bindings[0].m_name == "self" )
    {
        const auto& sp = f.args()[0].pat.span();
        auto& arg_self_ty = args.front().second;

        struct Ivcr {
            const Span& sp;
            const ::HIR::TypeRef& real_self_type;
            Ivcr(const Span& sp, const ::HIR::TypeRef& real_self_type)
                : sp(sp)
                , real_self_type(real_self_type)
            {
            }

            bool is_valid_custom_receiver(::HIR::TypeRef& ty) const {
                // - The path must include Self as a (the only?) type param.
                if( ty == ty_Self ) {
                    return true;
                }
                else if( ty == real_self_type ) {
                    ty = ty_Self.clone();
                    return true;
                }
                else if( auto* e = ty.data_mut().opt_Path() )
                {
                    if( auto* pe = e->path.m_data.opt_Generic() ) {
                        if( pe->m_params.m_types.size() == 0 ) {
                            ERROR(sp, E0000, "Receiver type should have one type param - " << ty);
                        }
                        //if( pe->m_params.m_types.size() != 1 ) {
                        //   TODO(sp, "Receiver types with more than one param - " << arg_self_ty);
                        //}

                        // TODO: Allow if the type parm is a valid receiver it type too
                        // - In general, it's valid if there's a deref chain from this type to `self` (maybe could check that in a later pass, instead of erroring here)
                        return is_valid_custom_receiver(pe->m_params.m_types[0]);
                    }
                    return false;
                }
                else if( ty.data().is_Borrow() ) {
                    return is_valid_custom_receiver(ty.data_mut().as_Borrow().inner);
                }
                else if( ty.data().is_Pointer() ) {
                    return is_valid_custom_receiver(ty.data_mut().as_Pointer().inner);
                }
                else {
                    return false;
                }
            }
        } ivcr(sp, real_self_type);

        if( arg_self_ty == ty_Self  || arg_self_ty == real_self_type ) {
            receiver = ::HIR::Function::Receiver::Value;
        }
        else if(auto* e = arg_self_ty.data_mut().opt_Borrow() ) {
            if( e->inner == ty_Self || e->inner == real_self_type )
            {
                switch(e->type)
                {
                case ::HIR::BorrowType::Owned:  receiver = ::HIR::Function::Receiver::BorrowOwned;  break;
                case ::HIR::BorrowType::Unique: receiver = ::HIR::Function::Receiver::BorrowUnique; break;
                case ::HIR::BorrowType::Shared: receiver = ::HIR::Function::Receiver::BorrowShared; break;
                }
            }
            else
            {
                if( ivcr.is_valid_custom_receiver(e->inner) )
                {
                    receiver = ::HIR::Function::Receiver::Custom;
                }
            }
        }
        else if(auto* e = arg_self_ty.data_mut().opt_Path()) {
            // Box - Compare with `owned_box` lang item
            if(auto* pe = e->path.m_data.opt_Generic()) {
                auto p = g_crate_ptr->get_lang_item_path_opt("owned_box");
                if( pe->m_path == p )
                {
                    if( pe->m_params.m_types.size() >= 1 && (pe->m_params.m_types[0] == ty_Self || pe->m_params.m_types[0] == real_self_type) )
                    {
                        pe->m_params.m_types[0] = ty_Self.clone();
                        receiver = ::HIR::Function::Receiver::Box;
                    }
                }
                // TODO: for other types, support arbitary structs/paths.
                if( receiver == ::HIR::Function::Receiver::Free )
                {
                    if(ivcr.is_valid_custom_receiver(arg_self_ty))
                    {
                        receiver = ::HIR::Function::Receiver::Custom;
                    }
                }
            }
        }
        else if( ivcr.is_valid_custom_receiver(arg_self_ty) ) {
            receiver = ::HIR::Function::Receiver::Custom;
        }
        else {
        }

        if( receiver == ::HIR::Function::Receiver::Free )
        {
            ERROR(sp, E0000, "Unknown receiver type - " << arg_self_ty);
        }
    }

    bool force_emit = false;
    HIR::Function::Markings markings;
    switch(f.m_markings.inline_type)
    {
    case ::AST::Function::Markings::Inline::Auto:
        markings.inline_type = ::HIR::Function::Markings::Inline::Auto;
        break;
    case ::AST::Function::Markings::Inline::Never:
        markings.inline_type = ::HIR::Function::Markings::Inline::Never;
        break;
    case ::AST::Function::Markings::Inline::Always:
        markings.inline_type = ::HIR::Function::Markings::Inline::Always;
        force_emit = true;
        break;
    case ::AST::Function::Markings::Inline::Normal:
        markings.inline_type = ::HIR::Function::Markings::Inline::Normal;
        force_emit = true;
        break;
    }

    // #[rustc_legacy_const_generics] - Used to convert a literal argument into a const generic
    for(auto idx : f.m_markings.rustc_legacy_const_generics)
    {
        ASSERT_BUG(attrs.get("rustc_legacy_const_generics")->span(), idx < args.size() + f.m_markings.rustc_legacy_const_generics.size(),
            "#[rustc_legacy_const_generics(" << idx << ")] out of range (0.." << args.size() + f.m_markings.rustc_legacy_const_generics.size() << ")");
        markings.rustc_legacy_const_generics.push_back( idx );
    }
    // #[track_caller] - Provides caller information
    // NOTE: This can only be (cleanly) handled in the backend [where it sees fully monomorphised paths]
    if( attrs.get("track_caller") )
    {
        markings.track_caller = true;
    }

    ::HIR::Linkage  linkage;
    switch(f.m_markings.linkage) {
    case AST::Linkage::Default:
        break;
    case AST::Linkage::Weak:
        linkage.type = HIR::Linkage::Type::Weak;
        break;
    case AST::Linkage::ExternWeak:
        BUG(sp, "Invalid linkage on function");
    }
    linkage.section = f.m_markings.link_section;

    // Convert #[link_name/no_mangle] attributes into the name
    if( g_ast_crate_ptr->m_test_harness && f.code().is_valid() )
    {
        // If we're making a test harness, and this item defines code, don't apply the linkage rules
    }
    else if( f.m_markings.link_name != "" )
    {
        linkage.name = f.m_markings.link_name;
    }
    else if( attrs.get("rustc_std_internal_symbol") )
    {
        linkage.name = p.get_name();
        linkage.type = ::HIR::Linkage::Type::Weak;
    }
    else if( attrs.get("no_mangle") )
    {
        linkage.name = p.get_name();
    }
    else
    {
        // Leave linkage.name as empty
    }

    // If there's no code, mangle the name (According to the ABI) and set linkage.
    if( linkage.name == "" && ! f.code().is_valid() )
    {
        linkage.name = p.get_name();
    }

    ::HIR::Function rv;
    rv.m_save_code = force_emit;
    rv.m_linkage = mv$(linkage);
    rv.m_receiver = receiver;
    if(receiver == HIR::Function::Receiver::Custom) {
        rv.m_receiver_type = MonomorphiserNop().monomorph_type(f.args()[0].ty.span(), args.front().second, false);
        // Ensure that the reciever references `Self`
        ASSERT_BUG(f.args()[0].ty.span(),
            visit_ty_with(rv.m_receiver_type, [](const HIR::TypeRef& v){ return v.data().is_Generic() && v.data().as_Generic().is_self(); }),
            rv.m_receiver_type
            );
    }
    rv.m_abi = RcString::new_interned(f.abi());
    rv.m_unsafe = f.is_unsafe();
    rv.m_const = f.is_const();
    rv.m_params = LowerHIR_GenericParams(f.params(), nullptr);  // TODO: If this is a method, then it can add the Self: Sized bound
    rv.m_args = mv$(args);
    rv.m_variadic = f.is_variadic();
    rv.m_return = LowerHIR_Type( f.rettype() );
    rv.m_code = LowerHIR_Expr( f.code() );
    rv.m_markings = markings;

    if( f.is_async() ) {
        //rv.m_markings.is_async = true;
        // Wrap the code in an async block
        rv.m_code = HIR::ExprPtr(box$(::HIR::ExprNode_AsyncBlock(sp, std::move(rv.m_code.into_unique()), true) ));
        // Make the return type be `impl Future<Output=Ret>`
        HIR::TraitPath  future_path;
        future_path.m_path.m_path = g_crate_ptr->get_lang_item_path(sp, "future_trait");
        future_path.m_type_bounds.insert(std::make_pair(
            RcString::new_interned("Output"),
            ::HIR::TraitPath::AtyEqual { future_path.m_path.clone(), {}, std::move(rv.m_return) }
        ));
        rv.m_return = ::HIR::TypeRef(::HIR::TypeData::make_ErasedType(::HIR::TypeData_ErasedType {
            true,
            ::make_vec1(std::move(future_path)),
            {},
            ::HIR::TypeData_ErasedType_Inner::Data_Fcn { ::HIR::Path(::HIR::SimplePath()), 0 }
        }));
    }

    return rv;
}

void _add_mod_ns_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::TypeItem ti) {
    mod.m_mod_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::TypeItem> { is_pub, mv$(ti) }) ) );
}
void _add_mod_val_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::ValueItem ti) {
    mod.m_value_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::ValueItem> { is_pub, mv$(ti) }) ) );
}
void _add_mod_mac_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::MacroItem ti) {
    mod.m_macro_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::MacroItem> { is_pub, mv$(ti) }) ) );
}

::HIR::ValueItem LowerHIR_Static(::HIR::ItemPath p, const ::AST::AttributeList& attrs, const ::AST::Static& e, const Span& sp, const RcString& name)
{
    TRACE_FUNCTION_F(p);

    if( e.s_class() == ::AST::Static::CONST )
        // Note: Empty names are allowed for `const _: ...`
        return ::HIR::ValueItem::make_Constant(::HIR::Constant(
            ::HIR::GenericParams {},
            LowerHIR_Type(e.type()),
            LowerHIR_Expr(e.value())
            ));
    else {
        // Note: Empty names are allowed for `const _: ...`
        ASSERT_BUG(sp, name != "", "Empty constant name " << p);
        ::HIR::Linkage  linkage;
        switch(e.m_markings.linkage) {
        case AST::Linkage::Default:
            break;
        case AST::Linkage::Weak:
            linkage.type = HIR::Linkage::Type::Weak;
            break;
        case AST::Linkage::ExternWeak:
            linkage.type = HIR::Linkage::Type::ExternWeak;
            break;
        }
        linkage.section = e.m_markings.link_section;

        if( e.m_markings.link_name != "" ) {
            linkage.name = e.m_markings.link_name;
        }
        // If there's no code, demangle the name (TODO: By ABI) and set linkage.
        else if( linkage.name == "" && !e.value().is_valid() ) {
            linkage.name = name.c_str();
        }

        return ::HIR::ValueItem::make_Static(::HIR::Static(
            mv$(linkage),
            (e.s_class() == ::AST::Static::MUT),
            LowerHIR_Type(e.type()),
            LowerHIR_Expr(e.value())
            ));
    }
}

::HIR::Module LowerHIR_Module(const ::AST::Module& ast_mod, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits)
{
    TRACE_FUNCTION_F("path = " << path);
    ::HIR::Module   mod { };

    mod.m_traits = mv$(traits);

    auto mod_path = path.get_simple_path();
    auto get_vis = [&](const AST::Visibility& vis) { return LowerHIR_Vis(mod_path, vis); };

    // Populate trait list
    for(const auto& trait_path : ast_mod.m_traits )
    {
        auto sp = HIR::SimplePath((trait_path.crate == "" ? g_crate_name : trait_path.crate), trait_path.nodes);
        if( ::std::find(mod.m_traits.begin(), mod.m_traits.end(), sp) == mod.m_traits.end() )
            mod.m_traits.push_back( mv$(sp) );
    }

    for( unsigned int i = 0; i < ast_mod.anon_mods().size(); i ++ )
    {
        const auto& submod_ptr = ast_mod.anon_mods()[i];
        if( submod_ptr )
        {
            auto& submod = *submod_ptr;
            auto name = RcString::new_interned(FMT("#" << i));
            auto item_path = ::HIR::ItemPath(path, name.c_str());
            auto ti = ::HIR::TypeItem::make_Module( LowerHIR_Module(submod, item_path, mod.m_traits) );
            _add_mod_ns_item( mod,  mv$(name), ::HIR::Publicity::new_priv(mod_path), mv$(ti) );
        }
    }

    for( const auto& ip : ast_mod.m_items )
    {
        const auto& item = *ip;
        const auto& sp = item.span;
        auto item_path = ::HIR::ItemPath(path, item.name.c_str());
        DEBUG(item_path << " " << item.data.tag_str());
        TU_MATCH_HDRA( (item.data), {)
        TU_ARMA(None, e) {
            }
        TU_ARMA(Macro, e) {
            // NOTE: These are in `m_macros`
            }
        TU_ARMA(MacroInv, e) {
            // Valid.
            //BUG(sp, "Stray macro invocation in " << path);
            }
        TU_ARMA(GlobalAsm, e) {
            ::HIR::GlobalAssembly   item;
            item.m_lines = std::move(e.lines);
            item.m_symbols.reserve(e.symbols.size());
            for(const AST::Path& s : e.symbols) {
                item.m_symbols.push_back( LowerHIR_Path(Span(), s, FromAST_PathClass::Value) );
            }
            item.m_options = e.options;
            g_crate_ptr->m_global_asm.push_back(std::move(item));
            }
        TU_ARMA(ExternBlock, e) {
            if( e.items().size() > 0 )
            {
                TODO(sp, "Expand ExternBlock");
            }
            for(const auto& lib : e.m_libraries)
            {
                g_crate_ptr->m_ext_libs.push_back( ::HIR::ExternLibrary { lib.lib_name } );
            }
            }
        TU_ARMA(Impl, e) {
            // NOTE: impl blocks are handled in a second pass
            }
        TU_ARMA(NegImpl, e) {
            // NOTE: impl blocks are handled in a second pass
            }
        TU_ARMA(Use, e) {
            // Ignore - The index is used to add `Import`s
            }
        TU_ARMA(Module, e) {
            _add_mod_ns_item( mod, item.name, get_vis(item.vis), LowerHIR_Module(e, mv$(item_path)) );
            }
        TU_ARMA(Crate, e) {
            // All 'extern crate' items should be normalised into a list in the crate root
            // - If public, add a namespace import here referring to the root of the imported crate
            _add_mod_ns_item( mod, item.name, get_vis(item.vis), ::HIR::TypeItem::make_Import({ ::HIR::SimplePath(e.name, {}), false, 0} ) );
            }
        TU_ARMA(Type, e) {
            if( e.type().m_data.is_Any() )
            {
                if( !e.params().m_params.empty() || !e.params().m_bounds.empty() )
                {
                    ERROR(item.span, E0000, "Generics on extern type");
                }
                _add_mod_ns_item(mod, item.name, get_vis(item.vis), ::HIR::ExternType {});
                break;
            }
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), ::HIR::TypeItem::make_TypeAlias( LowerHIR_TypeAlias(item_path, e) ) );
            }
        TU_ARMA(Struct, e) {
            /// Add value reference
            if( e.m_data.is_Unit() ) {
                _add_mod_val_item( mod,  item.name, get_vis(item.vis), ::HIR::ValueItem::make_StructConstant({item_path.get_simple_path()}) );
            }
            else if( e.m_data.is_Tuple() ) {
                _add_mod_val_item( mod,  item.name, get_vis(item.vis), ::HIR::ValueItem::make_StructConstructor({item_path.get_simple_path()}) );
            }
            else {
            }
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), LowerHIR_Struct(ip->span, item_path, e, item.attrs) );
            }
        TU_ARMA(Enum, e) {
            auto enm = LowerHIR_Enum(item_path, e, item.attrs, [&](auto name, auto str){ _add_mod_ns_item(mod, name, get_vis(item.vis), mv$(str)); });
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), mv$(enm) );
            }
        TU_ARMA(Union, e) {
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), LowerHIR_Union(item_path, e, item.attrs) );
            }
        TU_ARMA(Trait, e) {
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), LowerHIR_Trait(item_path.get_simple_path(), e) );
            }
        TU_ARMA(TraitAlias, e) {
            _add_mod_ns_item( mod,  item.name, get_vis(item.vis), LowerHIR_TraitAlias(sp, item_path, e) );
            }
        TU_ARMA(Function, e) {
            _add_mod_val_item(mod, item.name, get_vis(item.vis),  LowerHIR_Function(item_path, item.attrs, e, ::HIR::TypeRef{}));
            }
        TU_ARMA(Static, e) {
            _add_mod_val_item(mod, item.name, get_vis(item.vis),  LowerHIR_Static(item_path, item.attrs, e, sp, item.name));
            }
        }
    }
    // Some explicit handling of mac
    for(auto& mac : const_cast<AST::Module&>(ast_mod).macros() )
    {
        if( mac.data || mac.vis.is_global() )
        {
            ASSERT_BUG(mac.span, mac.data, "Null macro - " << mac.name);
            ASSERT_BUG(mac.span, mac.data->m_rules.size() > 0, "Empty macro - " << mac.name);
            _add_mod_mac_item(mod, mac.name, get_vis(mac.vis), std::move(mac.data));
        }
    }

    // Imports
    Span    mod_span;
    for( const auto& ie : ast_mod.m_namespace_items )
    {
        const auto& sp = mod_span;
        if( ie.first.c_str()[0] == ' ' )
            continue;
        // TODO: Only transfer private imports if this module contains a `macro`?
        // - Well... sub-modules that contain a `macro` would also lead to the same import
        if( ie.second.is_import ) { //&& ie.second.is_pub ) {
            auto hir_path = LowerHIR_SimplePath( sp, ie.second.path, FromAST_PathClass::Type );
            assert(hir_path.components().empty() || hir_path.components().back() != "");
            ::HIR::TypeItem ti;
            if( const auto* pb = ie.second.path.m_bindings.type.binding.opt_EnumVar() ) {
                DEBUG("Import NS " << ie.first << " = " << hir_path << " (Enum Variant)");
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), true, pb->idx });
            }
            else {
                DEBUG("Import NS " << ie.first << " = " << hir_path);
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), false, 0 });
            }
            _add_mod_ns_item(mod, ie.first, get_vis(ie.second.vis), mv$(ti));
        }
    }
    for( const auto& ie : ast_mod.m_value_items )
    {
        const auto& sp = mod_span;
        if( ie.first.c_str()[0] == ' ' )
            continue;
        // TODO: See code for `m_namespace_items` above
        if( ie.second.is_import ) {//&& ie.second.is_pub ) {
            auto hir_path = LowerHIR_SimplePath( sp, ie.second.path, FromAST_PathClass::Value );
            assert(!hir_path.components().empty());
            assert(hir_path.components().back() != "");
            ::HIR::ValueItem    vi;

            TU_MATCH_HDRA( (ie.second.path.m_bindings.value.binding), {)
            default:
                DEBUG("Import VAL " << ie.first << " = " << hir_path);
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), false, 0 });
            TU_ARMA(EnumVar, pb) {
                DEBUG("Import VAL " << ie.first << " = " << hir_path << " (Enum Variant)");
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), true, pb.idx });
                }
            }
            _add_mod_val_item(mod, ie.first, get_vis(ie.second.vis), mv$(vi));
        }
    }

    for( const auto& ie : ast_mod.m_macro_items )
    {
        const auto& sp = mod_span;
        if( ie.first.c_str()[0] == ' ' )
            continue;
        auto hir_path = LowerHIR_SimplePath( sp, ie.second.path, FromAST_PathClass::Macro );
        if( ie.second.is_import )
        {
            assert(!hir_path.components().empty());
            assert(hir_path.components().back() != "");

            DEBUG("Import MACRO " << ie.first << " = " << hir_path);
            auto mi = ::HIR::MacroItem::make_Import({ mv$(hir_path) });
            _add_mod_mac_item( mod, ie.first, get_vis(ie.second.vis), mv$(mi) );
        }
        else
        {
            DEBUG("Defined MACRO " << ie.first << " = " << hir_path);
        }
    }

    return mod;
}

void LowerHIR_Module_Impls(const ::AST::Module& ast_mod,  ::HIR::Crate& hir_crate)
{
    DEBUG(ast_mod.path());
    ::HIR::SimplePath   mod_path(g_crate_name, ast_mod.path().nodes);

    // Sub-modules
    for( const auto& item : ast_mod.m_items )
    {
        if(const auto* e = item->data.opt_Module()) {
            LowerHIR_Module_Impls(*e,  hir_crate);
        }
    }
    for( const auto& submod_ptr : ast_mod.anon_mods() )
    {
        if( submod_ptr ) {
            LowerHIR_Module_Impls(*submod_ptr,  hir_crate);
        }
    }

    //
    for( const auto& i : ast_mod.m_items )
    {
        if( !i->data.is_Impl() ) continue;
        const auto& impl = i->data.as_Impl();
        const Span  impl_span;
        auto params = LowerHIR_GenericParams(impl.def().params(), nullptr);

        TRACE_FUNCTION_F("IMPL " << impl.def());

        if( impl.def().trait().ent.is_valid() )
        {
            const auto& pb = impl.def().trait().ent.m_bindings.type.binding;
            ASSERT_BUG(Span(), pb.is_Trait(), "Binding for trait path in impl isn't a Trait - " << impl.def().trait().ent);
            ASSERT_BUG(Span(), pb.as_Trait().trait_ || pb.as_Trait().hir, "Trait pointer for trait path in impl isn't set");
            bool is_marker = (pb.as_Trait().trait_ ? pb.as_Trait().trait_->is_marker() : pb.as_Trait().hir->m_is_marker);
            auto trait_path = LowerHIR_GenericPath(impl.def().trait().sp, impl.def().trait().ent, FromAST_PathClass::Type);
            auto trait_name = mv$(trait_path.m_path);
            auto trait_args = mv$(trait_path.m_params);

            if( !is_marker )
            {
                auto type = LowerHIR_Type(impl.def().type());

                ::HIR::ItemPath    path(type, trait_name, trait_args);
                DEBUG(path);

                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> > methods;
                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> > constants;
                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> > types;

                for(const auto& item : impl.items())
                {
                    ::HIR::ItemPath    item_path(path, item.name.c_str());
                    TU_MATCH_HDRA( (*item.data), {)
                    default:
                        BUG(item.sp, "Unexpected item type in trait impl - " << item.data->tag_str());
                    TU_ARMA(None, e) {
                        }
                    TU_ARMA(MacroInv, e) {
                        }
                    TU_ARMA(Static, e) {
                        if( e.s_class() == ::AST::Static::CONST ) {
                            // TODO: Check signature against the trait?
                            constants.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> { item.is_specialisable, ::HIR::Constant (
                                ::HIR::GenericParams {},
                                LowerHIR_Type( e.type() ),
                                LowerHIR_Expr( e.value() )
                                ) }) );
                        }
                        else {
                            TODO(item.sp, "Associated statics in trait impl");
                        }
                        }
                    TU_ARMA(Type, e) {
                        DEBUG("- type " << item.name);
                        auto aty_params = LowerHIR_GenericParams(e.params(), nullptr);
                        //ASSERT_BUG(Span(), aty_params.is_empty(), "TODO: GATs");

                        assert(!g_impl_trait_source.path);
                        HIR::ItemPath   ip1(mod_path);
                        ::std::string name2 = ::std::string("#impl_") + ::std::to_string((uintptr_t)&impl) + "_" + item.name.c_str();
                        HIR::ItemPath   ip2(ip1, name2.c_str());
                        g_impl_trait_source = ImplTraitSource(&ip2, &params, &aty_params);

                        types.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { item.is_specialisable, LowerHIR_Type(e.type()) }) );

                        g_impl_trait_source = ImplTraitSource();
                        }
                    TU_ARMA(Function, e) {
                        DEBUG("- method " << item.name);
                        methods.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { item.is_specialisable, LowerHIR_Function(item_path, item.attrs, e, type) }) );
                        }
                    }
                }

                // Sorted later on
                hir_crate.m_trait_impls[mv$(trait_name)].generic.push_back(::std::make_unique<HIR::TraitImpl>(::HIR::TraitImpl {
                    mv$(params),
                    mv$(trait_args),
                    mv$(type),

                    mv$(methods),
                    mv$(constants),
                    {}, // Statics
                    mv$(types),

                    mod_path
                    }));
            }
            else if( impl.def().type().m_data.is_None() )
            {
                // Ignore - These are encoded in the 'is_marker' field of the trait
            }
            else
            {
                auto type = LowerHIR_Type(impl.def().type());
                hir_crate.m_marker_impls[mv$(trait_name)].generic.push_back(box$(::HIR::MarkerImpl {
                    mv$(params),
                    mv$(trait_args),
                    true,
                    mv$(type),

                    mod_path
                    }));
            }
        }
        else
        {
            // Inherent impls
            auto type = LowerHIR_Type(impl.def().type());
            ::HIR::ItemPath    path(type);

            auto get_vis = [&](const AST::Visibility& vis) { return LowerHIR_Vis(mod_path, vis); }; // TODO: Does this need to consume anon modules?

            ::std::map< RcString, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> > methods;
            ::std::map< RcString, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> > constants;

            for(const auto& item : impl.items())
            {
                ::HIR::ItemPath    item_path(path, item.name.c_str());
                TU_MATCH_HDRA( (*item.data), {)
                default:
                    BUG(item.sp, "Unexpected item type in inherent impl - " << item.data->tag_str());
                TU_ARMA(None, e) {
                    }
                TU_ARMA(MacroInv, e) {
                    }
                TU_ARMA(Static, e) {
                    if( e.s_class() == ::AST::Static::CONST ) {
                        constants.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> { get_vis(item.vis), item.is_specialisable, ::HIR::Constant (
                            ::HIR::GenericParams {},
                            LowerHIR_Type( e.type() ),
                            LowerHIR_Expr( e.value() )
                            ) }) );
                    }
                    else {
                        TODO(item.sp, "Associated statics in inherent impl");
                    }
                    }
                TU_ARMA(Function, e) {
                    methods.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> {
                        get_vis(item.vis), item.is_specialisable, LowerHIR_Function(item_path, item.attrs, e, type)
                        } ) );
                    }
                }
            }

            // Sorted later on
            hir_crate.m_type_impls.generic.push_back( box$(::HIR::TypeImpl {
                mv$(params),
                mv$(type),
                mv$(methods),
                mv$(constants),

                mod_path
                }) );
        }
    }
    for( const auto& i : ast_mod.m_items )
    {
        if( !i->data.is_NegImpl() ) continue;
        const auto& impl = i->data.as_NegImpl();

        auto params = LowerHIR_GenericParams(impl.params(), nullptr);
        auto type = LowerHIR_Type(impl.type());
        auto trait = LowerHIR_GenericPath(impl.trait().sp, impl.trait().ent, FromAST_PathClass::Type);
        auto trait_name = mv$(trait.m_path);
        auto trait_args = mv$(trait.m_params);

        // Sorting done later
        hir_crate.m_marker_impls[mv$(trait_name)].generic.push_back(box$(::HIR::MarkerImpl {
            mv$(params),
            mv$(trait_args),
            false,
            mv$(type),

                mod_path
            }) );
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
            if(auto* e = bound.opt_TraitBound()) {
                e->trait.m_trait_ptr = &this->crate.get_trait_by_path(null_span, e->trait.m_path.m_path);
            }
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

    if(crate.m_crate_type != ::AST::Crate::Type::Executable) {
        rv.m_crate_name = crate.m_crate_name_real;
    }
    else {
        // Use a non-empty crate name that won't conflict with any libraries
        rv.m_crate_name = "bin#";
    }
    rv.m_edition = crate.m_edition;

    g_crate_ptr = &rv;
    g_ast_crate_ptr = &crate;
    g_crate_name = rv.m_crate_name;
    g_core_crate = crate.m_ext_cratename_core;
    auto macros = std::map<RcString, HIR::MacroItem>();
    //auto& macros = rv.m_exported_macros;

    // - Extract exported macros
    {
        TRACE_FUNCTION_FR("macros", "macros");
        ::std::vector< ::AST::Module*>    mods;
        mods.push_back( &crate.m_root_module );
        do
        {
            auto& mod = *mods.back();
            mods.pop_back();

            for( /*const*/ auto& mac : mod.macros() ) {
                if( mac.data->m_exported ) {
                    HIR::MacroItem  mi;
                    if( &mod == &crate.m_root_module ) {
                        mi = mv$(mac.data);
                    }
                    else {
                        assert(mac.data);
                        assert(!mac.data->m_rules.empty());
                        auto pc = mod.path().nodes;
                        pc.push_back(mac.name);
                        mi = HIR::MacroItem::make_Import({ ::HIR::SimplePath(g_crate_name, std::move(pc)) });
                    }
                    ASSERT_BUG(Span(), macros.count(mac.name) == 0, "Duplicate export of: " << mac.name);
                    if( macros.count(mac.name) == 0 )
                    {
                        auto res = macros.insert( ::std::make_pair( mac.name, mv$(mi) ) );
                        if( res.second )
                        {
                            DEBUG("- Define " << mac.name << "!");
                            rv.m_exported_macro_names.push_back(mac.name);
                        }
                        if(res.first->second.is_MacroRules() ) {
                            ASSERT_BUG(Span(), !res.first->second.as_MacroRules()->m_rules.empty(), "Empty macro? - " << mac.name);
                        }
                    }

#if 1
                    for(auto& e : macros) {
                        if(e.second.is_MacroRules() ) {
                            ASSERT_BUG(Span(), !e.second.as_MacroRules()->m_rules.empty(), "Empty macro? - " << e.first);
                        }
                    }
#endif
                }
                else {
                    DEBUG("- Non-exported " << mac.name << "!");
                }
            }

            for(auto& i : mod.m_items) {
                if( i->data.is_Module() )
                    mods.push_back( &i->data.as_Module() );
            }
        } while( mods.size() > 0 );

        for( const auto& mac : crate.m_root_module.m_macro_imports )
        {
            if( mac.is_pub || (mac.ref.is_MacroRules() && mac.ref.as_MacroRules()->m_exported) ) {
                // Add to the re-export list
                auto path = ::HIR::SimplePath(mac.path.crate, mac.path.nodes);
                auto res = macros.insert( std::make_pair(mac.name, HIR::MacroItem::make_Import({path})) );
                if( !res.second ) {
                    DEBUG("Conflict in imported vs local macros: " << mac.name);
                }
                else {
                    DEBUG("Re-export " << mac.name << "! = " << path);
                    rv.m_exported_macro_names.push_back(mac.name);
                }
            }
        }

        for(const auto& i : crate.m_root_module.m_macro_items)
        {
            if(i.second.vis.is_global())
            {
                rv.m_exported_macro_names.push_back(i.first);
            }
        }
    }
    // - Proc Macros
    if( crate.m_crate_type == ::AST::Crate::Type::ProcMacro )
    {
        for(const auto& ent : crate.m_proc_macros)
        {
            struct H {
                static ::HIR::ProcMacro::Ty cvt_macro_ty(::AST::ProcMacroTy ast) {
                    switch(ast)
                    {
                    case ::AST::ProcMacroTy::Function:  return ::HIR::ProcMacro::Ty::Function;
                    case ::AST::ProcMacroTy::Derive:    return ::HIR::ProcMacro::Ty::Derive;
                    case ::AST::ProcMacroTy::Attribute: return ::HIR::ProcMacro::Ty::Attribute;
                    }
                    throw "Invalid AST macro type";
                }
            };
            // Register under an invalid SimplePath
            ::HIR::ProcMacro::Ty    ty = H::cvt_macro_ty(ent.ty);
            macros.insert( std::make_pair(ent.name, ::HIR::ProcMacro { ty, ent.name, ::HIR::SimplePath(RcString(""), { ent.name }), ent.attributes }) );
            rv.m_exported_macro_names.push_back(ent.name);
            DEBUG("Export proc_macro " << ent.name);
        }
    }
    else
    {
        ASSERT_BUG(Span(), crate.m_proc_macros.size() == 0, "Procedural macros defined in non proc-macro crate");
    }

    auto sp = Span();
    // - Store the lang item paths so conversion code can use them.
    for( const auto& lang_item_path : crate.m_lang_items )
    {
        assert(lang_item_path.second.crate == "");
        rv.m_lang_items.insert( ::std::make_pair(
            lang_item_path.first,
            HIR::SimplePath(g_crate_name, lang_item_path.second.nodes)
            ) );
            DEBUG("Defined language item '" << lang_item_path.first << "' at " << lang_item_path.second);
    }
    rv.m_ext_crates_ordered = crate.m_extern_crates_ord;
    for(auto& ext_crate : crate.m_extern_crates)
    {
        // Populate m_lang_items from loaded crates too
        for( const auto& lang : ext_crate.second.m_hir->m_lang_items )
        {
            const auto& name = lang.first;
            const auto& path = lang.second;
            auto irv = rv.m_lang_items.insert( ::std::make_pair(name, path) );
            DEBUG("Load language item '" << lang.first << "' at " << lang.second << " from " << ext_crate.first);
            if( irv.second == true )
            {
                // Doesn't yet exist, all good
            }
            else if( irv.first->second == path )
            {
                // Equal definitions, also good (TODO: How can this happen?)
            }
            else if( irv.first->second.components().empty() && path.components().empty() )
            {
                // Both are just markers, also good (e.g. #![needs_panic_runtime])
            }
            else
            {
                ERROR(sp, E0000, "Conflicting definitions of lang item '" << name << "'. " << path << " and " << irv.first->second);
            }
        }
        auto p1 = ext_crate.second.m_filename.rfind('/');
        auto p2 = ext_crate.second.m_filename.rfind('\\');
        auto p = (p1 == ::std::string::npos ? p2 : (p2 == ::std::string::npos ? p1 : ::std::max(p1,p2)));
        auto crate_file = (p == ::std::string::npos ? ext_crate.second.m_filename : ext_crate.second.m_filename.substr(p+1));
        rv.m_ext_crates.insert( ::std::make_pair( ext_crate.first, ::HIR::ExternCrate { mv$(ext_crate.second.m_hir), crate_file, ext_crate.second.m_filename } ) );
    }
    path_Sized = rv.get_lang_item_path(sp, "sized");
    path_PointeeSized = rv.get_lang_item_path_opt("pointee_sized");
    path_MetadataSized = rv.get_lang_item_path_opt("metadata_sized");

    rv.m_root_module = LowerHIR_Module( crate.m_root_module, ::HIR::ItemPath(rv.m_crate_name) );
    for(auto& e : macros)
    {
        if(e.second.is_MacroRules() ) {
            ASSERT_BUG(Span(), !e.second.as_MacroRules()->m_rules.empty(), "Empty macro? - " << e.first);
        }
        rv.m_root_module.m_macro_items.insert( ::std::make_pair(e.first, box$(HIR::VisEnt<HIR::MacroItem> { HIR::Publicity::new_global(), mv$(e.second) })) );
    }

    LowerHIR_Module_Impls(crate.m_root_module,  rv);

    // Set all pointers in the HIR to the correct (now fixed) locations
    //IndexVisitor(rv).visit_crate( rv );

    // HACK: If the current crate is libcore, store the paths to various non-lang ops items
    // - Some operators aren't tagged with #[lang], so this works around that
    if( crate.m_crate_name_set == "core" )
    {
        struct H {
            static ::HIR::SimplePath resolve_path(const ::HIR::Crate& crate, bool is_value, ::std::initializer_list<const char*> n)
            {
                ::std::vector<RcString> cur_path_components;

                const ::HIR::Module* mod = &crate.m_root_module;
                assert(n.begin() != n.end());
                for(auto it = n.begin(); it != n.end()-1; ++it)
                {
                    auto it2 = mod->m_mod_items.find(*it);
                    if( it2 == mod->m_mod_items.end() )
                        return ::HIR::SimplePath();
                    const auto& e = it2->second;
                    if(const auto* ip = e->ent.opt_Import())
                    {
                        // TODO: Handle module aliases?
                        (void)ip;
                        return ::HIR::SimplePath();
                    }
                    else if(const auto* ep = e->ent.opt_Module() )
                    {
                        cur_path_components.push_back(*it);
                        mod = ep;
                    }
                    else
                    {
                        // Incorrect item type
                        return ::HIR::SimplePath();
                    }
                }

                auto last = *(n.end()-1);
                if( is_value )
                {
                    throw "";
                }
                else
                {
                    auto it2 = mod->m_mod_items.find(last);
                    if( it2 == mod->m_mod_items.end() )
                        return ::HIR::SimplePath();

                    // Found: Either return the current path, or return this alias.
                    if(const auto* ip = it2->second->ent.opt_Import())
                    {
                        if(ip->is_variant)
                            return ::HIR::SimplePath();
                        return ip->path;
                    }
                    else
                    {
                        cur_path_components.push_back(last);
                        return ::HIR::SimplePath("", std::move(cur_path_components));
                    }
                }
            }
        };
        // Check for existing defintions of lang items before adding magic ones
        if( TARGETVER_MOST_1_19 )
        {
            if( rv.m_lang_items.count("boxed_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("boxed_trait"),  H::resolve_path(rv, false, {"ops", "Boxed"}) ));
            }
            if( rv.m_lang_items.count("placer_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("placer_trait"),  H::resolve_path(rv, false, {"ops", "Placer"}) ));
            }
            if( rv.m_lang_items.count("place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("place_trait"),  H::resolve_path(rv, false, {"ops", "Place"}) ));
            }
            if( rv.m_lang_items.count("box_place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("box_place_trait"),  H::resolve_path(rv, false, {"ops", "BoxPlace"}) ));
            }
            if( rv.m_lang_items.count("in_place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("in_place_trait"),  H::resolve_path(rv, false, {"ops", "InPlace"}) ));
            }
        }
    }

    // Macro fixups:
    // - Convert interpolated AST items to token sequences
    {
        struct H {
            static void fix_macro_contents(std::vector<MacroExpansionEnt>& rule_contents)
            {
                for( auto it = rule_contents.begin(); it != rule_contents.end(); )
                {
                    if(auto* tok = it->opt_Token())
                    {
                        //TODO: Can this share with `proc_macro`? Maybe a function on AST types to generate a token tree from the AST again.
                        struct NewToks {
                            std::vector<MacroExpansionEnt>  out;
                            void emit_ast(const AST::ExprNode& e)
                            {
                                if( const auto* ep = dynamic_cast<const AST::ExprNode_Integer*>(&e) ) {
                                    out.push_back( Token(ep->m_value, ep->m_datatype) );
                                }
                                else {
                                    throw std::runtime_error("Unknown node type");
                                }
                            }
                            void emit_path(const ::AST::Path& path) {
                                TU_MATCH_HDRA( (path.m_class), {)
                                default:
                                    TODO(Span(), "Convert interpolated macro fragment: " << path);
                                TU_ARMA(Relative, pc) {
                                    for(const auto& n : pc.nodes) {
                                        if( &n != &pc.nodes.front() )
                                            out.push_back(Token(TOK_DOUBLE_COLON));
                                        out.push_back(Token(TOK_IDENT, Ident(pc.hygiene, n.name())));
                                        if( !n.args().is_empty() ) {
                                            TODO(Span(), "Convert interpolated macro fragment (path node args): " << path);
                                        }
                                    }
                                    }
                                }
                            }
                            void emit_type(::TypeRef& ty) {
                                TU_MATCH_HDRA( (ty.m_data), { )
                                default:
                                    TODO(Span(), "Convert interpolated macro fragment: " << ty);
                                TU_ARMA(Path, p) {
                                    emit_path(*p);
                                    }
                                }
                            }
                            void emit_tokentree(TokenTree& tt) {
                                if( tt.is_token() ) {
                                    emit_token(tt.tok());
                                }
                                else {
                                    for(size_t i = 0; i < tt.size(); i ++) {
                                        emit_tokentree(tt[i]);
                                    }
                                }
                            }

                            void emit_token(Token& tok)
                            {
                                switch(tok.type())
                                {
                                case TOK_INTERPOLATED_PATH:
                                case TOK_INTERPOLATED_PATTERN:
                                case TOK_INTERPOLATED_STMT:
                                case TOK_INTERPOLATED_BLOCK:
                                case TOK_INTERPOLATED_ITEM:
                                case TOK_INTERPOLATED_VIS:
                                    // Emit as a token tree with no separator
                                    TODO(Span(), "Convert interpolated macro fragment: " << tok);
                                    break;
                                case TOK_INTERPOLATED_TYPE:
                                    emit_type(tok.frag_type());
                                    break;
                                case TOK_INTERPOLATED_META: {
                                    auto& i = tok.frag_meta();
                                    for(const auto& e : i.name().elems)
                                    {
                                        if( &e != &i.name().elems.front() )
                                            out.push_back(Token(TOK_DOUBLE_COLON));
                                        out.push_back(Token(TOK_IDENT, e));
                                    }
                                    emit_tokentree(i.data_mut());
                                    break; }
                                case TOK_INTERPOLATED_EXPR:
                                    try {
                                        emit_ast(*tok.take_frag_node());
                                    }
                                    catch(const std::exception& e) {
                                        TODO(Span(), "Convert interpolated macro fragment: " << tok << " - " << e.what());
                                    }
                                    break;
                                default:
                                    out.push_back(std::move(tok));
                                    return;
                                }
                            }
                        };

                        NewToks  new_toks;
                        switch(tok->type())
                        {
                        case TOK_INTERPOLATED_PATH:
                        case TOK_INTERPOLATED_TYPE:
                        case TOK_INTERPOLATED_PATTERN:
                        case TOK_INTERPOLATED_STMT:
                        case TOK_INTERPOLATED_BLOCK:
                        case TOK_INTERPOLATED_ITEM:
                        case TOK_INTERPOLATED_VIS:
                        case TOK_INTERPOLATED_META:
                        case TOK_INTERPOLATED_EXPR:
                            new_toks.emit_token(*tok);
                            break;
                        default:
                            ++ it;
                            continue;
                        }
                        if( new_toks.out.size() == 0 ) {
                            it = rule_contents.erase(it);
                        }
                        else {
                            *it = std::move(new_toks.out.front());
                            it = rule_contents.insert(it+1,
                                std::move_iterator< decltype(new_toks.out.begin()) >(new_toks.out.begin()+1),
                                std::move_iterator< decltype(new_toks.out.begin()) >(new_toks.out.end())
                            );
                            it += new_toks.out.size();
                        }
                    }
                    else {
                        ++it;
                    }
                }
            }
            static void fix_macros_in_mod(HIR::Module& mod)
            {
                for(auto& mi : mod.m_mod_items) {
                    if(auto* submod_p = mi.second->ent.opt_Module()) {
                        fix_macros_in_mod(*submod_p);
                    }
                }
                for(auto& mi : mod.m_macro_items) {
                    if(auto* mrpp = mi.second->ent.opt_MacroRules()) {
                        auto& mr = **mrpp;
                        if( mr.m_source_crate.size() == 0 ) {
                            mr.m_source_crate = g_crate_name;
                        }
                        for(auto& rule : mr.m_rules)
                        {
                            fix_macro_contents(rule.m_contents);
                        }
                    }
                    if( const auto* i = mi.second->ent.opt_Import() ) {
                        if( i->path.crate_name() == CRATE_BUILTINS ) {
                        }
                        else if( const auto* i2 = g_crate_ptr->get_macroitem_by_path(Span(), i->path).opt_Import() ) {
                            BUG(Span(), "Attempted recusive import - " << i->path << " points at " << i2->path);
                        }
                    }
                }
            }
        };
        H::fix_macros_in_mod(rv.m_root_module);
    }

    if(g_core_crate == "") {
        g_core_crate = g_crate_name;
    }

    g_crate_ptr = nullptr;
    return ::HIR::CratePtr( mv$(rv) );
}


