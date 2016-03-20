/**
 */
#include "ast_iterate.hpp"
#include "../ast/ast.hpp"

void CASTIterator::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
}
void CASTIterator::handle_type(TypeRef& type)
{
    TRACE_FUNCTION_F("type = " << type);
    
    TU_MATCH(TypeData, (type.m_data), (ent),
    (None),
    (Any),
    (Unit),
    (Macro),
    (Primitive),
    (Path,
        handle_path(ent.path, MODE_TYPE);
        ),
    (Tuple,
        for(auto& subtype : ent.inner_types)
            handle_type(subtype);
        ),
    (Borrow,
        handle_type(*ent.inner);
        ),
    (Pointer,
        handle_type(*ent.inner);
        ),
    (Array,
        handle_type(*ent.inner);
        ),
    (Function,
        handle_type(*ent.info.m_rettype);
        for(auto& arg : ent.info.m_arg_types)
            handle_type(arg);
        ),
    (Generic),
    (TraitObject,
        for(auto& trait : ent.traits)
            handle_path(trait, MODE_TYPE);
        )
    )
}
void CASTIterator::handle_expr(AST::ExprNode& node)
{
}
void CASTIterator::handle_params(AST::GenericParams& params)
{
    DEBUG("params");
    for( auto& param : params.ty_params() )
    {
        handle_type(param.get_default());
        local_type( param.name(), TypeRef(TypeRef::TagArg(), param.name(), params) );
    }
    DEBUG("Bounds");
    for( auto& bound : params.bounds() )
    {
        TU_MATCH( AST::GenericBound, (bound), (ent),
        (Lifetime,
            DEBUG("namecheck lifetime bounds?");
            ),
        (TypeLifetime,
            handle_type(ent.type);
            DEBUG("namecheck lifetime bounds?");
            ),
        (IsTrait,
            handle_type(ent.type);
            // TODO: Define HRLs
            push_self(ent.type);
            handle_path(ent.trait, CASTIterator::MODE_TYPE);
            pop_self();
            ),
        (MaybeTrait,
            handle_type(ent.type);
            push_self(ent.type);
            handle_path(ent.trait, CASTIterator::MODE_TYPE);
            pop_self();
            // TODO: Process trait, ensuring that it's a valid lang item
            ),
        (NotTrait,
            handle_type(ent.type);
            push_self(ent.type);
            handle_path(ent.trait, CASTIterator::MODE_TYPE);
            pop_self();
            ),
        (Equality,
            handle_type(ent.type);
            handle_type(ent.replacement);
            )
        )
    }
}


void CASTIterator::start_scope()
{
    INDENT();
}
void CASTIterator::local_type(::std::string name, TypeRef type)
{
    DEBUG("type " << name << " = " << type);
}
void CASTIterator::local_variable(bool is_mut, ::std::string name, const TypeRef& type)
{
    DEBUG( (is_mut ? "mut " : "") << name << " : " << type );
}
void CASTIterator::local_use(::std::string name, AST::Path path)
{
    DEBUG( name << " = " << path );
}
void CASTIterator::end_scope()
{
    UNINDENT();
}

