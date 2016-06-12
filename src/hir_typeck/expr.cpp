/*
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

#include "expr.hpp"

namespace typeck {
    
    bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
    
    bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
    {
        for(const auto& ty : tpl.m_types)
            if( monomorphise_type_needed(ty) )
                return true;
        return false;
    }
    bool monomorphise_path_needed(const ::HIR::Path& tpl)
    {
        TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e),
        (Generic,
            return monomorphise_pathparams_needed(e.m_params);
            ),
        (UfcsInherent,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
            ),
        (UfcsKnown,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.trait.m_params) || monomorphise_pathparams_needed(e.params);
            ),
        (UfcsUnknown,
            return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
            )
        )
        throw "";
    }
    bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl)
    {
        if( monomorphise_pathparams_needed(tpl.m_path.m_params) )    return true;
        for(const auto& assoc : tpl.m_type_bounds)
            if( monomorphise_type_needed(assoc.second) )
                return true;
        return false;
    }
    bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
    {
        TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
        (Infer,
            assert(!"ERROR: _ type found in monomorphisation target");
            ),
        (Diverge,
            return false;
            ),
        (Primitive,
            return false;
            ),
        (Path,
            return monomorphise_path_needed(e.path);
            ),
        (Generic,
            return true;
            ),
        (TraitObject,
            if( monomorphise_traitpath_needed(e.m_trait) )
                return true;
            for(const auto& trait : e.m_markers)
                if( monomorphise_pathparams_needed(trait.m_params) )
                    return true;
            return false;
            ),
        (Array,
            TODO(Span(), "Array - " << tpl);
            ),
        (Slice,
            return monomorphise_type_needed(*e.inner);
            ),
        (Tuple,
            for(const auto& ty : e) {
                if( monomorphise_type_needed(ty) )
                    return true;
            }
            return false;
            ),
        (Borrow,
            return monomorphise_type_needed(*e.inner);
            ),
        (Pointer,
            return monomorphise_type_needed(*e.inner);
            ),
        (Function,
            TODO(Span(), "Function - " << tpl);
            ),
        (Closure,
            TODO(Span(), "Closure - " << tpl);
            )
        )
        throw "";
    }
    
    ::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer)
    {
        ::HIR::PathParams   rv;
        for( const auto& ty : tpl.m_types) 
            rv.m_types.push_back( monomorphise_type_with(sp, ty, callback) );
        return rv;
    }
    ::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer)
    {
        return ::HIR::GenericPath( tpl.m_path, monomorphise_path_params_with(sp, tpl.m_params, callback, allow_infer) );
    }
    ::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer)
    {
        ::HIR::TraitPath    rv {
            monomorphise_genericpath_with(sp, tpl.m_path, callback, allow_infer),
            tpl.m_hrls,
            {},
            tpl.m_trait_ptr
            };
        
        for(const auto& assoc : tpl.m_type_bounds)
            rv.m_type_bounds.insert(::std::make_pair( assoc.first, monomorphise_type_with(sp, assoc.second, callback, allow_infer) ));
        
        return rv;
    }
    ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
    {
        ::HIR::TypeRef  rv;
        TRACE_FUNCTION_FR("tpl = " << tpl, rv);
        TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
        (Infer,
            if( allow_infer ) {
                rv = ::HIR::TypeRef(e);
            }
            else {
               BUG(sp, "_ type found in monomorphisation target");
            }
            ),
        (Diverge,
            rv = ::HIR::TypeRef(e);
            ),
        (Primitive,
            rv = ::HIR::TypeRef(e);
            ),
        (Path,
            TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
            (Generic,
                rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_Path {
                        monomorphise_genericpath_with(sp, e2, callback, allow_infer),
                        e.binding.clone()
                        } );
                ),
            (UfcsKnown,
                rv = ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsKnown({
                    box$( monomorphise_type_with(sp, *e2.type, callback, allow_infer) ),
                    monomorphise_genericpath_with(sp, e2.trait, callback, allow_infer),
                    e2.item,
                    monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
                    }) );
                ),
            (UfcsUnknown,
                TODO(sp, "UfcsUnknown");
                ),
            (UfcsInherent,
                TODO(sp, "UfcsInherent");
                )
            )
            ),
        (Generic,
            rv = callback(tpl).clone();
            ),
        (TraitObject,
            ::HIR::TypeRef::Data::Data_TraitObject  to;
            to.m_trait = monomorphise_traitpath_with(sp, e.m_trait, callback, allow_infer);
            for(const auto& trait : e.m_markers)
            {
                to.m_markers.push_back( monomorphise_genericpath_with(sp, trait, callback, allow_infer) ); 
            }
            to.m_lifetime = e.m_lifetime;
            rv = ::HIR::TypeRef( mv$(to) );
            ),
        (Array,
            if( e.size_val == ~0u ) {
                BUG(sp, "Attempting to clone array with unknown size - " << tpl);
            }
            rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
                box$( monomorphise_type_with(sp, *e.inner, callback) ),
                ::HIR::ExprPtr(),
                e.size_val
                }) );
            ),
        (Slice,
            rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({ box$(monomorphise_type_with(sp, *e.inner, callback)) }) );
            ),
        (Tuple,
            ::std::vector< ::HIR::TypeRef>  types;
            for(const auto& ty : e) {
                types.push_back( monomorphise_type_with(sp, ty, callback) );
            }
            rv = ::HIR::TypeRef( mv$(types) );
            ),
        (Borrow,
            rv = ::HIR::TypeRef::new_borrow(e.type, monomorphise_type_with(sp, *e.inner, callback));
            ),
        (Pointer,
            rv = ::HIR::TypeRef::new_pointer(e.type, monomorphise_type_with(sp, *e.inner, callback));
            ),
        (Function,
            ::HIR::FunctionType ft;
            ft.is_unsafe = e.is_unsafe;
            ft.m_abi = e.m_abi;
            ft.m_rettype = box$( e.m_rettype->clone() );
            for( const auto& arg : e.m_arg_types )
                ft.m_arg_types.push_back( arg.clone() );
            rv = ::HIR::TypeRef( mv$(ft) );
            ),
        (Closure,
            ::HIR::TypeRef::Data::Data_Closure  oe;
            oe.node = e.node;
            oe.m_rettype = box$( monomorphise_type_with(sp, *e.m_rettype, callback) );
            for(const auto& a : e.m_arg_types)
                oe.m_arg_types.push_back( monomorphise_type_with(sp, a, callback) );
            rv = ::HIR::TypeRef(::HIR::TypeRef::Data::make_Closure( mv$(oe) ));
            )
        )
        return rv;
    }
    
    ::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl)
    {
        DEBUG("tpl = " << tpl);
        return monomorphise_type_with(sp, tpl, [&](const auto& gt)->const auto& {
            const auto& e = gt.m_data.as_Generic();
            if( e.name == "Self" )
                TODO(sp, "Handle 'Self' when monomorphising");
            //if( e.binding >= params_def.m_types.size() ) {
            //}
            if( e.binding >= params.m_types.size() ) {
                BUG(sp, "Generic param out of input range - " << e.binding << " '"<<e.name<<"' >= " << params.m_types.size());
            }
            return params.m_types[e.binding];
            }, false);
    }

    void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct)
    {
        switch(ic)
        {
        case ::HIR::InferClass::None:
            break;
        case ::HIR::InferClass::Float:
            switch(ct)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                break;
            default:
                ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
            }
            break;
        case ::HIR::InferClass::Integer:
            switch(ct)
            {
            case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
            case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
            case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
            case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                break;
            default:
                ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
            }
            break;
        }
    }
    
    template<typename T>
    void fix_param_count_(const Span& sp, TypecheckContext& context, const T& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params)
    {
        if( params.m_types.size() == param_defs.m_types.size() ) {
            // Nothing to do, all good
            return ;
        }
        
        if( params.m_types.size() == 0 ) {
            for(const auto& typ : param_defs.m_types) {
                (void)typ;
                params.m_types.push_back( context.new_ivar_tr() );
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
    void fix_param_count(const Span& sp, TypecheckContext& context, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
        fix_param_count_(sp, context, path, param_defs, params);
    }
    void fix_param_count(const Span& sp, TypecheckContext& context, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
        fix_param_count_(sp, context, path, param_defs, params);
    }
    
    // Enumerate inferrence variables (most of them) in the expression tree
    //
    // - Any type equalities here are mostly optimisations (as this gets run only once)
    //  - If ivars can be shared down the tree - good.
    class ExprVisitor_Enum:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
        const ::HIR::TypeRef&   ret_type;
    public:
        ExprVisitor_Enum(TypecheckContext& context, const ::HIR::TypeRef& ret_type):
            context(context),
            ret_type(ret_type)
        {
        }
        
        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
            TRACE_FUNCTION_FR(typeid(*node_ptr).name(), node_ptr->m_res_type << " = " << this->context.get_type(node_ptr->m_res_type));
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
        }
        void visit_node(::HIR::ExprNode& node) override {
            this->context.add_ivars(node.m_res_type);
            DEBUG(typeid(node).name() << " : " << node.m_res_type);
        }
        void visit(::HIR::ExprNode_Block& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_nodes.size() > 0 ) {
                auto& ln = *node.m_nodes.back();
                this->context.apply_equality(ln.span(), node.m_res_type, ln.m_res_type, &node.m_nodes.back());
            }
            else {
                node.m_res_type = ::HIR::TypeRef::new_unit();
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            this->context.apply_equality(node.span(), this->ret_type, node.m_value->m_res_type,  &node.m_value);
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            TRACE_FUNCTION_F("let " << node.m_pattern << ": " << node.m_type);
            
            this->context.add_ivars(node.m_type);
            
            this->context.add_binding(node.span(), node.m_pattern, node.m_type);
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F("match ...");
            
            this->context.add_ivars(node.m_value->m_res_type);
            
            for(auto& arm : node.m_arms)
            {
                DEBUG("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns)
                {
                    this->context.add_binding(node.span(), pat, node.m_value->m_res_type);
                }
            }
            DEBUG("- match val : " << this->context.get_type(node.m_value->m_res_type));

            ::HIR::ExprVisitorDef::visit(node);
            DEBUG("- match val : " << this->context.get_type(node.m_value->m_res_type));
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            node.m_cond->m_res_type = ::HIR::TypeRef( ::HIR::CoreType::Bool );
            if( node.m_false ) {
                node.m_true->m_res_type = node.m_res_type.clone();
                node.m_false->m_res_type = node.m_res_type.clone();
            }
            else {
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
                node.m_true->m_res_type = node.m_res_type.clone();
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_UniOp& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                break;
            }
        }

        void visit(::HIR::ExprNode_ArraySized& node) override {
            if(node.m_size_val == ~0u) {
                BUG(node.span(), "sized array literal didn't have its size evaluated");
            }
            
            ::HIR::ExprVisitorDef::visit(node);
            
            this->context.apply_equality( node.span(), node.m_res_type, ::HIR::TypeRef::new_array(node.m_val->m_res_type.clone(), node.m_size_val) );
            this->context.apply_equality( node.span(), node.m_size->m_res_type, ::HIR::TypeRef(::HIR::CoreType::Usize) );
        }
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);

            ::std::vector< ::HIR::TypeRef>  types;
            for( const auto& sn : node.m_vals )
                types.push_back( sn->m_res_type.clone() );
            auto tup_type = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple(mv$(types)) );
            
            this->context.apply_equality(node.span(), node.m_res_type, tup_type);
        }
        void visit(::HIR::ExprNode_Closure& node) override
        {
            ::HIR::TypeRef::Data::Data_Closure  ty_data;
            ty_data.node = &node;
            
            for(auto& a : node.m_args) {
                this->context.add_ivars(a.second);
                this->context.add_binding(node.span(), a.first, a.second);
                ty_data.m_arg_types.push_back( a.second.clone() );
            }
            this->context.add_ivars(node.m_return);
            
            ty_data.m_rettype = box$( node.m_return.clone() );
            
            if( node.m_code->m_res_type == ::HIR::TypeRef() ) {
                node.m_code->m_res_type = node.m_return.clone();
            }
            else {
                this->context.add_ivars( node.m_code->m_res_type );
                this->context.apply_equality( node.span(), node.m_code->m_res_type, node.m_return );
            }

            this->context.apply_equality( node.span(), node.m_res_type, ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Closure(mv$(ty_data)) ) );
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Variable: Bind to same ivar
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TRACE_FUNCTION_F("var #"<<node.m_slot<<" '"<<node.m_name<<"'");
            this->context.apply_equality(node.span(), node.m_res_type, this->context.get_var_type(node.span(), node.m_slot));
        }
        
        void visit_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
            for(auto& ty : gp.m_params.m_types)
                this->context.add_ivars(ty);
        }
        void visit_path(const Span& sp, ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
            (Generic,
                this->visit_generic_path(sp, e);
                ),
            (UfcsKnown,
                this->context.add_ivars(*e.type);
                this->visit_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(*e.type);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                )
            )
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            this->visit_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            this->visit_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            this->visit_generic_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            this->visit_generic_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            this->visit_generic_path(node.span(), node.m_path);
            if( node.m_is_struct )
            {
                const auto& str = this->context.m_crate.get_struct_by_path(node.span(), node.m_path.m_path);
                fix_param_count(node.span(), this->context, node.m_path, str.m_params, node.m_path.m_params);
                auto ty = ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&str) );
                this->context.apply_equality(node.span(), node.m_res_type, ty);
            }
            else
            {
                auto s_path = node.m_path.m_path;
                s_path.m_components.pop_back();
                
                const auto& enm = this->context.m_crate.get_enum_by_path(node.span(), s_path);
                fix_param_count(node.span(), this->context, node.m_path, enm.m_params, node.m_path.m_params);
                
                auto ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(s_path), node.m_path.m_params.clone()), ::HIR::TypeRef::TypePathBinding::make_Enum(&enm) );
                this->context.apply_equality(node.span(), node.m_res_type, ty);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
    };
    
    // Continually run over the expression tree until nothing changes
    class ExprVisitor_Run:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
        ::HIR::ExprNodeP    *m_node_ptr_ptr;
    public:
        ExprVisitor_Run(TypecheckContext& context):
            context(context),
            m_node_ptr_ptr(nullptr)
        {
        }
        
        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
            m_node_ptr_ptr = &node_ptr;
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
            m_node_ptr_ptr = nullptr;
        }
        
        // - Block: Ignore all return values except the last one (which is yeilded)
        void visit(::HIR::ExprNode_Block& node) override
        {
            TRACE_FUNCTION_F("{ }");
            if( node.m_nodes.size() ) {
                auto& lastnode = node.m_nodes.back();
                this->context.apply_equality(node.span(), node.m_res_type, lastnode->m_res_type,  &lastnode);
            }
            else {
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Let: Equates inner to outer
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F("let " << node.m_pattern << " : " << node.m_type);
            if( node.m_value ) {
                this->context.apply_equality(node.span(), node.m_type, node.m_value->m_res_type,  &node.m_value);
            }

            ::HIR::ExprVisitorDef::visit(node);
        }
        
        // - If: Both branches have to agree
        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_F("if ...");
            this->context.apply_equality(node.span(), node.m_res_type, node.m_true->m_res_type,  &node.m_true);
            if( node.m_false ) {
                this->context.apply_equality(node.span(), node.m_res_type, node.m_false->m_res_type, &node.m_false);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Match: all branches match
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F("match (...: " << this->context.get_type(node.m_value->m_res_type) << ")");
            
            for(auto& arm : node.m_arms)
            {
                DEBUG("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns) {
                    this->context.apply_pattern(/*node.span(),*/ pat, node.m_value->m_res_type);
                }
                // TODO: Span on the arm
                this->context.apply_equality(node.span(), node.m_res_type, arm.m_code->m_res_type, &arm.m_code);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Assign: both sides equal
        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("... = ...");
            ::HIR::ExprVisitorDef::visit(node);
            // Plain assignment can't be overloaded, requires equal types
            if( node.m_op == ::HIR::ExprNode_Assign::Op::None ) {
                this->context.apply_equality(node.span(),
                    node.m_slot->m_res_type, node.m_value->m_res_type,
                    &node.m_value
                    );
            }
            else {
                const auto& ty_left  = this->context.get_type(node.m_slot->m_res_type );
                const auto& ty_right = this->context.get_type(node.m_value->m_res_type);
                
                bool isprim_l = ty_left .m_data.is_Primitive() || (ty_left .m_data.is_Infer() && ty_left .m_data.as_Infer().ty_class != ::HIR::InferClass::None);
                bool isprim_r = ty_right.m_data.is_Primitive() || (ty_right.m_data.is_Infer() && ty_right.m_data.as_Infer().ty_class != ::HIR::InferClass::None);
                // SHORTCUT - If both sides are primitives (either confirmed or literals)
                if( isprim_l && isprim_r ) {
                    // - The only option is for them both to be the same type (because primitives can't have multiple overloads)
                    // TODO: Check that this operation is valid to peform. (e.g. not doing f32_val <<= f32_val)
                    // TODO: Aren't the bitwise shift operators valid with any integer type count?
                    this->context.apply_equality(node.span(), node.m_slot->m_res_type, node.m_value->m_res_type);
                }
                else {
                    const char *lang_item = nullptr;
                    switch( node.m_op )
                    {
                    case ::HIR::ExprNode_Assign::Op::None:  throw "";
                    case ::HIR::ExprNode_Assign::Op::Add: lang_item = "add_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Sub: lang_item = "sub_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Mul: lang_item = "mul_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Div: lang_item = "div_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Mod: lang_item = "rem_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::And: lang_item = "bitand_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Or : lang_item = "bitor_assign" ; break;
                    case ::HIR::ExprNode_Assign::Op::Xor: lang_item = "bitxor_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Shr: lang_item = "shl_assign"; break;
                    case ::HIR::ExprNode_Assign::Op::Shl: lang_item = "shr_assign"; break;
                    }
                    assert(lang_item);
                    const auto& trait_path = this->context.m_crate.get_lang_item_path(node.span(), lang_item);
                    ::HIR::PathParams   trait_path_pp;
                    trait_path_pp.m_types.push_back( ty_right.clone() );
                    
                    
                    /*bool rv =*/ this->context.find_trait_impls(node.span(), trait_path,trait_path_pp, ty_left, [&](const auto& args, const auto& a_types) {
                        assert( args.m_types.size() == 1 );
                        const auto& impl_right = args.m_types[0];
                        
                        TODO(node.span(), "Check " << impl_right << " vs " << ty_right);
                        
                        //auto cmp = impl_index.compare_with_placeholders(node.span(), index_ty, this->context.callback_resolve_infer());
                        //if( cmp == ::HIR::Compare::Unequal)
                        //    return false;
                        //if( cmp == ::HIR::Compare::Equal ) {
                        //    return true;
                        //}
                        //DEBUG("TODO: Handle fuzzy match index operator " << impl_index);
                        return false;
                        });
                    
                    this->context.dump();
                    TODO(node.span(), "Search for implementation of " << trait_path << "<" << ty_right << "> for " << ty_left);
                }
            }
        }
        // - BinOp: Look for overload or primitive
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);
            const auto& ty_left  = this->context.get_type(node.m_left->m_res_type );
            const auto& ty_right = this->context.get_type(node.m_right->m_res_type);
            const auto& ty_res = this->context.get_type(node.m_res_type);
            
            TRACE_FUNCTION_FR("BinOp " << ty_left << " <> " << ty_right << " = " << ty_res, "BinOp");
            
            // Boolean ops can't be overloaded, and require `bool` on both sides
            if( node.m_op == ::HIR::ExprNode_BinOp::Op::BoolAnd || node.m_op == ::HIR::ExprNode_BinOp::Op::BoolOr )
            {
                assert(node.m_res_type.m_data.is_Primitive() && node.m_res_type.m_data.as_Primitive() == ::HIR::CoreType::Bool);
                this->context.apply_equality( node.span(), node.m_res_type, node.m_left->m_res_type );
                this->context.apply_equality( node.span(), node.m_res_type, node.m_right->m_res_type );
                return ;
            }

            struct H {
                static bool type_is_num_prim(const ::HIR::TypeRef& ty) {
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (e),
                    (
                        return false;
                        ),
                    (Primitive,
                        switch(e)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                            return false;
                        default:
                            return true;
                        }
                        ),
                    (Infer,
                        switch(e.ty_class)
                        {
                        case ::HIR::InferClass::None:
                            return false;
                        case ::HIR::InferClass::Integer:
                        case ::HIR::InferClass::Float:
                            return true;
                        }
                        )
                    )
                    return false;
                }
            };
            
            // Result is known to be a primitive, and left is infer
            if( H::type_is_num_prim(ty_res) && H::type_is_num_prim(ty_left) ) {
                // If the op is a aritmatic op, and the left is a primtive ivar
                // - The result must be the same as the input
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:
                case ::HIR::ExprNode_BinOp::Op::BoolOr:
                    break;
                default:
                    this->context.apply_equality(node.span(), node.m_res_type, ty_left);
                    break;
                }
            }
            
            // NOTE: Inferrence rules when untyped integer literals are in play
            // - `impl Add<Foo> for u32` is valid, and makes `1 + Foo` work
            //  - But `[][0] + Foo` doesn't
            //  - Adding `impl Add<Foo> for u64` leads to "`Add<Foo>` is not implemented for `i32`"
            // - HACK! (kinda?) libcore includes impls of `Add<i32> for i32`, which means that overloads work for inferrence purposes
            if( H::type_is_num_prim(ty_left) && H::type_is_num_prim(ty_right) )
            {
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    this->context.apply_equality(node.span(), node.m_left->m_res_type, node.m_right->m_res_type);
                    this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
                    break;
               
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:    BUG(node.span(), "Encountered BoolAnd in primitive op");
                case ::HIR::ExprNode_BinOp::Op::BoolOr:     BUG(node.span(), "Encountered BoolOr in primitive op");

                case ::HIR::ExprNode_BinOp::Op::Add:
                case ::HIR::ExprNode_BinOp::Op::Sub:
                case ::HIR::ExprNode_BinOp::Op::Mul:
                case ::HIR::ExprNode_BinOp::Op::Div:
                case ::HIR::ExprNode_BinOp::Op::Mod: {
                    this->context.apply_equality(node.span(), node.m_left->m_res_type, node.m_right->m_res_type);
                    this->context.apply_equality(node.span(), node.m_res_type, node.m_left->m_res_type);
                    
                    const auto& ty = this->context.get_type(node.m_left->m_res_type);
                    TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                        switch(e)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                            ERROR(node.span(), E0000, "Invalid use of arithmatic on " << ty);
                            break;
                        default:
                            break;
                        }
                    )
                    } break;
                case ::HIR::ExprNode_BinOp::Op::And:
                case ::HIR::ExprNode_BinOp::Op::Or:
                case ::HIR::ExprNode_BinOp::Op::Xor: {
                    this->context.apply_equality(node.span(), node.m_left->m_res_type, node.m_right->m_res_type);
                    this->context.apply_equality(node.span(), node.m_res_type, node.m_left->m_res_type);
                    const auto& ty = this->context.get_type(node.m_left->m_res_type);
                    TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                        switch(e)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            ERROR(node.span(), E0000, "Invalid use of bitwise operation on " << ty);
                            break;
                        default:
                            break;
                        }
                    )
                    } break;
                case ::HIR::ExprNode_BinOp::Op::Shr:
                case ::HIR::ExprNode_BinOp::Op::Shl: {
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_right.m_data), (e),
                    (
                        ),
                    (Primitive,
                        switch(e)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            ERROR(node.span(), E0000, "Invalid type for shift count - " << ty_right);
                        default:
                            break;
                        }
                        ),
                    (Infer,
                        if( e.ty_class != ::HIR::InferClass::Integer ) {
                            ERROR(node.span(), E0000, "Invalid type for shift count - " << ty_right);
                        }
                        )
                    )
                    
                    this->context.apply_equality(node.span(), node.m_res_type, node.m_left->m_res_type);
                    const auto& ty = this->context.get_type(node.m_left->m_res_type);
                    TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                        switch(e)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            ERROR(node.span(), E0000, "Invalid use of bitwise operation on " << ty);
                            break;
                        default:
                            break;
                        }
                    )
                    } break;
                }
            }
            else
            {
                const auto& ty_left  = this->context.get_type(node.m_left->m_res_type );
                const auto& ty_right = this->context.get_type(node.m_right->m_res_type);
                //const auto& ty_res = this->context.get_type(node.m_res_type);
                
                const char* item_name = nullptr;
                bool has_output = true;
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:  item_name = "eq"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu: item_name = "eq"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = "ord"; has_output = false; break;
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:    BUG(node.span(), "Encountered BoolAnd in overload search");
                case ::HIR::ExprNode_BinOp::Op::BoolOr:     BUG(node.span(), "Encountered BoolOr in overload search");

                case ::HIR::ExprNode_BinOp::Op::Add: item_name = "add"; break;
                case ::HIR::ExprNode_BinOp::Op::Sub: item_name = "sub"; break;
                case ::HIR::ExprNode_BinOp::Op::Mul: item_name = "mul"; break;
                case ::HIR::ExprNode_BinOp::Op::Div: item_name = "div"; break;
                case ::HIR::ExprNode_BinOp::Op::Mod: item_name = "rem"; break;
                
                case ::HIR::ExprNode_BinOp::Op::And: item_name = "bitand"; break;
                case ::HIR::ExprNode_BinOp::Op::Or:  item_name = "bitor";  break;
                case ::HIR::ExprNode_BinOp::Op::Xor: item_name = "bitxor"; break;
                
                case ::HIR::ExprNode_BinOp::Op::Shr: item_name = "shr"; break;
                case ::HIR::ExprNode_BinOp::Op::Shl: item_name = "shl"; break;
                }
                assert(item_name);
                
                // Search for ops trait impl
                const ::HIR::TraitImpl* impl_ptr = nullptr;
                ::HIR::TypeRef  possible_right_type;
                unsigned int count = 0;
                const auto& ops_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);
                ::HIR::PathParams   ops_trait_pp;
                ops_trait_pp.m_types.push_back( ty_right.clone() );
                DEBUG("Searching for impl " << ops_trait << "< " << ty_right << "> for " << ty_left);
                bool found_bound = this->context.find_trait_impls_bound(sp, ops_trait, ops_trait_pp,  ty_left,
                    [&](const auto& args, const auto& assoc) {
                        assert(args.m_types.size() == 1);
                        const auto& arg_type = args.m_types[0];
                        // TODO: if arg_type mentions Self?
                        auto cmp = arg_type.compare_with_placeholders(node.span(), ty_right, this->context.callback_resolve_infer());
                        if( cmp == ::HIR::Compare::Unequal ) {
                            DEBUG("- (fail) bounded impl " << ops_trait << "<" << arg_type << "> (ty_right = " << this->context.get_type(ty_right));
                            return false;
                        }
                        count += 1;
                        if( cmp == ::HIR::Compare::Equal ) {
                            return true;
                        }
                        else {
                            if( possible_right_type == ::HIR::TypeRef() ) {
                                DEBUG("- Set possibility for " << ty_right << " - " << arg_type);
                                possible_right_type = arg_type.clone();
                            }
                        
                            return false;
                        }
                    });
                // - Only set found_exact if either found_bound returned true, XOR this returns true
                bool found_exact = found_bound ^ this->context.m_crate.find_trait_impls(ops_trait, ty_left, this->context.callback_resolve_infer(),
                    [&](const auto& impl) {
                        assert( impl.m_trait_args.m_types.size() == 1 );
                        const auto& arg_type = impl.m_trait_args.m_types[0];
                        
                        if( monomorphise_type_needed(arg_type) ) {
                            return true;
                            //TODO(node.span(), "Compare ops trait type when it contains generics - " << arg_type << " == " << ty_right);
                        }
                        auto cmp = arg_type.compare_with_placeholders(node.span(), ty_right, this->context.callback_resolve_infer());
                        if( cmp == ::HIR::Compare::Unequal ) {
                            return false;
                        }
                        count += 1;
                        impl_ptr = &impl;
                        if( cmp == ::HIR::Compare::Equal ) {
                            DEBUG("Operator impl exact match - '"<<item_name<<"' - " << arg_type << " == " << ty_right);
                            return true;
                        }
                        else {
                            DEBUG("Operator impl fuzzy match - '"<<item_name<<"' - " << arg_type << " == " << ty_right);
                            if( possible_right_type == ::HIR::TypeRef() ) {
                                DEBUG("- Set possibility for " << ty_right << " - " << arg_type);
                                possible_right_type = arg_type.clone();
                            }
                            return false;
                        }
                    }
                    );
                // If there wasn't an exact match, BUT there was one partial match - assume the partial match is what we want
                if( !found_exact && count == 1 ) {
                    assert( possible_right_type != ::HIR::TypeRef() );
                    this->context.apply_equality(node.span(), possible_right_type, ty_right);
                }
                if( count > 0 ) {
                    // - If the output type is variable
                    if( has_output )
                    {
                        // - If an impl block was found
                        if( impl_ptr )
                        {
                            const auto& type = impl_ptr->m_types.at("Output");
                            if( monomorphise_type_needed(type) ) {
                                TODO(node.span(), "BinOp output = " << type);
                            }
                            else {
                                this->context.apply_equality(node.span(), node.m_res_type, type);
                            }
                        }
                        else
                        {
                            const auto& ty_right = this->context.get_type(node.m_right->m_res_type);
                            
                            ::HIR::PathParams   tpp;
                            DEBUG("- ty_right = " << ty_right);
                            tpp.m_types.push_back( ty_right.clone() );
                            auto type = ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsKnown({
                                box$( ty_left.clone() ),
                                ::HIR::GenericPath( ops_trait, mv$(tpp) ),
                                "Output",
                                {}
                                }) );
                            this->context.apply_equality(node.span(), node.m_res_type, type);
                        }
                    }
                    else
                    {
                        this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
                    }
                }
                else {
                    // TODO: Determine if this could ever succeed, and error if not.
                    // - Likely `count` can help, but only if fuzzy matching of the impl type is done
                }
            }
        }
        // - UniOp: Look for overload or primitive
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                // - Handled above?
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                // - Handled above?
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(node.span(), E0000, "Invalid use of ! on " << ty);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty);
                        break;
                    }
                )
                else {
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Infer, e,
                        if( e.ty_class == ::HIR::InferClass::Integer ) {
                            this->context.apply_equality(node.span(), node.m_res_type, ty);
                            break;
                        }
                    )
                    // TODO: Search for an implementation of ops::Not
                }
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        ERROR(node.span(), E0000, "Invalid use of - on " << ty);
                        break;
                    default:
                        this->context.apply_equality(node.span(), node.m_res_type, ty);
                        break;
                    }
                )
                else {
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Infer, e,
                        if( e.ty_class == ::HIR::InferClass::Integer || e.ty_class == ::HIR::InferClass::Float ) {
                            this->context.apply_equality(node.span(), node.m_res_type, ty);
                            break;
                        }
                    )
                    // TODO: Search for an implementation of ops::Neg
                }
                break;
            }
        }
        // - Cast: Nothing needs to happen
        void visit(::HIR::ExprNode_Cast& node) override
        {
            const auto& val_ty = this->context.get_type( node.m_value->m_res_type );
            const auto& target_ty = this->context.get_type( node.m_res_type );
            TU_MATCH_DEF(::HIR::TypeRef::Data, (target_ty.m_data), (e),
            (
                ERROR(node.span(), E0000, "Invalid cast");
                ),
            (Primitive,
                switch(e)
                {
                case ::HIR::CoreType::Char:
                    break;
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Bool:
                    ERROR(node.span(), E0000, "Invalid cast");
                    break;
                default:
                    // TODO: Check that the source and destination are integer types.
                    break;
                }
                ),
            (Borrow,
                // TODO: Actually a coerce - check it
                ),
            (Infer,
                // - wait
                ),
            (Pointer,
                // Valid source:
                // *<same> <any>
                // *<other> <same>
                // &<same> <same>
                TU_MATCH_DEF(::HIR::TypeRef::Data, (val_ty.m_data), (e2),
                (
                    ),
                (Infer,
                    ),
                (Borrow,
                    if( e.type != e2.type ) {
                        // ERROR
                    }
                    this->context.apply_equality(node.span(), *e2.inner, *e.inner);
                    ),
                (Pointer,
                    if( e.type != e2.type ) {
                        this->context.apply_equality(node.span(), *e2.inner, *e.inner);
                    }
                    else {
                        // Nothing
                    }
                    )
                )
                )
            )
            // TODO: Check cast validity and do inferrence
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Index: Look for implementation of the Index trait
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_FR("_Index","_Index");
            ::HIR::ExprVisitorDef::visit(node);
            const auto& path_Index = this->context.m_crate.get_lang_item_path(node.span(), "index");
            
            // NOTE: Indexing triggers autoderef
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            
            ::HIR::PathParams   trait_pp;
            trait_pp.m_types.push_back( this->context.get_type(node.m_index->m_res_type).clone() );
            do {
                const auto& ty = this->context.get_type(*current_ty);
                DEBUG("_Index: (: " << ty << ")[: " << trait_pp.m_types[0] << "]");
                
                ::HIR::TypeRef  possible_index_type;
                unsigned int count = 0;
                bool rv = this->context.find_trait_impls(node.span(), path_Index,trait_pp, ty, [&](const auto& args, const auto& assoc) {
                    assert( args.m_types.size() == 1 );
                    const auto& impl_index = args.m_types[0];
                    
                    auto cmp = impl_index.compare_with_placeholders(node.span(), trait_pp.m_types[0], this->context.callback_resolve_infer());
                    if( cmp == ::HIR::Compare::Unequal)
                        return false;
                    // TODO: use `assoc`
                    if( cmp == ::HIR::Compare::Equal ) {
                        return true;
                    }
                    possible_index_type = impl_index.clone();
                    count += 1;
                    return false;
                    });
                if( rv ) {
                    break;
                }
                else if( count == 1 ) {
                    assert( possible_index_type != ::HIR::TypeRef() );
                    this->context.apply_equality(node.span(), node.m_index->m_res_type, possible_index_type);
                    break;
                }
                else {
                    // Either no matches, or multiple fuzzy matches
                }
                
                
                deref_count += 1;
                current_ty = this->context.autoderef(node.span(), ty,  tmp_type);
            } while( current_ty );
            
            if( current_ty )
            {
                const auto& index_ty = this->context.get_type(node.m_index->m_res_type);
                if( deref_count > 0 )
                    DEBUG("Adding " << deref_count << " dereferences");
                while( deref_count > 0 )
                {
                    node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Deref(node.span(), mv$(node.m_value)) );
                    this->context.add_ivars( node.m_value->m_res_type );
                    deref_count -= 1;
                }
                
                // Set output to `< "*current_ty" as Index<"index_ty> >::Output`
                // TODO: Get the output type from the bound/impl in `find_trait_impls` (reduces load on expand_associated_types)
                auto tp = ::HIR::GenericPath( path_Index );
                tp.m_params.m_types.push_back( index_ty.clone() );
                auto out_type = ::HIR::TypeRef::new_path(
                    ::HIR::Path(::HIR::Path::Data::Data_UfcsKnown {
                        box$(this->context.get_type(*current_ty).clone()),
                        mv$(tp),
                        "Output",
                        {}
                    }),
                    {}
                    );
                
                this->context.apply_equality( node.span(), node.m_res_type, out_type );
            }
        }
        // - Deref: Look for impl of Deref
        void visit(::HIR::ExprNode_Deref& node) override
        {
            const auto& ty = this->context.get_type( node.m_value->m_res_type );
            TRACE_FUNCTION_FR("Deref *{" << ty << "}", this->context.get_type(node.m_res_type));
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
                this->context.apply_equality(node.span(), node.m_res_type, *e.inner);
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->context.apply_equality(node.span(), node.m_res_type, ::HIR::TypeRef::new_slice(e.inner->clone()));
            )
            else {
                // TODO: Search for Deref impl
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            const Span& sp = node.span();
            auto& arg_types = node.m_arg_types;
            if( arg_types.size() == 0 )
            {
                auto& path_params = node.m_path.m_params;
                auto monomorph_cb = [&](const auto& gt)->const auto& {
                        const auto& e = gt.m_data.as_Generic();
                        if( e.name == "Self" )
                            TODO(sp, "Handle 'Self' when monomorphising type");
                        if( e.binding < 256 ) {
                            auto idx = e.binding;
                            if( idx >= path_params.m_types.size() ) {
                                BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                            }
                            return path_params.m_types[idx];
                        }
                        else if( e.binding < 512 ) {
                            BUG(sp, "Method-level parameter on struct/enum");
                        }
                        else {
                            BUG(sp, "Generic bounding out of total range");
                        }
                    };
                
                if( node.m_is_struct )
                {
                    const auto& str = this->context.m_crate.get_struct_by_path(sp, node.m_path.m_path);
                    fix_param_count(sp, this->context, node.m_path, str.m_params,  path_params);
                    const auto& fields = str.m_data.as_Tuple();
                    arg_types.reserve( fields.size() );
                    for(const auto& fld : fields)
                    {
                        if( monomorphise_type_needed(fld.ent) ) {
                            arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fld.ent,  monomorph_cb)) );
                        }
                        else {
                            arg_types.push_back( fld.ent.clone() );
                        }
                    }
                    
                    arg_types.push_back( ::HIR::TypeRef(node.m_path.clone()) );
                }
                else
                {
                    const auto& variant_name = node.m_path.m_path.m_components.back();
                    auto type_path = node.m_path.m_path;
                    type_path.m_components.pop_back();
                    
                    const auto& enm = this->context.m_crate.get_enum_by_path(sp, type_path);
                    fix_param_count(sp, this->context, node.m_path, enm.m_params,  path_params);
                    
                    auto it = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x){ return x.first == variant_name; });
                    if( it == enm.m_variants.end() ) {
                        ERROR(sp, E0000, "Unable to find variant '" << variant_name << " of " << type_path);
                    }
                    const auto& fields = it->second.as_Tuple();
                    arg_types.reserve( fields.size() );
                    for(const auto& fld : fields)
                    {
                        if( monomorphise_type_needed(fld) ) {
                            arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fld,  monomorph_cb)) );
                        }
                        else {
                            arg_types.push_back( fld.clone() );
                        }
                    }
                    arg_types.push_back( ::HIR::TypeRef( ::HIR::GenericPath(type_path, path_params.clone()) ) );
                }
                
                if( node.m_args.size() != arg_types.size() - 1 ) {
                    ERROR(sp, E0000, "Incorrect number of arguments to " << node.m_path);
                }
                DEBUG("--- RESOLVED");
            }
            
            for( unsigned int i = 0; i < arg_types.size() - 1; i ++ )
            {
                auto& arg_expr_ptr = node.m_args[i];
                const auto& arg_ty = arg_types[i];
                DEBUG("Arg " << i << ": " << arg_ty);
                this->context.apply_equality(sp, arg_ty, arg_expr_ptr->m_res_type,  &arg_expr_ptr);
            }
            
            DEBUG("_TupleVariant - Return: " << arg_types.back());
            this->context.apply_equality(sp, node.m_res_type, arg_types.back() /*,  &this_node_ptr*/);
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit_call(const Span& sp,
                ::HIR::Path& path, bool is_method,
                ::std::vector< ::HIR::ExprNodeP>& args, ::HIR::TypeRef& res_type, ::HIR::ExprNodeP& this_node_ptr,
                ::HIR::ExprCallCache& cache
                )
        {
            TRACE_FUNCTION_F("path = " << path);
            unsigned int arg_ofs = (is_method ? 1 : 0);
            
            if( cache.m_arg_types.size() == 0 )
            {
                const ::HIR::Function*  fcn_ptr = nullptr;
                ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>    monomorph_cb;
                
                TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
                (Generic,
                    const auto& fcn = this->context.m_crate.get_function_by_path(sp, e.m_path);
                    fix_param_count(sp, this->context, path, fcn.m_params,  e.m_params);
                    fcn_ptr = &fcn;
                    cache.m_fcn_params = &fcn.m_params;
                    
                    //const auto& params_def = fcn.m_params;
                    const auto& path_params = e.m_params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& e = gt.m_data.as_Generic();
                            if( e.name == "Self" || e.binding == 0xFFFF )
                                TODO(sp, "Handle 'Self' when monomorphising");
                            if( e.binding < 256 ) {
                                BUG(sp, "Impl-level parameter on free function (#" << e.binding << " " << e.name << ")");
                            }
                            else if( e.binding < 512 ) {
                                auto idx = e.binding - 256;
                                if( idx >= path_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                                }
                                return this->context.get_type(path_params.m_types[idx]);
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    ),
                (UfcsKnown,
                    const auto& trait = this->context.m_crate.get_trait_by_path(sp, e.trait.m_path);
                    fix_param_count(sp, this->context, path, trait.m_params, e.trait.m_params);
                    const auto& fcn = trait.m_values.at(e.item).as_Function();
                    fix_param_count(sp, this->context, path, fcn.m_params,  e.params);
                    cache.m_fcn_params = &fcn.m_params;
                    cache.m_top_params = &trait.m_params;
                    
                    // TODO: Check/apply trait bounds (apply = closure arguments or fixed trait args)
                    
                    fcn_ptr = &fcn;
                    
                    const auto& trait_params = e.trait.m_params;
                    const auto& path_params = e.params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return *e.type;
                            }
                            else if( ge.binding < 256 ) {
                                auto idx = ge.binding;
                                if( idx >= trait_params.m_types.size() ) {
                                    BUG(sp, "Generic param (impl) out of input range - " << idx << " '"<<ge.name<<"' >= " << trait_params.m_types.size());
                                }
                                return this->context.get_type(trait_params.m_types[idx]);
                            }
                            else if( ge.binding < 512 ) {
                                auto idx = ge.binding - 256;
                                if( idx >= path_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '"<<ge.name<<"' >= " << path_params.m_types.size());
                                }
                                return this->context.get_type(path_params.m_types[idx]);
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    ),
                (UfcsUnknown,
                    TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                    ),
                (UfcsInherent,
                    const ::HIR::TypeImpl* impl_ptr = nullptr;
                    this->context.m_crate.find_type_impls(*e.type, [&](const auto& ty)->const auto& {
                            if( ty.m_data.is_Infer() )
                                return this->context.get_type(ty);
                            else
                                return ty;
                        },
                        [&](const auto& impl) {
                            DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            for(const auto& v : impl.m_methods)
                                DEBUG(" > " << v.first);
                            auto it = impl.m_methods.find(e.item);
                            if( it == impl.m_methods.end() )
                                return false;
                            fcn_ptr = &it->second;
                            impl_ptr = &impl;
                            return true;
                        });
                    if( !fcn_ptr ) {
                        ERROR(sp, E0000, "Failed to locate function " << path);
                    }
                    assert(impl_ptr);
                    fix_param_count(sp, this->context, path, fcn_ptr->m_params,  e.params);
                    cache.m_fcn_params = &fcn_ptr->m_params;
                    
                    
                    auto& impl_params = cache.m_ty_impl_params;
                    if( impl_ptr->m_params.m_types.size() > 0 ) {
                        impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                        impl_ptr->m_type.match_generics(sp, *e.type, this->context.callback_resolve_infer(), [&](auto idx, const auto& ty) {
                            assert( idx < impl_params.m_types.size() );
                            impl_params.m_types[idx] = ty.clone();
                            });
                        for(const auto& ty : impl_params.m_types)
                            assert( !( ty.m_data.is_Infer() && ty.m_data.as_Infer().index == ~0u) );
                    }
                    const auto& fcn_params = e.params;
                    monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return this->context.get_type(*e.type);
                            }
                            else if( ge.binding < 256 ) {
                                auto idx = ge.binding;
                                if( idx >= impl_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                                }
                                return this->context.get_type(impl_params.m_types[idx]);
                            }
                            else if( ge.binding < 512 ) {
                                auto idx = ge.binding - 256;
                                if( idx >= fcn_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                                }
                                return this->context.get_type(fcn_params.m_types[idx]);
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };
                    )
                )

                assert( fcn_ptr );
                const auto& fcn = *fcn_ptr;
                
                if( args.size() + (is_method ? 1 : 0) != fcn.m_args.size() ) {
                    ERROR(sp, E0000, "Incorrect number of arguments to " << path);
                }
                
                for(const auto& arg : fcn.m_args) {
                    if( monomorphise_type_needed(arg.second) ) {
                        cache.m_arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, arg.second,  monomorph_cb)) );
                    }
                    else {
                        cache.m_arg_types.push_back( arg.second.clone() );
                    }
                }
                if( monomorphise_type_needed(fcn.m_return) ) {
                    cache.m_arg_types.push_back( this->context.expand_associated_types(sp, monomorphise_type_with(sp, fcn.m_return,  monomorph_cb)) );
                }
                else {
                    cache.m_arg_types.push_back( fcn.m_return.clone() );
                }
                
                cache.m_monomorph_cb = mv$(monomorph_cb);
            }
            
            for( unsigned int i = arg_ofs; i < cache.m_arg_types.size() - 1; i ++ )
            {
                auto& arg_expr_ptr = args[i - arg_ofs];
                const auto& arg_ty = cache.m_arg_types[i];
                DEBUG("Arg " << i << ": " << arg_ty);
                this->context.apply_equality(sp, arg_ty, arg_expr_ptr->m_res_type,  &arg_expr_ptr);
            }
            
            // TODO: Remove if not needed
            // HACK: Expand UFCS again
            cache.m_arg_types.back() = this->context.expand_associated_types(sp, mv$(cache.m_arg_types.back()));
            
            DEBUG("RV " << cache.m_arg_types.back());
            this->context.apply_equality(sp, res_type, cache.m_arg_types.back(),  &this_node_ptr);
            
            // Check generic bounds (after links between args and params are known)
            // - HACK! Below just handles closures and fn traits.
            // - TODO: Make this FAR more generic than it is
            for(const auto& bound : cache.m_fcn_params->m_bounds)
            {
                TU_MATCH(::HIR::GenericBound, (bound), (be),
                (Lifetime,
                    ),
                (TypeLifetime,
                    ),
                (TraitBound,
                    auto real_type = monomorphise_type_with(sp, be.type, cache.m_monomorph_cb);
                    auto real_trait = monomorphise_genericpath_with(sp, be.trait.m_path, cache.m_monomorph_cb, false);
                    DEBUG("Bound " << be.type << ":  " << be.trait);
                    DEBUG("= (" << real_type << ": " << real_trait << ")");
                    auto monomorph_bound = [&](const auto& gt)->const auto& {
                        return gt;
                        };
                    const auto& trait_params = be.trait.m_path.m_params;
                    // TODO: Detect marker traits
                    const auto& trait_gp = be.trait.m_path;
                    auto rv = this->context.find_trait_impls(sp, trait_gp.m_path, real_trait.m_params, real_type, [&](const auto& pp, const auto& at) {
                        if( pp.m_types.size() != trait_params.m_types.size() ) {
                            BUG(sp, "Parameter mismatch");
                        }
                        DEBUG("Apply equality of " << pp << " and " << trait_params << " (once monomorphed)");
                        for(unsigned int i = 0; i < pp.m_types.size(); i ++ ) {
                            auto& l = pp.m_types[i];
                            auto r = monomorphise_type_with(sp, trait_params.m_types[i], cache.m_monomorph_cb);
                            DEBUG(i << " " << l << " and " << r);
                            //this->context.apply_equality(sp, pp.m_types[i], monomorph_bound, trait_params.m_types[i], cache.m_monomorph_cb, nullptr);
                            this->context.apply_equality(sp, pp.m_types[i], monomorph_bound, real_trait.m_params.m_types[i], IDENT_CR, nullptr);
                        }
                        // TODO: Use `at`
                        // - Check if the associated type bounds are present
                        #if 0
                        for( const auto& assoc : be.trait.m_type_bounds ) {
                            auto it = at.find( assoc.first );
                            if( it == at.end() )
                                TODO(sp, "Bounded associated type " << assoc.first << " wasn't in list provided by find_trait_impls");
                            DEBUG("Equate (impl) " << it->second << " and (bound) " << assoc.second);
                            //this->context.apply_equality(sp, it->second, IDENT_CR, assoc.second, cache.m_monomorph_cb, nullptr);
                            auto other_ty = monomorphise_type_with(sp, assoc.second, cache.m_monomorph_cb, true);
                            this->context.apply_equality(sp, it->second, other_ty);
                        }
                        DEBUG("at = " << at);
                        #endif
                        return true;
                        });
                    if( !rv ) {
                        // TODO: Enable this once marker impls are checked correctly
                        //if( this->context.type_contains_ivars(real_type) ) {
                        //    ERROR(sp, E0000, "No suitable impl of " << real_trait << " for " << real_type);
                        //}
                        DEBUG("- No impl of " << be.trait.m_path << " for " << real_type);
                        continue ;
                    }
                    // TODO: Only do this if associated type was missing in list from impl
                    // - E.g. FnMut<Output=Bar>
                    #if 1
                    for( const auto& assoc : be.trait.m_type_bounds ) {
                        auto ty = ::HIR::TypeRef( ::HIR::Path(::HIR::Path::Data::Data_UfcsKnown { box$(real_type.clone()), be.trait.m_path.clone(), assoc.first, {} }) );
                        // TODO: I'd like to avoid this copy, but expand_associated_types doesn't use the monomorph callback
                        auto other_ty = monomorphise_type_with(sp, assoc.second, cache.m_monomorph_cb, true);
                        DEBUG("Checking associated type bound " << assoc.first << " (" << ty << ") = " << assoc.second << " (" << other_ty << ")");
                        //this->context.apply_equality( sp, ty, [](const auto&x)->const auto&{return x;}, assoc.csecond, cache.m_monomorph_cb, nullptr );
                        this->context.apply_equality( sp, ty, other_ty );
                    }
                    #endif
                    ),
                (TypeEquality,
                    #if 0
                    this->context.apply_equality(sp, be.type, cache.m_monomorph_cb, be.other_type, cache.m_monomorph_cb, nullptr);
                    #else
                    // Search for an impl of this trait? Or just do a ufcs expand and force equality
                    auto real_type_left = this->context.expand_associated_types(sp, monomorphise_type_with(sp, be.type, cache.m_monomorph_cb));
                    DEBUG("real_type_left = " << real_type_left);
                    auto real_type_right = this->context.expand_associated_types(sp, monomorphise_type_with(sp, be.other_type, cache.m_monomorph_cb));
                    DEBUG("real_type_right = " << real_type_right);
                    
                    this->context.apply_equality(sp, real_type_left, real_type_right);
                    #endif
                    )
                )
            }
        }
        
        // - Call Path: Locate path and build return
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            auto& node_ptr = *m_node_ptr_ptr;
            TRACE_FUNCTION_F("CallPath " << node.m_path);
            assert(node_ptr.get() == &node);
            // - Pass m_arg_types as a cache to avoid constant lookups
            visit_call(node.span(), node.m_path, false, node.m_args, node.m_res_type, node_ptr,  node.m_cache);
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Call Value: If type is known, locate impl of Fn/FnMut/FnOnce
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            DEBUG("(CallValue) ty = " << ty);
            
            if( node.m_arg_types.size() == 0 )
            {
                TU_MATCH_DEF(decltype(ty.m_data), (ty.m_data), (e),
                (
                    ::HIR::TypeRef  fcn_args_tup;
                    ::HIR::TypeRef  fcn_ret;
                    // Locate impl of FnOnce
                    const auto& lang_FnOnce = this->context.m_crate.get_lang_item_path(node.span(), "fn_once");
                    ::HIR::PathParams   trait_pp;
                    trait_pp.m_types.push_back( this->context.new_ivar_tr() );  // TODO: Bind to arguments?
                    auto was_bounded = this->context.find_trait_impls_bound(node.span(), lang_FnOnce, trait_pp, ty, [&](const auto& args, const auto& assoc) {
                            const auto& tup = args.m_types[0];
                            if( !tup.m_data.is_Tuple() )
                                ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                            fcn_args_tup = tup.clone();
                            return true;
                            });
                    if( was_bounded )
                    {
                        // RV must be in a bound
                        fcn_ret = ::HIR::TypeRef( ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                            box$( ty.clone() ),
                            ::HIR::GenericPath(lang_FnOnce),
                            "Output",
                            {}
                            })) );
                        fcn_ret.m_data.as_Path().path.m_data.as_UfcsKnown().trait.m_params.m_types.push_back( fcn_args_tup.clone() );
                    }
                    else if( !ty.m_data.is_Generic() )
                    {
                        TODO(node.span(), "Search for other implementations of FnOnce for " << ty);
                    }
                    else
                    {
                        // Didn't find anything. Error?
                        ERROR(node.span(), E0000, "Unable to find an implementation of Fn* for " << ty);
                    }
                    
                    node.m_arg_types = mv$( fcn_args_tup.m_data.as_Tuple() );
                    node.m_arg_types.push_back( mv$(fcn_ret) );
                    ),
                (Closure,
                    for( const auto& arg : e.m_arg_types )
                        node.m_arg_types.push_back( arg.clone() );
                    node.m_arg_types.push_back( e.m_rettype->clone() );
                    ),
                (Function,
                    for( const auto& arg : e.m_arg_types )
                        node.m_arg_types.push_back( arg.clone() );
                    node.m_arg_types.push_back( e.m_rettype->clone() );
                    ),
                (Infer,
                    )
                )
            }
            
            if( node.m_arg_types.size() > 0 )
            {
                if( node.m_args.size() + 1 != node.m_arg_types.size() ) {
                    ERROR(node.span(), E0000, "Incorrect number of arguments when calling " << ty);
                }
                
                for( unsigned int i = 0; i < node.m_args.size(); i ++ )
                {
                    auto& arg_node = node.m_args[i];
                    this->context.apply_equality(node.span(), node.m_arg_types[i], arg_node->m_res_type,  &arg_node);
                }
                // TODO: Allow infer
                this->context.apply_equality(node.span(), node.m_res_type, node.m_arg_types.back());
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Call Method: Locate method on type
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            auto& node_ptr = *m_node_ptr_ptr;
            
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_method_path.m_data.is_Generic() && node.m_method_path.m_data.as_Generic().m_path.m_components.size() == 0 )
            {
                const auto& ty = this->context.get_type(node.m_value->m_res_type);
                DEBUG("(CallMethod) ty = " << ty);
                // Using autoderef, locate this method on the type
                ::HIR::Path   fcn_path { ::HIR::SimplePath() };
                unsigned int deref_count = this->context.autoderef_find_method(node.span(), ty, node.m_method,  fcn_path);
                if( deref_count != ~0u )
                {
                    DEBUG("Found method " << fcn_path);
                    node.m_method_path = mv$(fcn_path);
                    // NOTE: Steals the params from the node
                    TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                    (Generic,
                        ),
                    (UfcsUnknown,
                        ),
                    (UfcsKnown,
                        e.params = mv$(node.m_params);
                        ),
                    (UfcsInherent,
                        e.params = mv$(node.m_params);
                        )
                    )
                    DEBUG("Adding " << deref_count << " dereferences");
                    while( deref_count > 0 )
                    {
                        node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Deref(node.span(), mv$(node.m_value)) );
                        this->context.add_ivars( node.m_value->m_res_type );
                        deref_count -= 1;
                    }
                }
                else
                {
                    // Return early, don't know enough yet
                    return ;
                }
            }
            
            assert(node_ptr.get() == &node);
            visit_call(node.span(), node.m_method_path, true, node.m_args, node.m_res_type, node_ptr,  node.m_cache);
        }
        // - Field: Locate field on type
        void visit(::HIR::ExprNode_Field& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            ::HIR::TypeRef  out_type;
            unsigned int deref_count = this->context.autoderef_find_field(node.span(), node.m_value->m_res_type, node.m_field,  out_type);
            if( deref_count != ~0u )
            {
                assert( out_type != ::HIR::TypeRef() );
                if( deref_count > 0 )
                    DEBUG("Adding " << deref_count << " dereferences");
                while( deref_count > 0 )
                {
                    node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Deref(node.span(), mv$(node.m_value)) );
                    this->context.add_ivars( node.m_value->m_res_type );
                    deref_count -= 1;
                }
                this->context.apply_equality(node.span(), node.m_res_type, out_type);
            }
        }
        // - PathValue: Insert type from path
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            const auto& sp = node.span();
            TU_MATCH(::HIR::Path::Data, (node.m_path.m_data), (e),
            (Generic,
                switch(node.m_target) {
                case ::HIR::ExprNode_PathValue::UNKNOWN:
                    BUG(sp, "Unknown target PathValue encountered with Generic path");
                case ::HIR::ExprNode_PathValue::FUNCTION: {
                    const auto& f = this->context.m_crate.get_function_by_path(sp, e.m_path);
                    ::HIR::FunctionType ft {
                        f.m_unsafe,
                        f.m_abi,
                        box$( f.m_return.clone() ),
                        {}
                        };
                    for( const auto& arg : f.m_args )
                        ft.m_arg_types.push_back( arg.second.clone() );
                    auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
                    this->context.apply_equality(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::STATIC: {
                    const auto& v = this->context.m_crate.get_static_by_path(sp, e.m_path);
                    DEBUG("static v.m_type = " << v.m_type);
                    this->context.apply_equality(sp, node.m_res_type, v.m_type);
                    } break;
                case ::HIR::ExprNode_PathValue::CONSTANT: {
                    const auto& v = this->context.m_crate.get_constant_by_path(sp, e.m_path);
                    DEBUG("const"<<v.m_params.fmt_args()<<" v.m_type = " << v.m_type);
                    if( v.m_params.m_types.size() > 0 ) {
                        TODO(sp, "Support generic constants in typeck");
                    }
                    this->context.apply_equality(sp, node.m_res_type, v.m_type);
                    } break;
                }
                DEBUG("TODO: Get type for constant/static - " << e);
                ),
            (UfcsUnknown,
                BUG(sp, "Encountered UfcsUnknown");
                ),
            (UfcsKnown,
                TODO(sp, "Look up associated constants/statics (trait)");
                ),
            (UfcsInherent,
                TODO(sp, "Look up associated constants/statics (inherent)");
                )
            )
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Variable: Bind to same ivar
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // TODO: How to apply deref coercions here?
            // - Don't need to, instead construct "higher" nodes to avoid it
            TRACE_FUNCTION_F("var #"<<node.m_slot<<" '"<<node.m_name<<"' = " << this->context.get_type(node.m_res_type));
            this->context.apply_equality(node.span(),
                node.m_res_type, this->context.get_var_type(node.span(), node.m_slot)
                );
        }
        // - Struct literal: Semi-known types
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            const Span& sp = node.span();
            // TODO: what if this is an enum struct variant constructor?
            
            auto& val_types = node.m_value_types;
            
            if( val_types.size() == 0 )
            {
                const auto& str = this->context.m_crate.get_struct_by_path(node.span(), node.m_path.m_path);
                fix_param_count(node.span(), this->context, node.m_path.clone(), str.m_params, node.m_path.m_params);
                
                auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_Path { node.m_path.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&str) } );
                this->context.apply_equality(node.span(), node.m_res_type, ty);
                
                if( !str.m_data.is_Named() )
                    ERROR(sp, E0000, "Struct literal constructor for non-struct-like struct");
                const auto& flds_def = str.m_data.as_Named();
                
                for(auto& field : node.m_values)
                {
                    auto fld_def_it = ::std::find_if( flds_def.begin(), flds_def.end(), [&](const auto& x){ return x.first == field.first; } );
                    if( fld_def_it == flds_def.end() ) {
                        ERROR(sp, E0000, "Struct " << node.m_path << " doesn't have a field " << field.first);
                    }
                    const ::HIR::TypeRef& field_type = fld_def_it->second.ent;
                    
                    if( monomorphise_type_needed(field_type) ) {
                        val_types.push_back( monomorphise_type(sp, str.m_params, node.m_path.m_params,  field_type) );
                    }
                    else {
                        // SAFE: Can't have _ as monomorphise_type_needed checks for that
                        val_types.push_back( field_type.clone() );
                    }
                }
            }
            
            for( unsigned int i = 0; i < node.m_values.size(); i ++ )
            {
                auto& field = node.m_values[i];
                this->context.apply_equality(sp, val_types[i], field.second->m_res_type,  &field.second);
            }
            
            if( node.m_base_value ) {
                this->context.apply_equality(node.span(), node.m_res_type, node.m_base_value->m_res_type);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Tuple literal: 
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            auto& ty = this->context.get_type(node.m_res_type);
            if( !ty.m_data.is_Tuple() ) {
                this->context.dump();
                BUG(node.span(), "Return type of tuple literal wasn't a tuple - " << node.m_res_type << " = " << ty);
            }
            auto& tup_ents = ty.m_data.as_Tuple();
            assert( tup_ents.size() == node.m_vals.size() );
            
            for(unsigned int i = 0; i < tup_ents.size(); i ++)
            {
                this->context.apply_equality(node.span(), tup_ents[i], node.m_vals[i]->m_res_type,  &node.m_vals[i]);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        // - Array list
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            auto& ty = this->context.get_type(node.m_res_type);
            if( !ty.m_data.is_Array() ) {
                this->context.dump();
                BUG(node.span(), "Return type of array literal wasn't an array - " << node.m_res_type << " = " << ty);
            }
            const auto& val_type = *ty.m_data.as_Array().inner;
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& sn : node.m_vals)
                this->context.apply_equality(sn->span(), val_type, sn->m_res_type,  &sn);
        }
        // - Array (sized)
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            auto& ty = this->context.get_type(node.m_res_type);
            if( !ty.m_data.is_Array() ) {
                this->context.dump();
                BUG(node.span(), "Return type of array literal wasn't an array - " << node.m_res_type << " = " << ty);
            }
            const auto& val_type = *ty.m_data.as_Array().inner;
            ::HIR::ExprVisitorDef::visit(node);
            this->context.apply_equality(node.span(), val_type, node.m_val->m_res_type,  &node.m_val);
        }
        // - Closure
        void visit(::HIR::ExprNode_Closure& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
            
            DEBUG("_Closure: " << node.m_return << " = " << node.m_code->m_res_type);
            this->context.apply_equality(node.span(), node.m_return, node.m_code->m_res_type,  &node.m_code);
        }
    };
    
    /// Visitor that applies the inferred types, and checks that all of them are fully resolved
    class ExprVisitor_Apply:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Apply(TypecheckContext& context):
            context(context)
        {
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const char* node_ty = typeid(*node).name();
            TRACE_FUNCTION_FR(node_ty << " : " << node->m_res_type, node_ty);
            this->check_type_resolved(node->span(), node->m_res_type, node->m_res_type);
            DEBUG(node_ty << " : = " << node->m_res_type);
            ::HIR::ExprVisitorDef::visit_node_ptr(node);
        }
        
    private:
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
            (Infer,
                auto new_ty = this->context.get_type(ty).clone();
                // - Move over before checking, so that the source type mentions the correct ivar
                ty = mv$(new_ty);
                if( ty.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Failed to infer type " << ty << " in "  << top_type);
                }
                check_type_resolved(sp, ty, top_type);
                ),
            (Diverge,
                // Leaf
                ),
            (Primitive,
                // Leaf
                ),
            (Path,
                // TODO:
                ),
            (Generic,
                // Leaf
                ),
            (TraitObject,
                // TODO:
                ),
            (Array,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Slice,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Tuple,
                for(auto& st : e)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Borrow,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Pointer,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Function,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Closure,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                )
            )
        }
    };
};

