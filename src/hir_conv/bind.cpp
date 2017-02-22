/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/bind.cpp
 * - Set binding pointers in HIR structures
 * - Also fixes parameter counts.
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <mir/mir.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>

void ConvertHIR_Bind(::HIR::Crate& crate);

namespace {


    enum class Target {
        TypeItem,
        Struct,
        Enum,
        EnumVariant,
    };
    const void* get_type_pointer(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, Target t)
    {
        if( t == Target::EnumVariant )
        {
            return &crate.get_typeitem_by_path(sp, path, false, true).as_Enum();
        }
        else
        {
            const auto& ti = crate.get_typeitem_by_path(sp, path);
            switch(t)
            {
            case Target::TypeItem:  return &ti;
            case Target::EnumVariant:   throw "";

            case Target::Struct:
                TU_IFLET(::HIR::TypeItem, ti, Struct, e2,
                    return &e2;
                )
                else {
                    ERROR(sp, E0000, "Expected a struct at " << path << ", got a " << ti.tag_str());
                }
                break;
            case Target::Enum:
                TU_IFLET(::HIR::TypeItem, ti, Enum, e2,
                    return &e2;
                )
                else {
                    ERROR(sp, E0000, "Expected a enum at " << path << ", got a " << ti.tag_str());
                }
                break;
            }
            throw "";
        }
    }

    void fix_type_params(const Span& sp, const ::HIR::GenericParams& params_def, ::HIR::PathParams& params)
    {
        #if 1
        if( params.m_types.size() == 0 ) {
            params.m_types.resize( params_def.m_types.size() );
        }
        if( params.m_types.size() != params_def.m_types.size() ) {
            ERROR(sp, E0000, "Incorrect parameter count, expected " << params_def.m_types.size() << ", got " << params.m_types.size());
        }
        #endif
    }