void CASTIterator::handle_pattern(AST::Pattern& pat, const TypeRef& type_hint)
{
    //DEBUG("pat = " << pat);
    // Resolve names
    TU_MATCH(AST::Pattern::Data, (pat.data()), (v),
    (Any,
        // Wildcard, nothing to do
        ),
    (Macro,
        // Macro, nothing really (should be impossible?)
        ),
    (Box, {
        auto& v = pat.data().as_Box();
        if( type_hint.is_wildcard() )
            handle_pattern(*v.sub, (const TypeRef&)TypeRef());
        else {
            throw ::std::runtime_error("TODO: Handle box patterns in CASTIterator::handle_pattern");
            handle_pattern(*v.sub, type_hint.inner_type());
        }
        }),
    (Ref, {
        auto& v = pat.data().as_Ref();
        if( type_hint.is_wildcard() )
            handle_pattern(*v.sub, (const TypeRef&)TypeRef());
        else if( !type_hint.is_reference() )
            throw ::std::runtime_error( FMT("Ref pattern on non-ref value: " << type_hint) );
        else
            handle_pattern(*v.sub, type_hint.inner_type());
        }),
    (MaybeBind,
        throw ::std::runtime_error("Calling CASTIterator::handle_pattern on MAYBE_BIND, not valid");
        ),
    (Value, {
        auto& v = pat.data().as_Value();
        handle_expr( *v.start );
        if( v.end.get() )
            handle_expr( *v.end );
        }),
    (Tuple, {
        auto& v = pat.data().as_Tuple();
        // Tuple is handled by subpattern code
        if( type_hint.is_wildcard() )
        {
            for( auto& sp : v.sub_patterns )
                handle_pattern(sp, (const TypeRef&)TypeRef());
        }
        else if( !type_hint.is_tuple() )
        {
            throw ::std::runtime_error("Tuple pattern on non-tuple value");
        }
        else
        {
            const auto& inner_types = type_hint.m_data.as_Tuple().inner_types;
            if( inner_types.size() != v.sub_patterns.size() )
            {
                throw ::std::runtime_error("Tuple pattern count doesn't match");
            }
            for( unsigned int i = 0; i < v.sub_patterns.size(); i ++ )
            {
                handle_pattern(v.sub_patterns[i], inner_types[i]);
            }
        }
        }),
    (Struct, {
        auto& v = pat.data().as_Struct();
        handle_path( v.path, CASTIterator::MODE_TYPE );
        if( type_hint.is_wildcard() )
        {
            for( auto& sp : v.sub_patterns )
                handle_pattern(sp.second, (const TypeRef&)TypeRef());
        }
        else if( !type_hint.is_path() )
        {
            throw ::std::runtime_error("Tuple struct pattern on non-tuple value");
        }
        else
        {
            auto& path = type_hint.m_data.as_Path().path;
            if( path.is_bound() ) {
                throw ::std::runtime_error("TODO: Typecheck/iterate struct pattern (with known type)");
            }
            else {
                for( auto& sp : v.sub_patterns )
                    handle_pattern(sp.second, (const TypeRef&)TypeRef());
            }
        }
        }),
    (StructTuple, {
        auto& v = pat.data().as_StructTuple();
        // Resolve the path!
        handle_path( v.path, CASTIterator::MODE_TYPE );
        // Handle sub-patterns
        if( type_hint.is_wildcard() )
        {
            for( auto& sp : v.sub_patterns )
                handle_pattern(sp, (const TypeRef&)TypeRef());
        }
        else if( !type_hint.is_path() )
        {
            throw ::std::runtime_error("Tuple struct pattern on non-tuple value");
        }
        else
        {
            auto& hint_path = type_hint.path();
            auto& pat_path = v.path;
            const auto& hint_binding = hint_path.binding();
            const auto& pat_binding = pat_path.binding();
            DEBUG("Pat: " << pat_path << ", Type: " << type_hint.path());
            TU_MATCH_DEF( AST::PathBinding, (hint_binding), (info),
            (
                throw ::std::runtime_error(FMT("Bad type in tuple struct pattern : " << type_hint.path()))
                ),
            (Unbound,
                throw ::std::runtime_error("Unbound path in pattern");
                ),
            (Enum,
                // The pattern's path must refer to a variant of the hint path
                // - Actual type params are checked by the 'handle_pattern_enum' code
                if( !pat_binding.is_EnumVar() )
                    throw ::std::runtime_error(FMT("Paths in pattern are invalid"));
                if( pat_binding.as_EnumVar().enum_ != info.enum_ )
                    throw ::std::runtime_error(FMT("Paths in pattern are invalid"));
                const auto& enm = *pat_binding.as_EnumVar().enum_;
                auto idx = pat_binding.as_EnumVar().idx;
                auto& var = enm.variants().at(idx);
                handle_pattern_enum(pat_path[-2].args(), hint_path[-1].args(), enm.params(), var, v.sub_patterns);
                )
            )
        }
        }),
    (Slice,
        TypeRef null_type;
        const auto* inner_type = &null_type;
        if( !type_hint.is_wildcard() )
        {
            TU_MATCH_DEF( TypeData, (type_hint.m_data), (v),
            (
                ERROR(Span(), E0000, "Slice pattern on non-slice/array");
                ),
            (Array,
                inner_type = v.inner.get();
                ),
            (Borrow,
                if( v.inner->is_wildcard() ) {
                }
                else if( v.inner->m_data.is_Array() ) {
                    inner_type = v.inner->m_data.as_Array().inner.get();
                }
                else {
                    // TODO: Deref more?
                    ERROR(Span(), E0000, "Slice pattern on non-slice/array");
                }
                )
            )
        }
        for( auto& sp : v.leading )
            handle_pattern(sp, *inner_type);
        for( auto& sp : v.trailing )
            handle_pattern(sp, *inner_type);
        )
    )
    // Extract bindings and add to namespace
    if( pat.binding().size() > 0 )
    {
        // TODO: Mutable bindings
        if(pat.binding() != "_")
        {
            local_variable( false, pat.binding(), type_hint );
        }
    }
}
void CASTIterator::handle_pattern_enum(
        ::std::vector<TypeRef>& pat_args, const ::std::vector<TypeRef>& hint_args,
        const AST::GenericParams& enum_params, const AST::EnumVariant& var,
        ::std::vector<AST::Pattern>& sub_patterns
        )
{
    // This implementation doesn't attempt to do anything with types, just propagates _
    for( auto& sp : sub_patterns )
        handle_pattern(sp, (const TypeRef&)TypeRef());
}