void Typecheck_Code(typeck::TypecheckContext context, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    TRACE_FUNCTION;
    
    // Convert ExprPtr into unique_ptr for the execution of this function
    auto root_ptr = expr.into_unique();
    
    //context.apply_equality(expr->span(), result_type, expr->m_res_type);

    // 1. Enumerate inferrence variables and assign indexes to them
    {
        typeck::ExprVisitor_Enum    visitor { context, result_type };
        visitor.visit_node_ptr(root_ptr);
    }
    // - Apply equality between the node result and the expected type
    DEBUG("- Apply RV");
    context.apply_equality(root_ptr->span(), result_type, root_ptr->m_res_type,  &root_ptr);
    
    context.dump();
    // 2. Iterate through nodes applying rules until nothing changes
    {
        typeck::ExprVisitor_Run visitor { context };
        unsigned int count = 0;
        do {
            count += 1;
            assert(count < 1000);
            DEBUG("==== PASS " << count << " ====");
            visitor.visit_node_ptr(root_ptr);
            context.compact_ivars();
        } while( context.take_changed() || context.apply_defaults() );
        DEBUG("==== STOPPED: " << count << " passes ====");
    }
    
    // 3. Check that there's no unresolved types left
    expr = ::HIR::ExprPtr( mv$(root_ptr) );
    context.dump();
    {
        DEBUG("==== VALIDATE ====");
        typeck::ExprVisitor_Apply   visitor { context };
        expr->visit( visitor );
    }
}



