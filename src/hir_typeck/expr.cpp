/*
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

namespace {
    
    struct IVar
    {
        unsigned int alias;
        ::std::unique_ptr< ::HIR::TypeRef> type;
        
        IVar():
            alias(~0u),
            type(new ::HIR::TypeRef())
        {}
        bool is_alias() const { return alias != ~0u; }
    };
    
    class TypecheckContext
    {
        ::std::vector< IVar >   m_ivars;
        bool    m_has_changed;
    public:
        TypecheckContext(const ::HIR::TypeRef& result_type):
            m_has_changed(false)
        {
        }
        
        bool take_changed() {
            bool rv = m_has_changed;
            m_has_changed = false;
            return rv;
        }
        void mark_change() {
            m_has_changed = true;
        }
        
        /// Adds a local variable binding (type is mutable so it can be inferred if required)
        void add_local(unsigned int index, ::HIR::TypeRef type)
        {
        }
        /// Add (and bind) all '_' types in `type`
        void add_ivars(::HIR::TypeRef& type)
        {
            TU_MATCH(::HIR::TypeRef::Data, (type.m_data), (e),
            (Infer,
                e.index = this->new_ivar();
                ),
            (Diverge,
                ),
            (Primitive,
                ),
            (Path,
                // Iterate all arguments
                ),
            (Generic,
                ),
            (TraitObject,
                // Iterate all paths
                ),
            (Array,
                add_ivars(*e.inner);
                ),
            (Slice,
                add_ivars(*e.inner);
                ),
            (Tuple,
                for(auto& ty : e)
                    add_ivars(ty);
                ),
            (Borrow,
                add_ivars(*e.inner);
                ),
            (Pointer,
                add_ivars(*e.inner);
                ),
            (Function,
                // No ivars allowed
                // TODO: Check?
                )
            )
        }
        
        
        void add_pattern_binding(const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
        {
            assert( pb.is_valid() );
            switch( pb.m_type )
            {
            case ::HIR::PatternBinding::Type::Move:
                this->add_local( pb.m_slot, mv$(type) );
                break;
            case ::HIR::PatternBinding::Type::Ref:
                this->add_local( pb.m_slot,  ::HIR::TypeRef::Data::make_Borrow( {::HIR::BorrowType::Shared, box$(mv$(type))} ) );
                break;
            case ::HIR::PatternBinding::Type::MutRef:
                this->add_local( pb.m_slot,  ::HIR::TypeRef::Data::make_Borrow( {::HIR::BorrowType::Unique, box$(mv$(type))} ) );
                break;
            }
        }
        
        void add_binding(const ::HIR::Pattern& pat, ::HIR::TypeRef type)
        {
            if( pat.m_binding.is_valid() ) {
                this->add_pattern_binding(pat.m_binding, mv$(type));
                // TODO: Can there be bindings within a bound pattern?
                return ;
            }
            
            // 
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                // Just leave it, the pattern says nothing
                ),
            (Value,
                TODO(Span(), "Value pattern");
                ),
            (Range,
                TODO(Span(), "Range pattern");
                ),
            (Box,
                // Type must be box-able
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer,
                    // Apply rule based on recursing the pattern?
                    this->add_binding(*e.sub, this->new_ivar_tr());
                    )
                )
                ),
            (Ref,
                // Type must be a &-ptr
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Borrow,
                    if( te.type != e.type ) {
                        // TODO: Type mismatch
                    }
                    this->add_binding( *e.sub, mv$(*te.inner) );
                    ),
                (Infer,
                    // Apply rule based on recursing the pattern?
                    this->add_binding( *e.sub, this->new_ivar_tr());
                    )
                )
                ),
            (Tuple,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Tuple,
                    if( te.size() != e.sub_patterns.size() ) {
                        // TODO: Type mismatch
                    }
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->add_binding( e.sub_patterns[i], mv$(te[i]) );
                    ),
                (Infer,
                    // Apply rule based on recursing the pattern?
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->add_binding( e.sub_patterns[i], this->new_ivar_tr() );
                    )
                )
                ),
            (Slice,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Slice,
                    for(const auto& sp : e.sub_patterns)
                        this->add_binding( sp, te.inner->clone() );
                    ),
                (Infer,
                    // Apply rule based on recursing the pattern?
                    auto ty = this->new_ivar_tr();
                    for(const auto& sp : e.sub_patterns)
                        this->add_binding( sp, ty.clone() );
                    )
                )
                ),
            (SplitSlice,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Slice,
                    for(const auto& sp : e.leading)
                        this->add_binding( sp, te.inner->clone() );
                    for(const auto& sp : e.trailing)
                        this->add_binding( sp, te.inner->clone() );
                    if( e.extra_bind.is_valid() ) {
                        this->add_local( e.extra_bind.m_slot, ::HIR::TypeRef(::HIR::TypeRef::Data::make_Slice({ box$(mv$(*te.inner)) })) );
                    }
                    ),
                (Infer,
                    // Apply rule based on recursing the pattern?
                    auto ty = this->new_ivar_tr();
                    for(const auto& sp : e.leading)
                        this->add_binding( sp, ty.clone() );
                    for(const auto& sp : e.trailing)
                        this->add_binding( sp, ty.clone() );
                    if( e.extra_bind.is_valid() ) {
                        this->add_local( e.extra_bind.m_slot, ::HIR::TypeRef(::HIR::TypeRef::Data::make_Slice({ box$(mv$(ty)) })) );
                    }
                    )
                )
                ),
            
            // - Enums/Structs
            (StructTuple,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer,
                    TODO(Span(), "StructTuple - bind + infer");
                    ),
                (Path,
                    TODO(Span(), "StructTuple - bind");
                    )
                )
                ),
            (Struct,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer,
                    TODO(Span(), "Struct - bind + infer");
                    ),
                (Path,
                    TODO(Span(), "Struct - bind");
                    )
                )
                ),
            (EnumTuple,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer,
                    TODO(Span(), "EnumTuple - bind + infer");
                    ),
                (Path,
                    TODO(Span(), "EnumTuple - bind");
                    )
                )
                ),
            (EnumStruct,
                TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer,
                    TODO(Span(), "EnumStruct - bind + infer");
                    ),
                (Path,
                    TODO(Span(), "EnumStruct - bind");
                    )
                )
                )
            )
        }
        
        /// Run inferrence using a pattern
        void apply_pattern(const ::HIR::Pattern& pat, ::HIR::TypeRef& type)
        {
            static Span _sp;
            const Span& sp = _sp;
            // TODO: Should this do an equality on the binding?

            auto& ty = this->get_type(type);
            
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                // Just leave it, the pattern says nothing about the type
                ),
            (Value,
                TODO(sp, "Value pattern");
                ),
            (Range,
                TODO(sp, "Range pattern");
                ),
            // - Pointer destructuring
            (Box,
                // Type must be box-able
                TODO(sp, "Box patterns");
                ),
            (Ref,
                if( ty.m_data.is_Infer() ) {
                    ty.m_data = ::HIR::TypeRef::Data::make_Borrow( {e.type, box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                // Type must be a &-ptr
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Borrow,
                    if( te.type != e.type ) {
                        // TODO: Type mismatch
                    }
                    this->apply_pattern( *e.sub, *te.inner );
                    )
                )
                ),
            (Tuple,
                if( ty.m_data.is_Infer() ) {
                    ::std::vector< ::HIR::TypeRef>  sub_types;
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        sub_types.push_back( this->new_ivar_tr() );
                    ty.m_data = ::HIR::TypeRef::Data::make_Tuple( mv$(sub_types) );
                    
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Tuple,
                    if( te.size() != e.sub_patterns.size() ) {
                        // TODO: Type mismatch
                    }
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        this->apply_pattern( e.sub_patterns[i], te[i] );
                    )
                )
                ),
            // --- Slices
            (Slice,
                if( ty.m_data.is_Infer() ) {
                    ty.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Slice,
                    for(const auto& sp : e.sub_patterns )
                        this->apply_pattern( sp, *te.inner );
                    )
                )
                ),
            (SplitSlice,
                if( ty.m_data.is_Infer() ) {
                    ty.m_data = ::HIR::TypeRef::Data::make_Slice( {box$(this->new_ivar_tr())} );
                    this->mark_change();
                }
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Slice,
                    for(const auto& sp : e.leading)
                        this->apply_pattern( sp, *te.inner );
                    for(const auto& sp : e.trailing)
                        this->apply_pattern( sp, *te.inner );
                    // TODO: extra_bind? (see comment at start of function)
                    )
                )
                ),
            
            // - Enums/Structs
            (StructTuple,
                if( ty.m_data.is_Infer() ) {
                    TODO(sp, "StructTuple - infer");
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    TODO(sp, "StructTuple - destructure");
                    )
                )
                ),
            (Struct,
                if( ty.m_data.is_Infer() ) {
                    //TODO: Does this lead to issues with generic parameters?
                    ty.m_data = ::HIR::TypeRef::Data::make_Path( e.path.clone() );
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    if( ! te.m_data.is_Generic() ) {
                        ERROR(sp, E0000, "UFCS path being destructured - " << te);
                    }
                    const auto& gp = te.m_data.as_Generic();
                    TODO(sp, "Struct - destructure - " << te);
                    )
                )
                ),
            (EnumTuple,
                if( ty.m_data.is_Infer() ) {
                    TODO(sp, "EnumTuple - infer");
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    TODO(sp, "EnumTuple - destructure");
                    )
                )
                ),
            (EnumStruct,
                if( ty.m_data.is_Infer() ) {
                    TODO(sp, "EnumStruct - infer");
                    this->mark_change();
                }
                
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    // TODO: Type mismatch
                    ),
                (Infer, throw "";),
                (Path,
                    TODO(sp, "EnumStruct - destructure");
                    )
                )
                )
            )
        }
        // Adds a rule that two types must be equal
        void apply_equality(const ::HIR::TypeRef& left, const ::HIR::TypeRef& right)
        {
        }
    public:
        unsigned int new_ivar()
        {
            m_ivars.push_back( IVar() );
            return m_ivars.size() - 1;
        }
        ::HIR::TypeRef new_ivar_tr() {
            ::HIR::TypeRef rv;
            rv.m_data.as_Infer().index = this->new_ivar();
            return rv;
        }
        
        ::HIR::TypeRef& get_type(::HIR::TypeRef& type)
        {
            TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
                auto index = e.index;
                while( m_ivars.at(index).is_alias() ) {
                    index = m_ivars.at(index).alias;
                }
                return *m_ivars.at(index).type;
            )
            else {
                return type;
            }
        }
    };
    
    class ExprVisitor_Enum:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Enum(TypecheckContext& context):
            context(context)
        {
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            this->context.add_ivars(node.m_type);
            
            this->context.add_binding(node.m_pattern, node.m_type.clone());
            //if( node.m_value ) {
            //    this->context.add_rule_equality(node.m_type, node.m_value->m_res_type);
            //}
        }
        
    };
    
    class ExprVisitor_Run:
        public ::HIR::ExprVisitorDef
    {
        TypecheckContext& context;
    public:
        ExprVisitor_Run(TypecheckContext& context):
            context(context)
        {
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            this->context.apply_pattern(node.m_pattern, node.m_type);
            //if( node.m_value ) {
            //    this->context.add_rule_equality(node.m_type, node.m_value->m_res_type);
            //}
        }
        
    };
}

void Typecheck_Code(TypecheckContext context, ::HIR::ExprNode& root_node)
{
    TRACE_FUNCTION;
    
    // 1. Enumerate inferrence variables and assign indexes to them
    {
        ExprVisitor_Enum    visitor { context };
        root_node.visit( visitor );
    }
    // 2. Iterate through nodes applying rules until nothing changes
    {
        ExprVisitor_Run visitor { context };
        do {
            root_node.visit( visitor );
        } while( context.take_changed() );
    }
}



namespace {
    class OuterVisitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        
        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;
    public:
        OuterVisitor(::HIR::Crate& crate):
            crate(crate),
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
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            TODO(Span(), "visit_expr");
        }

        void visit_trait(::HIR::Trait& item) override
        {
            //::HIR::TypeRef tr { "Self", 0 };
            auto _ = this->set_impl_generics(item.m_params);
            //m_self_types.push_back(&tr);
            ::HIR::Visitor::visit_trait(item);
            //m_self_types.pop_back();
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_type_impl(impl);
            // Check that the type is valid
            
            //m_self_types.pop_back();
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            //m_self_types.pop_back();
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->set_impl_generics(impl.m_params);
            //m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            //m_self_types.pop_back();
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                TypecheckContext    typeck_context { ::HIR::TypeRef(::HIR::CoreType::Usize) };
                Typecheck_Code( mv$(typeck_context), *e.size );
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::Function& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_code )
            {
                TypecheckContext typeck_context { item.m_return };
                for( const auto& arg : item.m_args )
                    typeck_context.add_binding( arg.first, arg.second.clone() );
                Typecheck_Code( mv$(typeck_context), *item.m_code );
            }
        }
        void visit_static(::HIR::Static& item) override {
            //auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_value )
            {
                TypecheckContext typeck_context { item.m_type };
            }
        }
        void visit_constant(::HIR::Constant& item) override {
            auto _ = this->set_item_generics(item.m_params);
            if( &*item.m_value )
            {
                TypecheckContext typeck_context { item.m_type };
            }
        }
    };
}

void Typecheck_Expressions(::HIR::Crate& crate)
{
    OuterVisitor    visitor { crate };
    visitor.visit_crate( crate );
}