void CASTIterator::handle_module(AST::Path path, AST::Module& mod)
{
    INDENT();
    start_scope();
    
    for( auto& item : mod.items() )
    {
        TU_MATCH(::AST::Item, (item.data), (e),
        (None,
            // Explicitly ignored (represents a deleted item)
            ),
        (Crate,
            // Nothing to be done
            ),
        (Struct,
            DEBUG("Handling struct " << item.name);
            handle_struct(path + item.name, e);
            ),
        (Enum,
            DEBUG("Handling enum " << item.name);
            handle_enum(path + item.name, e);
            ),
        (Trait,
            DEBUG("Handling trait " << item.name);
            handle_trait(path + item.name, e);
            ),
        (Type,
            DEBUG("Handling alias " << item.name);
            handle_alias(path + item.name, e);
            ),
        (Static,
            DEBUG("handling static " << item.name);
            handle_type(e.type());
            if( e.value().is_valid() )
            {
                handle_expr(e.value().node());
            }
            ),
        (Function,
            DEBUG("Handling function '" << item.name << "'");
            handle_function(path + item.name, e);
            ),
        (Module,
            // Skip, done after all items
            )
        )
    }
    
    for( auto& impl : mod.impls() )
    {
        DEBUG("Handling 'impl' " << impl);
        handle_impl(path, impl);
    }
    
    // End scope before handling sub-modules
    end_scope(); 
 
    for( auto& item : mod.items() )
    {
        if(!item.data.is_Module())    continue;
        auto& submod = item.data.as_Module();
        DEBUG("Handling submod '" << item.name << "'");
        handle_module(path + item.name, submod);
    }
    unsigned int anon_mod_idx = 0;
    for( auto& anonmod : mod.anon_mods() )
    {
        auto& submod = *anonmod;
        DEBUG("Handling submod #" << anon_mod_idx);
        handle_module(path + FMT("#" << anon_mod_idx), submod);
        anon_mod_idx += 1;
    }
    UNINDENT();
}
void CASTIterator::handle_function(AST::Path path, AST::Function& fcn)
{
    TRACE_FUNCTION_F("path = " << path << ", fcn.params() = " << fcn.params());
    start_scope();
    
    DEBUG("params");
    handle_params(fcn.params());
    
    DEBUG("ret type");
    handle_type(fcn.rettype());
    
    //switch( fcn.fcn_class() )
    //{
    //case AST::Function::CLASS_UNBOUND:
    //    break;
    //case AST::Function::CLASS_REFMETHOD:
    //    local_variable(false, "self", TypeRef(TypeRef::TagReference(), false, TypeRef(TypeRef::TagArg(), "Self")));
    //    break;
    //case AST::Function::CLASS_MUTMETHOD:
    //    local_variable(false, "self", TypeRef(TypeRef::TagReference(), true, TypeRef(TypeRef::TagArg(), "Self")));
    //    break;
    //case AST::Function::CLASS_VALMETHOD:
    //    local_variable(false, "self", TypeRef(TypeRef::TagArg(), "Self"));
    //    break;
    //case AST::Function::CLASS_MUTVALMETHOD:
    //    local_variable(true, "self", TypeRef(TypeRef::TagArg(), "Self"));
    //    break;
    //}
    
    DEBUG("args");
    for( auto& arg : fcn.args() )
    {
        handle_type(arg.second);
        // TODO: Check if pattern is irrefutable?
        handle_pattern( arg.first, arg.second );
    }

    DEBUG("code");
    if( fcn.code().is_valid() )
    {
        INDENT();
        handle_expr( fcn.code().node() );
        UNINDENT();
    }
    
    end_scope();
}