namespace {
    using typeck::TypecheckContext;
    
    class OuterVisitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& m_crate;
        
        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;
        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
    public:
        OuterVisitor(::HIR::Crate& crate):
            m_crate(crate),
            m_impl_generics(nullptr),
            m_item_generics(nullptr)
        {
        }
        
    private:
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
    
    public:
        void visit_module(::HIR::PathChain p, ::HIR::Module& mod) override
        {
            DEBUG("Module has " << mod.m_traits.size() << " in-scope traits");
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("Push " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->m_crate.get_trait_by_path(Span(), trait_path) ) );
            }
            ::HIR::Visitor::visit_module(p, mod);
            for(unsigned int i = 0; i < mod.m_traits.size(); i ++ )
                m_traits.pop_back();
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            TODO(Span(), "visit_expr");
        }

        void visit_trait(::HIR::PathChain p, ::HIR::Trait& item) override
        {
            auto _ = this->set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                TypecheckContext    typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Array size " << ty);
                Typecheck_Code( mv$(typeck_context), ::HIR::TypeRef(::HIR::CoreType::Usize), e.size );
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::PathChain p, ::HIR::Function& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( item.m_code )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                for( auto& arg : item.m_args ) {
                    typeck_context.add_binding( Span(), arg.first, arg.second );
                }
                DEBUG("Function code " << p);
                Typecheck_Code( mv$(typeck_context), item.m_return, item.m_code );
            }
        }
        void visit_static(::HIR::PathChain p, ::HIR::Static& item) override {
            //auto _ = this->set_item_generics(item.m_params);
            if( item.m_value )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Static value " << p);
                Typecheck_Code( mv$(typeck_context), item.m_type, item.m_value );
            }
        }
        void visit_constant(::HIR::PathChain p, ::HIR::Constant& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( item.m_value )
            {
                TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                typeck_context.m_traits = this->m_traits;
                DEBUG("Const value " << p);
                Typecheck_Code( mv$(typeck_context), item.m_type, item.m_value );
            }
        }
        void visit_enum(::HIR::PathChain p, ::HIR::Enum& item) override {
            auto _ = this->set_item_generics(item.m_params);
            
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Usize);
            
            // TODO: Check types too?
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    TypecheckContext typeck_context { m_crate, m_impl_generics, m_item_generics };
                    typeck_context.m_traits = this->m_traits;
                    DEBUG("Enum value " << p << " - " << var.first);
                    Typecheck_Code( mv$(typeck_context), enum_type, e );
                )
            }
        }
    };
}

void Typecheck_Expressions(::HIR::Crate& crate)
{
    OuterVisitor    visitor { crate };
    visitor.visit_crate( crate );
}
