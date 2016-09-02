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
    typedef ::std::vector< ::std::pair< ::std::string, ::HIR::Static> > t_new_values;
    
    struct NewvalState {
        t_new_values&   newval_output;
        const ::HIR::ItemPath&  mod_path;
        ::std::string   name_prefix;
        
        NewvalState(t_new_values& newval_output, const ::HIR::ItemPath& mod_path, ::std::string prefix):
            newval_output(newval_output),
            mod_path(mod_path),
            name_prefix(prefix)
        {
        }
    };
    
    ::HIR::Literal evaluate_constant(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprPtr& expr, ::std::vector< ::HIR::Literal> args={});
    
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
        (BorrowOf,
            return ::HIR::Literal(e);
            ),
        (String,
            return ::HIR::Literal(e);
            )
        )
        throw "";
    }
    
    TAGGED_UNION(EntPtr, Function,
        (NotFound, struct{}),
        (Function, const ::HIR::Function*),
        (Constant, const ::HIR::Constant*),
        (Struct, const ::HIR::Struct*)
        );
    enum class EntNS {
        Type,
        Value
    };
    EntPtr get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, EntNS ns)
    {
        const ::HIR::Module* mod;
        if( path.m_crate_name != "" ) {
            ASSERT_BUG(sp, crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
            mod = &crate.m_ext_crates.at(path.m_crate_name)->m_root_module;
        }
        else {
            mod = &crate.m_root_module;
        }
        
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
        
        switch( ns )
        {
        case EntNS::Value: {
            auto it = mod->m_value_items.find( path.m_components.back() );
            if( it == mod->m_value_items.end() ) {
                return EntPtr {};
            }
            
            TU_MATCH( ::HIR::ValueItem, (it->second->ent), (e),
            (Import,
                ),
            (StructConstant,
                ),
            (StructConstructor,
                ),
            (Function,
                return EntPtr { &e };
                ),
            (Constant,
                return EntPtr { &e };
                ),
            (Static,
                )
            )
            BUG(sp, "Path " << path << " pointed to a invalid item - " << it->second->ent.tag_str());
            } break;
        case EntNS::Type: {
            auto it = mod->m_mod_items.find( path.m_components.back() );
            if( it == mod->m_mod_items.end() ) {
                return EntPtr {};
            }
            
            TU_MATCH( ::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                ),
            (Module,
                ),
            (Trait,
                ),
            (Struct,
                return &e;
                ),
            (Enum,
                ),
            (TypeAlias,
                )
            )
            BUG(sp, "Path " << path << " pointed to an invalid item - " << it->second->ent.tag_str());
            } break;
        }
        throw "";
    }
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, EntNS ns)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            return get_ent_simplepath(sp, crate, e.m_path, ns);
            ),
        (UfcsInherent,
            // Easy (ish)
            EntPtr rv {};
            crate.find_type_impls(*e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                switch( ns )
                {
                case EntNS::Value: {
                    auto fit = impl.m_methods.find(e.item);
                    if( fit != impl.m_methods.end() )
                    {
                        rv = EntPtr { &fit->second.data };
                        return true;
                    }
                    } break;
                case EntNS::Type:
                    break;
                }
                return false;
                });
            return rv;
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
        auto rv = get_ent_fullpath(sp, crate, path, EntNS::Value);
        TU_IFLET( EntPtr, rv, Function, e,
            return *e;
        )
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }
    const ::HIR::Struct& get_struct(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path)
    {
        auto rv = get_ent_fullpath(sp, crate, path, EntNS::Type);
        TU_IFLET( EntPtr, rv, Struct, e,
            return *e;
        )
        else {
            TODO(sp, "Could not find struct for " << path << " - " << rv.tag_str());
        }
    }
    
    ::HIR::Literal evaluate_constant_hir(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprNode& expr, ::std::vector< ::HIR::Literal> args)
    {
        struct Visitor:
            public ::HIR::ExprVisitor
        {
            const ::HIR::Crate& m_crate;
            NewvalState m_newval_state;
            
            ::std::vector< ::HIR::Literal>   m_values;
            
            ::HIR::TypeRef  m_exp_type;
            ::HIR::Literal  m_rv;
            
            Visitor(const ::HIR::Crate& crate, NewvalState newval_state):
                m_crate(crate),
                m_newval_state( mv$(newval_state) )
            {}
            
            void badnode(const ::HIR::ExprNode& node) const {
                ERROR(node.span(), E0000, "Node not allowed in constant expression");
            }
            
            void visit(::HIR::ExprNode_Block& node) override {
                TRACE_FUNCTION_F("_Block");
                
                for(const auto& e : node.m_nodes)
                {
                    e->visit(*this);
                }
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
            void visit(::HIR::ExprNode_Borrow& node) override {
                //auto ty = m_exp_type;
                
                node.m_value->visit(*this);
                auto val = mv$(m_rv);
                
                if( node.m_type != ::HIR::BorrowType::Shared ) {
                    ERROR(node.span(), E0000, "Only shared borrows are allowed in constants");
                }
                
                // Create new static containing borrowed data
                auto name = FMT(m_newval_state.name_prefix << &node);
                m_newval_state.newval_output.push_back(::std::make_pair( name, ::HIR::Static {
                    false,
                    ::HIR::TypeRef(),
                    ::HIR::ExprNodeP(),
                    mv$(val)
                    } ));
                m_rv = ::HIR::Literal::make_BorrowOf( (m_newval_state.mod_path + name).get_simple_path() );
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
                ::std::vector< ::HIR::Literal>  args;
                args.reserve( fcn.m_args.size() );
                for(unsigned int i = 0; i < fcn.m_args.size(); i ++ )
                {
                    const auto& pattern = fcn.m_args[i].first;
                    node.m_args[i]->visit(*this);
                    args.push_back( mv$(args) );
                    TU_IFLET(::HIR::Pattern::Data, pattern.m_data, Any, e,
                        // Good
                    )
                    else {
                        ERROR(node.span(), E0000, "Constant functions can't have destructuring pattern argments");
                    }
                }
                
                // Call by running the code directly
                {
                    TRACE_FUNCTION_F("Call const fn " << node.m_path);
                    m_rv = evaluate_constant(node.span(), m_crate, m_newval_state,  fcn.m_code, mv$(args));
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
                auto ep = get_ent_fullpath(node.span(), m_crate, node.m_path, EntNS::Value);
                TU_MATCH_DEF( EntPtr, (ep), (e),
                (
                    BUG(node.span(), "Path value with unsupported value type - " << ep.tag_str());
                    ),
                (Function,
                    // TODO: Should be a more complex path
                    m_rv = ::HIR::Literal(node.m_path.m_data.as_Generic().m_path);
                    ),
                (Constant,
                    const auto& c = *e;
                    if( c.m_value_res.is_Invalid() ) {
                        const_cast<HIR::ExprNode&>(*c.m_value).visit(*this);
                    }
                    else {
                        m_rv = clone_literal(c.m_value_res);
                    }
                    )
                )
            }
            void visit(::HIR::ExprNode_Variable& node) override {
                TRACE_FUNCTION_FR("_Variable - " << node.m_name, m_rv);
                // TODO: use the binding?
                if( node.m_slot >= m_values.size() ) {
                    ERROR(node.span(), E0000, "Couldn't find variable #" << node.m_slot << " " << node.m_name);
                }
                auto& v = m_values.at( node.m_slot );
                TU_MATCH_DEF(::HIR::Literal, (v), (e),
                (
                    m_rv = mv$(v);
                    ),
                (Integer,
                    m_rv = ::HIR::Literal(e);
                    ),
                (Float,
                    m_rv = ::HIR::Literal(e);
                    )
                )
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
        
        Visitor v { crate, newval_state };
        for(auto& arg : args)
            v.m_values.push_back( mv$(arg) );
        const_cast<::HIR::ExprNode&>(expr).visit(v);
        
        if( v.m_rv.is_Invalid() ) {
            BUG(sp, "Expression did not yeild a literal");
        }
        
        return mv$(v.m_rv);
    }
    
    ::HIR::Literal evaluate_constant(const Span& sp, const ::HIR::Crate& crate, NewvalState newval_state, const ::HIR::ExprPtr& expr, ::std::vector< ::HIR::Literal> args)
    {
        if( expr ) {
            return evaluate_constant_hir(sp, crate, mv$(newval_state), *expr, mv$(args));
        }
        else if( expr.m_mir ) {
            TODO(sp, "Execute MIR");
        }
        else {
            BUG(sp, "Attempting to evaluate constant expression with code");
        }
    }

    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::ItemPath*  m_mod_path;
        t_new_values    m_new_values;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_mp = m_mod_path;
            m_mod_path = &p;
            auto saved = mv$( m_new_values );
            
            ::HIR::Visitor::visit_module(p, mod);
            
            //auto items = mv$( m_new_values );
            //for( auto item : items )
            //{
            //    
            //}
            m_new_values = mv$(saved);
            m_mod_path = saved_mp;
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                ::HIR::Visitor::visit_type(*e.inner);
                assert(e.size.get() != nullptr);
                auto val = evaluate_constant(e.size->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT("ty_" << &ty << "$") }, e.size);
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
            item.m_value_res = evaluate_constant(item.m_value->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") }, item.m_value, {});
            DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            visit_type(item.m_type);
            item.m_value_res = evaluate_constant(item.m_value->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") }, item.m_value, {});
            DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    e.val = evaluate_constant(e.expr->span(), m_crate, NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$" << var.first << "$") }, e.expr, {});
                    DEBUG("enum variant: " << p << "::" << var.first << " = " << e.val);
                )
            }
            ::HIR::Visitor::visit_enum(p, item);
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
                    assert( node.m_size );
                    auto val = evaluate_constant_hir(node.span(), m_exp.m_crate, NewvalState { m_exp.m_new_values, *m_exp.m_mod_path, FMT("array_" << &node << "$") }, *node.m_size, {});
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