void CASTIterator::handle_impl_def(AST::ImplDef& impl)
{
    // Generic params
    handle_params( impl.params() );
    
    // Type
    handle_type( impl.type() );
    
    push_self(impl.type());
    
    // Trait
    if( impl.trait() != AST::Path() )
        handle_path( impl.trait(), MODE_TYPE );
}

void CASTIterator::handle_impl(AST::Path modpath, AST::Impl& impl)
{
    start_scope();
    
    handle_impl_def(impl.def());
    
    // Associated types
    for( auto& it : impl.items() )
    {
        TU_MATCH_DEF(AST::Item, (it.data), (e),
        (
            ),
        (Type,
            DEBUG("- Type '" << it.name << "'");
            handle_type( e.type() );
            ),
        (Function,
            DEBUG("- Function '" << it.name << "'");
            handle_function(AST::Path(AST::Path::TagRelative(), { AST::PathNode(it.name) }), e);
            )
        )
    }
    
    pop_self();
    end_scope();
}

void CASTIterator::handle_struct(AST::Path path, AST::Struct& str)
{
    start_scope();
    handle_params( str.params() );
    TU_MATCH(AST::StructData, (str.m_data), (e),
    (Tuple,
        for( auto& f : e.ents )
            handle_type( f.m_type );
        ),
    (Struct,
        for( auto& f : e.ents )
            handle_type( f.m_type );
        )
    )
    end_scope();
}
void CASTIterator::handle_enum(AST::Path path, AST::Enum& enm)
{
    start_scope();
    handle_params( enm.params() );
    for( auto& f : enm.variants() )
    {
        TU_MATCH(AST::EnumVariantData, (f.m_data), (e),
        (Value,
            ),
        (Tuple,
            for( auto& t : e.m_sub_types )
                handle_type(t);
            ),
        (Struct,
            for( auto& t : e.m_fields )
                handle_type(t.m_type);
            )
        )
    }
    end_scope();
}
void CASTIterator::handle_trait(AST::Path path, AST::Trait& trait)
{
    start_scope();
    push_self(path, trait);
    handle_params( trait.params() );
    
    for( auto& st : trait.supertraits() ) {
        if( st.m_class.is_Invalid() ) {
            // An invalid path is used for 'static
        }
        else {
            handle_path(st, MODE_TYPE);
        }
    }
    
    for( auto& i : trait.items() )
    {
        TU_MATCH_DEF(AST::Item, (i.data), (e),
        (
            ),
        (Type,
            // TODO: Can trait associated types have default types?
            ),
        (Static,
            handle_type(e.type());
            ),
        (Function,
            handle_function( path + i.name, e );
            )
        )
    }
    pop_self();
    end_scope();
}
void CASTIterator::handle_alias(AST::Path path, AST::TypeAlias& alias)
{
    start_scope();
    handle_params( alias.params() );
    handle_type( alias.type() );
    end_scope();
}
void CASTIterator::push_self() {
}
void CASTIterator::push_self(AST::Path path, const AST::Trait& trait) {
}
void CASTIterator::push_self(TypeRef real_type) {
}
void CASTIterator::pop_self() {
}
