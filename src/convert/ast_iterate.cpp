/**
 */
#include "ast_iterate.hpp"
#include "../ast/ast.hpp"

void CASTIterator::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
}
void CASTIterator::handle_type(TypeRef& type)
{
    //DEBUG("type = " << type);
    if( type.is_path() )
    {
        handle_path(type.path(), MODE_TYPE);
    }
    else
    {
        for(auto& subtype : type.sub_types())
            handle_type(subtype);
    }
}
void CASTIterator::handle_expr(AST::ExprNode& node)
{
}
void CASTIterator::handle_params(AST::TypeParams& params)
{
    for( auto& param : params.params() )
    {
        if( param.is_type() )
            local_type( param.name(), TypeRef(TypeRef::TagArg(), param.name()) );
    }
    for( auto& bound : params.bounds() )
    {
        handle_type(bound.type());
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
    switch(pat.type())
    {
    case AST::Pattern::ANY:
        // Wildcard, nothing to do
        break;
    case AST::Pattern::MAYBE_BIND:
        throw ::std::runtime_error("Calling CASTIterator::handle_pattern on MAYBE_BIND, not valid");
    case AST::Pattern::VALUE:
        handle_expr( pat.node() );
        break;
    case AST::Pattern::TUPLE:
        // Tuple is handled by subpattern code
        break;
    case AST::Pattern::TUPLE_STRUCT:
        // Resolve the path!
        // - TODO: Restrict to types and enum variants
        handle_path( pat.path(), CASTIterator::MODE_TYPE );
        break;
    }
    // Extract bindings and add to namespace
    if( pat.binding().size() > 0 )
    {
        // TODO: Mutable bindings
        local_variable( false, pat.binding(), type_hint );
    }
    for( auto& subpat : pat.sub_patterns() )
        handle_pattern(subpat, (const TypeRef&)TypeRef());
}

void CASTIterator::handle_module(AST::Path path, AST::Module& mod)
{
    INDENT();
    start_scope();
    
    for( auto& item : mod.structs() )
    {
        DEBUG("Handling struct " << item.name);
        handle_struct(path + item.name, item.data);
    }
    for( auto& item : mod.enums() )
    {
        DEBUG("Handling enum " << item.name);
        handle_enum(path + item.name, item.data);
    }
    for( auto& item : mod.traits() )
    {
        DEBUG("Handling trait " << item.name);
        handle_trait(path + item.name, item.data);
    }
    for( auto& item : mod.type_aliases() )
    {
        DEBUG("Handling alias " << item.name);
        handle_alias(path + item.name, item.data);
    }
    
    for( auto& fcn : mod.functions() )
    {
        DEBUG("Handling function '" << fcn.name << "'");
        handle_function(path + fcn.name, fcn.data);
    }
    for( auto& impl : mod.impls() )
    {
        DEBUG("Handling 'impl' " << impl);
        handle_impl(path, impl);
    }
    
    // End scope before handling sub-modules
    end_scope(); 
 
    for( auto& submod : mod.submods() )
    {
        DEBUG("Handling submod '" << submod.first.name() << "'");
        handle_module(path + submod.first.name(), submod.first);
    }
    UNINDENT();
}
void CASTIterator::handle_function(AST::Path path, AST::Function& fcn)
{
    start_scope();
    
    handle_params(fcn.params());
    
    handle_type(fcn.rettype());
    
    for( auto& arg : fcn.args() )
    {
        handle_type(arg.second);
        AST::Pattern    pat(AST::Pattern::TagBind(), arg.first);
        handle_pattern( pat, arg.second );
    }

    if( fcn.code().is_valid() )
    {
        handle_expr( fcn.code().node() );
    }
    
    end_scope();
}
void CASTIterator::handle_impl(AST::Path modpath, AST::Impl& impl)
{
    start_scope();
    
    // Generic params
    handle_params( impl.params() );
    
    // Trait
    handle_type( impl.trait() );
    // Type
    handle_type( impl.type() );
    
    local_type("Self", impl.type());
    
    // TODO: Associated types
    
    // Functions
    for( auto& fcn : impl.functions() )
    {
        DEBUG("- Function '" << fcn.name << "'");
        handle_function(AST::Path() + fcn.name, fcn.data);
    }
    
    end_scope();
}

void CASTIterator::handle_struct(AST::Path path, AST::Struct& str)
{
    start_scope();
    handle_params( str.params() );
    for( auto& f : str.fields() )
        handle_type( f.second );
    end_scope();
}
void CASTIterator::handle_enum(AST::Path path, AST::Enum& enm)
{
    start_scope();
    handle_params( enm.params() );
    for( auto& f : enm.variants() )
        handle_type( f.second );
    end_scope();
}
void CASTIterator::handle_trait(AST::Path path, AST::Trait& trait)
{
    start_scope();
    handle_params( trait.params() );
    for( auto& fcn : trait.functions() )
        handle_function( path + fcn.name, fcn.data );
    end_scope();
}
void CASTIterator::handle_alias(AST::Path path, AST::TypeAlias& alias)
{
    start_scope();
    handle_params( alias.params() );
    handle_type( alias.type() );
    end_scope();
}