    const ::HIR::Struct& get_struct_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& str = *reinterpret_cast< const ::HIR::Struct*>( get_type_pointer(sp, crate, path.m_path, Target::Struct) );
        fix_type_params(sp, str.m_params,  path.m_params);
        return str;
    }
    ::std::pair< const ::HIR::Enum*, unsigned int> get_enum_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& enm = *reinterpret_cast< const ::HIR::Enum*>( get_type_pointer(sp, crate, path.m_path, Target::EnumVariant) );
        const auto& des_name = path.m_path.m_components.back();
        unsigned int idx = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x) { return x.first == des_name; }) - enm.m_variants.begin();
        if( idx == enm.m_variants.size() ) {
            ERROR(sp, E0000, "Couldn't find enum variant " << path);
        }

        fix_type_params(sp, enm.m_params,  path.m_params);
        return ::std::make_pair( &enm, idx );
    }


    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {}

        void visit_trait_path(::HIR::TraitPath& p) override
        {
            static Span sp;
            p.m_trait_ptr = &m_crate.get_trait_by_path(sp, p.m_path.m_path);

            ::HIR::Visitor::visit_trait_path(p);
        }

        void visit_literal(const Span& sp, ::HIR::Literal& lit)
        {
            TU_MATCH(::HIR::Literal, (lit), (e),
            (Invalid,
                ),
            (List,
                for(auto& val : e) {
                    visit_literal(sp, val);
                }
                ),
            (Variant,
                for(auto& val : e.vals) {
                    visit_literal(sp, val);
                }
                ),
            (Integer,
                ),
            (Float,
                ),
            (BorrowOf,
                visit_path(e, ::HIR::Visitor::PathContext::VALUE);
                ),
            (String,
                )
            )
        }

        void visit_pattern_Value(const Span& sp, ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            bool is_single_value = pat.m_data.is_Value();

            TU_IFLET( ::HIR::Pattern::Value, val, Named, ve,
                TU_IFLET( ::HIR::Path::Data, ve.path.m_data, Generic, pe,
                    const auto& path = pe.m_path;
                    const auto& pc = path.m_components.back();
                    const ::HIR::Module*  mod = nullptr;
                    if( path.m_components.size() == 1 )
                    {
                        mod = &m_crate.get_mod_by_path(sp, path, true);
                    }
                    else
                    {
                        const auto& ti = m_crate.get_typeitem_by_path(sp, path, false, true);
                        if( const auto& enm = ti.opt_Enum() )
                        {
                            if( !is_single_value ) {
                                ERROR(sp, E0000, "Enum variant in range pattern - " << pat);
                            }

                            // Enum variant
                            auto it = ::std::find_if( enm->m_variants.begin(), enm->m_variants.end(), [&](const auto&v){ return v.first == pc; });
                            if( it == enm->m_variants.end() ) {
                                BUG(sp, "'" << pc << "' isn't a variant in path " << path);
                            }
                            unsigned int index = it - enm->m_variants.begin();
                            auto path = mv$(pe);
                            fix_type_params(sp, enm->m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_EnumValue({
                                mv$(path),
                                enm,
                                index
                                });
                        }
                        else if( (mod = ti.opt_Module()) )
                        {
                            mod = &ti.as_Module();
                        }
                        else
                        {
                            BUG(sp, "Node " << path.m_components.size()-2 << " of path " << ve.path << " wasn't a module");
                        }
                    }

                    if( mod )
                    {
                        auto it = mod->m_value_items.find( path.m_components.back() );
                        if( it == mod->m_value_items.end() ) {
                            BUG(sp, "Couldn't find final component of " << path);
                        }
                        // Unit-like struct match or a constant
                        TU_MATCH_DEF( ::HIR::ValueItem, (it->second->ent), (e2),
                        (
                            ERROR(sp, E0000, "Value pattern " << pat << " pointing to unexpected item type - " << it->second->ent.tag_str())
                            ),
                        (Constant,
                            // Store reference to this item for later use
                            ve.binding = &e2;
                            ),
                        (StructConstant,
                            const auto& str = mod->m_mod_items.find(pc)->second->ent.as_Struct();
                            // Convert into a dedicated pattern type
                            if( !is_single_value ) {
                                ERROR(sp, E0000, "Struct in range pattern - " << pat);
                            }
                            auto path = mv$(pe);
                            fix_type_params(sp, str.m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_StructValue({
                                mv$(path),
                                &str
                                });
                            )
                        )
                    }
                )
                else {
                    // NOTE: Defer until Resolve UFCS (saves duplicating logic)
                }
            )
        }


        void visit_pattern(::HIR::Pattern& pat) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_pattern(pat);

            TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (Value,
                this->visit_pattern_Value(sp, pat, e.val);
                ),
            (Range,
                this->visit_pattern_Value(sp, pat, e.start);
                this->visit_pattern_Value(sp, pat, e.end);
                ),
            (StructTuple,
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Tuple, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct tuple pattern on non-tuple struct " << e.path);
                }
                ),
            (Struct,
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Named, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct pattern on field-less struct " << e.path);
                }
                ),
            (EnumTuple,
                auto p = get_enum_ptr(sp, m_crate, e.path);
                const auto& var = p.first->m_variants[p.second].second;
                TU_IFLET(::HIR::Enum::Variant, var, Tuple, _,
                    e.binding_ptr = p.first;
                    e.binding_idx = p.second;
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                ),
            (EnumStruct,
                auto p = get_enum_ptr(sp, m_crate, e.path);
                const auto& var = p.first->m_variants[p.second].second;
                TU_IFLET(::HIR::Enum::Variant, var, Struct, _,
                    // All good
                    e.binding_ptr = p.first;
                    e.binding_idx = p.second;
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                )
            )
        }
        static void fix_param_count(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
            if( params.m_types.size() == param_defs.m_types.size() ) {
                // Nothing to do, all good
                return ;
            }

            if( params.m_types.size() == 0 ) {
                for(const auto& typ : param_defs.m_types) {
                    (void)typ;
                    params.m_types.push_back( ::HIR::TypeRef() );
                }
            }
            else if( params.m_types.size() > param_defs.m_types.size() ) {
                ERROR(sp, E0000, "Too many type parameters passed to " << path);
            }
            else {
                while( params.m_types.size() < param_defs.m_types.size() ) {
                    const auto& typ = param_defs.m_types[params.m_types.size()];
                    if( typ.m_default.m_data.is_Infer() ) {
                        ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                    }
                    else {
                        // TODO: What if this contains a generic param? (is that valid? Self maybe, what about others?)
                        params.m_types.push_back( typ.m_default.clone() );
                    }
                }
            }
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            //TRACE_FUNCTION_F(ty);
            static Span _sp = Span();
            const Span& sp = _sp;

            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
                TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
                (Generic,
                    const auto& item = *reinterpret_cast< const ::HIR::TypeItem*>( get_type_pointer(sp, m_crate, pe.m_path, Target::TypeItem) );
                    TU_MATCH_DEF( ::HIR::TypeItem, (item), (e3),
                    (
                        ERROR(sp, E0000, "Unexpected item type returned for " << pe.m_path << " - " << item.tag_str());
                        ),
                    (TypeAlias,
                        BUG(sp, "TypeAlias encountered after `Resolve Type Aliases` - " << ty);
                        ),
                    (Struct,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Struct(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Union,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Union(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Enum,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Enum(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Trait,
                        ty.m_data = ::HIR::TypeRef::Data::make_TraitObject({ ::HIR::TraitPath { mv$(pe), {}, {} }, {}, {} });
                        )
                    )
                    ),
                (UfcsUnknown,
                    //TODO(sp, "Should UfcsKnown be encountered here?");
                    ),
                (UfcsInherent,
                    ),
                (UfcsKnown,
                    if( pe.type->m_data.is_Path() && pe.type->m_data.as_Path().binding.is_Opaque() ) {
                        // - Opaque type, opaque result
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    }
                    else if( pe.type->m_data.is_Generic() ) {
                        // - Generic type, opaque resut. (TODO: Sometimes these are known - via generic bounds)
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    }
                    else {
                        //bool found = find_impl(sp, m_crate, pe.trait.m_path, pe.trait.m_params, *pe.type, [&](const auto& impl_params, const auto& impl) {
                        //    DEBUG("TODO");
                        //    return false;
                        //    });
                        //if( found ) {
                        //}
                        //TODO(sp, "Resolve known UfcsKnown - " << ty);
                    }
                    )
                )
            )

            ::HIR::Visitor::visit_type(ty);
        }

        void visit_static(::HIR::ItemPath p, ::HIR::Static& i) override
        {
            ::HIR::Visitor::visit_static(p, i);
            visit_literal(Span(), i.m_value_res);
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& i) override
        {
            ::HIR::Visitor::visit_constant(p, i);
            visit_literal(Span(), i.m_value_res);
        }

        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor& upper_visitor;

                ExprVisitor(Visitor& uv):
                    upper_visitor(uv)
                {}

                void visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& p)
                {
                    upper_visitor.visit_generic_path(p, pc);
                }

                void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
                {
                    upper_visitor.visit_type(node_ptr->m_res_type);
                    ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                }
                void visit(::HIR::ExprNode_Let& node) override
                {
                    upper_visitor.visit_type(node.m_type);
                    upper_visitor.visit_pattern(node.m_pattern);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_Match& node) override
                {
                    for(auto& arm : node.m_arms)
                    {
                        for(auto& pat : arm.m_patterns)
                            upper_visitor.visit_pattern(pat);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_Cast& node) override
                {
                    upper_visitor.visit_type(node.m_res_type);
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_PathValue& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                }
                void visit(::HIR::ExprNode_CallPath& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_CallMethod& node) override
                {
                    upper_visitor.visit_path_params(node.m_params);
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_StructLiteral& node) override
                {
                    upper_visitor.visit_generic_path(node.m_path, ::HIR::Visitor::PathContext::TYPE);
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_Closure& node) override
                {
                    upper_visitor.visit_type(node.m_return);
                    for(auto& arg : node.m_args) {
                        upper_visitor.visit_pattern(arg.first);
                        upper_visitor.visit_type(arg.second);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }
            };

            for(auto& ty : expr.m_erased_types)
                visit_type(ty);

            if( expr.get() != nullptr )
            {
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
            else if( expr.m_mir )
            {
                struct H {
                    static void visit_lvalue(Visitor& upper_visitor, ::MIR::LValue& lv)
                    {
                        TU_MATCHA( (lv), (e),
                        (Variable,
                            ),
                        (Temporary,
                            ),
                        (Argument,
                            ),
                        (Return,
                            ),
                        (Static,
                            upper_visitor.visit_path(e, ::HIR::Visitor::PathContext::VALUE);
                            ),
                        (Field,
                            H::visit_lvalue(upper_visitor, *e.val);
                            ),
                        (Deref,
                            H::visit_lvalue(upper_visitor, *e.val);
                            ),
                        (Index,
                            H::visit_lvalue(upper_visitor, *e.val);
                            H::visit_lvalue(upper_visitor, *e.idx);
                            ),
                        (Downcast,
                            H::visit_lvalue(upper_visitor, *e.val);
                            )
                        )
                    }
                    static void visit_param(Visitor& upper_visitor, ::MIR::Param& p)
                    {
                        TU_MATCHA( (p), (e),
                        (LValue, H::visit_lvalue(upper_visitor, e);),
                        (Constant,
                            TU_MATCHA( (e), (ce),
                            (Int, ),
                            (Uint,),
                            (Float, ),
                            (Bool, ),
                            (Bytes, ),
                            (StaticString, ),  // String
                            (Const,
                                // TODO: Should this trigger anything?
                                ),
                            (ItemAddr,
                                upper_visitor.visit_path(ce, ::HIR::Visitor::PathContext::VALUE);
                                )
                            )
                            )
                        )
                    }
                };
                for(auto& ty : expr.m_mir->named_variables)
                    this->visit_type(ty);
                for(auto& ty : expr.m_mir->temporaries)
                    this->visit_type(ty);
                for(auto& block : expr.m_mir->blocks)
                {
                    for(auto& stmt : block.statements)
                    {
                        TU_IFLET(::MIR::Statement, stmt, Assign, se,
                            H::visit_lvalue(*this, se.dst);
                            TU_MATCHA( (se.src), (e),
                            (Use,
                                H::visit_lvalue(*this, e);
                                ),
                            (Constant,
                                TU_MATCHA( (e), (ce),
                                (Int, ),
                                (Uint,),
                                (Float, ),
                                (Bool, ),
                                (Bytes, ),
                                (StaticString, ),  // String
                                (Const,
                                    // TODO: Should this trigger anything?
                                    ),
                                (ItemAddr,
                                    this->visit_path(ce, ::HIR::Visitor::PathContext::VALUE);
                                    )
                                )
                                ),
                            (SizedArray,
                                H::visit_param(*this, e.val);
                                ),
                            (Borrow,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (Cast,
                                H::visit_lvalue(*this, e.val);
                                this->visit_type(e.type);
                                ),
                            (BinOp,
                                H::visit_param(*this, e.val_l);
                                H::visit_param(*this, e.val_r);
                                ),
                            (UniOp,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (DstMeta,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (DstPtr,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (MakeDst,
                                H::visit_lvalue(*this, e.ptr_val);
                                H::visit_param(*this, e.meta_val);
                                ),
                            (Tuple,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                ),
                            (Array,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                ),
                            (Variant,
                                H::visit_param(*this, e.val);
                                ),
                            (Struct,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                )
                            )
                        )
                        else TU_IFLET(::MIR::Statement, stmt, Drop, se,
                            H::visit_lvalue(*this, se.slot);
                        )
                        else {
                        }
                    }
                    TU_MATCHA( (block.terminator), (te),
                    (Incomplete, ),
                    (Return, ),
                    (Diverge, ),
                    (Goto, ),
                    (Panic, ),
                    (If,
                        H::visit_lvalue(*this, te.cond);
                        ),
                    (Switch,
                        H::visit_lvalue(*this, te.val);
                        ),
                    (Call,
                        H::visit_lvalue(*this, te.ret_val);
                        TU_MATCHA( (te.fcn), (e2),
                        (Value,
                            H::visit_lvalue(*this, e2);
                            ),
                        (Path,
                            visit_path(e2, ::HIR::Visitor::PathContext::VALUE);
                            ),
                        (Intrinsic,
                            visit_path_params(e2.params);
                            )
                        )
                        for(auto& arg : te.args)
                            H::visit_param(*this, arg);
                        )
                    )
                }
            }
            else
            {
            }
        }
    };
}

void ConvertHIR_Bind(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );

    // Also visit extern crates to update their pointers
    for(auto& ec : crate.m_ext_crates)
    {
        exp.visit_crate( *ec.second.m_data );
    }
}
