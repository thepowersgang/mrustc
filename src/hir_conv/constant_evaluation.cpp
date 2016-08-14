/*
 * Evaluate constants
 *
 * HACK - Should be replaced with a reentrant typeck/mir pass
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <algorithm>

namespace {
    ::HIR::Literal clone_literal(const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            return ::HIR::Literal();
            ),
        (List,
            ::std::vector< ::HIR::Literal>  vals;
            for(const auto& val : e) {
                vals.push_back( clone_literal(val) );
            }
            return ::HIR::Literal( mv$(vals) );
            ),
        (Integer,
            return ::HIR::Literal(e);
            ),
        (Float,
            return ::HIR::Literal(e);
            ),
        (String,
            return ::HIR::Literal(e);
            )
        )
        throw "";
    }
    
    enum class EntType {
        Function,
        Constant,
        Struct,
    };
    const void* get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, EntType et)
    {
        if( path.m_crate_name != "" )
            TODO(sp, "get_ent_simplepath in crate");
        
        const ::HIR::Module* mod = &crate.m_root_module;
        for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        
        switch( et )
        {
        case EntType::Function: {
            auto it = mod->m_value_items.find( path.m_components.back() );
            if( it == mod->m_value_items.end() ) {
                return nullptr;
            }
            
            TU_IFLET( ::HIR::ValueItem, it->second->ent, Function, e,
                return &e;
            )
            else {
                BUG(sp, "Path " << path << " didn't point to a functon");
            }
            } break;
        case EntType::Constant: {
            auto it = mod->m_value_items.find( path.m_components.back() );
            if( it == mod->m_value_items.end() ) {
                return nullptr;
            }
            
            TU_IFLET( ::HIR::ValueItem, it->second->ent, Constant, e,
                return &e;
            )
            else {
                BUG(sp, "Path " << path << " didn't point to a functon");
            }
            } break;
        case EntType::Struct: {
            auto it = mod->m_mod_items.find( path.m_components.back() );
            if( it == mod->m_mod_items.end() ) {
                return nullptr;
            }
            
            TU_IFLET( ::HIR::TypeItem, it->second->ent, Struct, e,
                return &e;
            )
            else {
                BUG(sp, "Path " << path << " didn't point to a struct");
            }
            } break;
        }
        throw "";
    }
    const void* get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, EntType et)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            return get_ent_simplepath(sp, crate, e.m_path, et);
            ),
        (UfcsInherent,
            // Easy (ish)
            for( const auto& impl : crate.m_type_impls )
            {
                if( ! impl.matches_type(*e.type) ) {
                    continue ;
                }
                switch( et )
                {
                case EntType::Function: {
                    auto fit = impl.m_methods.find(e.item);
                    if( fit == impl.m_methods.end() )
                        continue ;
                    return &fit->second.data;
                    } break;
                case EntType::Struct:
                    break;
                case EntType::Constant:
                    break;
                }
            }
            return nullptr;
            ),
        (UfcsKnown,
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            ),
        (UfcsUnknown,
            // TODO - Since this isn't known, can it be searched properly?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            )
        )
        throw "";
    }
    const ::HIR::Function& get_function(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path)
    {
        auto* rv_p = reinterpret_cast<const ::HIR::Function*>( get_ent_fullpath(sp, crate, path, EntType::Function) );
        if( !rv_p ) {
            TODO(sp, "get_function(path = " << path << ")");
        }
        return *rv_p;
    }
    const ::HIR::Struct& get_struct(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path)
    {
        auto rv_p = reinterpret_cast<const ::HIR::Struct*>( get_ent_simplepath(sp, crate, path, EntType::Struct) );
        if( !rv_p ) {
            BUG(sp, "Could not find struct name in " << path);
        }
        return *rv_p;
    }
    const ::HIR::Constant& get_constant(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path)
    {
        auto rv_p = reinterpret_cast<const ::HIR::Constant*>( get_ent_fullpath(sp, crate, path, EntType::Constant) );
        if( !rv_p ) {
            BUG(sp, "Could not find constant in " << path);
        }
        return *rv_p;
    }
    
    ::HIR::Literal evaluate_constant(const ::HIR::Crate& crate, const ::HIR::ExprNode& expr)
    {
        struct Visitor:
            public ::HIR::ExprVisitor
        {
            const ::HIR::Crate& m_crate;
            ::std::vector< ::std::pair< ::std::string, ::HIR::Literal > >   m_values;
            
            ::HIR::Literal  m_rv;
            
            Visitor(const ::HIR::Crate& crate):
                m_crate(crate)
            {}
            
            void badnode(const ::HIR::ExprNode& node) const {
                ERROR(node.span(), E0000, "Node not allowed in constant expression");
            }
            
            void visit(::HIR::ExprNode_Block& node) override {
                TRACE_FUNCTION_F("_Block");
                for(const auto& e : node.m_nodes)
                    e->visit(*this);
            }
            void visit(::HIR::ExprNode_Return& node) override {
                TODO(node.span(), "ExprNode_Return");
            }
            void visit(::HIR::ExprNode_Let& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Loop& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_LoopControl& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Match& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_If& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_Assign& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_BinOp& node) override {
                TRACE_FUNCTION_F("_BinOp");
                node.m_left->visit(*this);
                auto left = mv$(m_rv);
                node.m_right->visit(*this);
                auto right = mv$(m_rv);
                
                if( left.tag() != right.tag() ) {
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Sides mismatched");
                }
                
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Comparisons");
                    break;
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:
                case ::HIR::ExprNode_BinOp::Op::BoolOr:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Logicals");
                    break;

                case ::HIR::ExprNode_BinOp::Op::Add:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le + re); ),
                    (Float,     m_rv = ::HIR::Literal(le + re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Sub:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le - re); ),
                    (Float,     m_rv = ::HIR::Literal(le - re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mul:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le * re); ),
                    (Float,     m_rv = ::HIR::Literal(le * re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Div:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le / re); ),
                    (Float,     m_rv = ::HIR::Literal(le / re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mod:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "modulo operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::And:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise and operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Or:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le | re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise or operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Xor:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le ^ re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise xor operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shr:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le >> re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift right operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shl:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le << re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift left operator on float in constant"); )
                    )
                    break;
                }
            }
            void visit(::HIR::ExprNode_UniOp& node) override {
                TRACE_FUNCTION_FR("_UniOp", m_rv);
                node.m_value->visit(*this);
                auto val = mv$(m_rv);
                
                switch(node.m_op)
                {
                case ::HIR::ExprNode_UniOp::Op::Ref:
                case ::HIR::ExprNode_UniOp::Op::RefMut:
                    TODO(node.span(), "&/&mut in constant");
                    break;
                case ::HIR::ExprNode_UniOp::Op::Invert:
                    TU_MATCH_DEF(::HIR::Literal, (val), (e),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal::make_Integer(~e); ),
                    (Float,     ERROR(node.span(), E0000, "not operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_UniOp::Op::Negate:
                    TU_MATCH_DEF(::HIR::Literal, (val), (e),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(-e); ),
                    (Float,     m_rv = ::HIR::Literal(-e); )
                    )
                    break;
                }
            }
            void visit(::HIR::ExprNode_Cast& node) override {
                TRACE_FUNCTION_F("_Cast");
                node.m_value->visit(*this);
                //auto val = mv$(m_rv);
                //DEBUG("ExprNode_Cast - val = " << val << " as " << node.m_type);
            }
            void visit(::HIR::ExprNode_Unsize& node) override {
                TRACE_FUNCTION_F("_Unsize");
                node.m_value->visit(*this);
                //auto val = mv$(m_rv);
                //DEBUG("ExprNode_Unsize - val = " << val << " as " << node.m_type);
            }
            void visit(::HIR::ExprNode_Index& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Deref& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_TupleVariant& node) override {
                TODO(node.span(), "ExprNode_TupleVariant");
            }
            void visit(::HIR::ExprNode_CallPath& node) override {
                TRACE_FUNCTION_FR("_CallPath - " << node.m_path, m_rv);
                auto& fcn = get_function(node.span(), m_crate, node.m_path);
                // TODO: Set m_const during parse
                //if( ! fcn.m_const ) {
                //    ERROR(node.span(), E0000, "Calling non-const function in const context - " << node.m_path);
                //}
                if( fcn.m_args.size() != node.m_args.size() ) {
                    ERROR(node.span(), E0000, "Incorrect argument count for " << node.m_path << " - expected " << fcn.m_args.size() << ", got " << node.m_args.size());
                }
                for(unsigned int i = 0; i < fcn.m_args.size(); i ++ )
                {
                    const auto& pattern = fcn.m_args[i].first;
                    node.m_args[i]->visit(*this);
                    auto arg_val = mv$(m_rv);
                    TU_IFLET(::HIR::Pattern::Data, pattern.m_data, Any, e,
                        m_values.push_back( ::std::make_pair(pattern.m_binding.m_name, mv$(arg_val)) );
                    )
                    else {
                        ERROR(node.span(), E0000, "Constant functions can't have destructuring pattern argments");
                    }
                }
                
                // Call by running the code directly
                {
                    TRACE_FUNCTION_F("Call const fn " << node.m_path);
                    const_cast<HIR::ExprNode&>(*fcn.m_code).visit( *this );
                    assert( ! m_rv.is_Invalid() );
                }
            
                for(unsigned int i = 0; i < fcn.m_args.size(); i ++ )
                {
                    m_values.pop_back();
                }
            }
            void visit(::HIR::ExprNode_CallValue& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_CallMethod& node) override {
                // TODO: const methods
                badnode(node);
            }
            void visit(::HIR::ExprNode_Field& node) override {
                badnode(node);
            }

            void visit(::HIR::ExprNode_Literal& node) override {
                TRACE_FUNCTION_FR("_Literal", m_rv);
                TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
                (Integer,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Float,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Boolean,
                    m_rv = ::HIR::Literal(static_cast<uint64_t>(e));
                    ),
                (String,
                    m_rv = ::HIR::Literal(e);
                    ),
                (ByteString,
                    m_rv = ::HIR::Literal::make_String({e.begin(), e.end()});
                    )
                )
            }
            void visit(::HIR::ExprNode_UnitVariant& node) override {
                TODO(node.span(), "Unit varant/struct constructors in constant context");
            }
            void visit(::HIR::ExprNode_PathValue& node) override {
                TRACE_FUNCTION_FR("_PathValue - " << node.m_path, m_rv);
                const auto& c = get_constant(node.span(), m_crate, node.m_path);
                if( c.m_value_res.is_Invalid() ) {
                    const_cast<HIR::ExprNode&>(*c.m_value).visit(*this);
                }
                else {
                    m_rv = clone_literal(c.m_value_res);
                }
            }
            void visit(::HIR::ExprNode_Variable& node) override {
                TRACE_FUNCTION_FR("_Variable - " << node.m_name, m_rv);
                for(auto it = m_values.rbegin(); it != m_values.rend(); ++it)
                {
                    if( it->first == node.m_name) {
                        TU_MATCH_DEF(::HIR::Literal, (it->second), (e),
                        (
                            m_rv = mv$(it->second);
                            ),
                        (Integer,
                            m_rv = ::HIR::Literal(e);
                            ),
                        (Float,
                            m_rv = ::HIR::Literal(e);
                            )
                        )
                        return;
                    }
                }
                ERROR(node.span(), E0000, "Couldn't find variable " << node.m_name);
            }
            
            void visit(::HIR::ExprNode_StructLiteral& node) override {
                TRACE_FUNCTION_FR("_StructLiteral - " << node.m_path, m_rv);
                const auto& str = get_struct(node.span(), m_crate, node.m_path.m_path);
                const auto& fields = str.m_data.as_Named();
                
                ::std::vector< ::HIR::Literal>  vals;
                if( node.m_base_value ) {
                    node.m_base_value->visit(*this);
                    auto base_val = mv$(m_rv);
                    if( !base_val.is_List() || base_val.as_List().size() != fields.size() ) {
                        BUG(node.span(), "Struct literal base value had an incorrect field count");
                    }
                    vals = mv$(base_val.as_List());
                }
                else {
                    vals.resize( fields.size() );
                }
                for( const auto& val_set : node.m_values ) {
                    unsigned int idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& v) { return v.first == val_set.first; } ) - fields.begin();
                    if( idx == fields.size() ) {
                        ERROR(node.span(), E0000, "Field name " << val_set.first << " isn't a member of " << node.m_path);
                    }
                    val_set.second->visit(*this);
                    vals[idx] = mv$(m_rv);
                }
                for( unsigned int i = 0; i < vals.size(); i ++ ) {
                    const auto& val = vals[i];
                    if( val.is_Invalid() ) {
                        ERROR(node.span(), E0000, "Field " << fields[i].first << " wasn't set");
                    }
                }

                m_rv = ::HIR::Literal::make_List(mv$(vals));
            }
            void visit(::HIR::ExprNode_Tuple& node) override {
                ::std::vector< ::HIR::Literal>  vals;
                for(const auto& vn : node.m_vals ) {
                    vn->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    vals.push_back( mv$(m_rv) );
                }
                m_rv = ::HIR::Literal::make_List(mv$(vals));
            }
            void visit(::HIR::ExprNode_ArrayList& node) override {
                ::std::vector< ::HIR::Literal>  vals;
                for(const auto& vn : node.m_vals ) {
                    vn->visit(*this);
                    assert( !m_rv.is_Invalid() );
                    vals.push_back( mv$(m_rv) );
                }
                m_rv = ::HIR::Literal::make_List(mv$(vals));
            }
            void visit(::HIR::ExprNode_ArraySized& node) override {
                TODO(node.span(), "ExprNode_ArraySized");
            }
            
            void visit(::HIR::ExprNode_Closure& node) override {
                badnode(node);
            }
        };
        
        Visitor v { crate };
        const_cast<::HIR::ExprNode&>(expr).visit(v);
        
        if( v.m_rv.is_Invalid() ) {
            BUG(Span(), "Expression did not yeild a literal");
        }
        
        return mv$(v.m_rv);
    }

    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                ::HIR::Visitor::visit_type(*e.inner);
                assert(e.size.get() != nullptr);
                auto val = evaluate_constant(m_crate, *e.size);
                if( !val.is_Integer() )
                    ERROR(e.size->span(), E0000, "Array size isn't an integer");
                e.size_val = val.as_Integer();
                DEBUG("Array " << ty << " - size = " << e.size_val);
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            visit_type(item.m_type);
            item.m_value_res = evaluate_constant(m_crate, *item.m_value);
            DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            visit_type(item.m_type);
            item.m_value_res = evaluate_constant(m_crate, *item.m_value);
            DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
        }
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct Visitor:
                public ::HIR::ExprVisitorDef
            {
                Expander& m_exp;
                
                Visitor(Expander& exp):
                    m_exp(exp)
                {}
                
                void visit(::HIR::ExprNode_Let& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_type);
                }
                void visit(::HIR::ExprNode_Cast& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_res_type);
                }
                // TODO: This shouldn't exist yet?
                void visit(::HIR::ExprNode_Unsize& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_res_type);
                }
                void visit(::HIR::ExprNode_Closure& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_type(node.m_return);
                    for(auto& a : node.m_args)
                        m_exp.visit_type(a.second);
                }

                void visit(::HIR::ExprNode_ArraySized& node) override {
                    auto val = evaluate_constant(m_exp.m_crate, *node.m_size);
                    if( !val.is_Integer() )
                        ERROR(node.span(), E0000, "Array size isn't an integer");
                    node.m_size_val = val.as_Integer();
                    DEBUG("Array literal [?; " << node.m_size_val << "]");
                }
                
                void visit(::HIR::ExprNode_CallPath& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                }
                void visit(::HIR::ExprNode_CallMethod& node) override {
                    ::HIR::ExprVisitorDef::visit(node);
                    m_exp.visit_path_params(node.m_params);
                }
            };
            
            if( expr.get() != nullptr )
            {
                Visitor v { *this };
                (*expr).visit(v);
            }
        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
