/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * convert/decorators.cpp
 * - Handles #[...] item decorators by delegating
 */
#include "ast_iterate.hpp"
#include "../ast/ast.hpp"
#include <main_bindings.hpp>
#include <unordered_map>    // C++11, hashmap
#include <synext.hpp>

::std::unordered_map< ::std::string, ::std::unique_ptr<CDecoratorHandler> > g_decorators;

template<typename T>
void Decorator_Apply(AST::Crate& crate, AST::Module& mod, const AST::MetaItems& attrs, const AST::Path& path, T& ent)
{
    // For all attributes on the item, search for a handler and call handler
    for( const auto& attr : attrs.m_items )
    {
        auto it = g_decorators.find(attr.name());
        if( it != g_decorators.end() )
        {
            const CDecoratorHandler&    handler = *it->second;
            
            handler.handle_item(crate, mod, attr, path, ent);
        }
        else {
        }
    }
}

class CProcessor:
    public CASTIterator
{
    AST::Crate& m_crate;
    ::std::vector<AST::Module*> m_modstack;
public:
    CProcessor(AST::Crate& crate):
        m_crate(crate)
    {}
    
    void handle_module(AST::Path path, AST::Module& mod) override
    {
        m_modstack.push_back(&mod);
        CASTIterator::handle_module(mv$(path), mod);
        m_modstack.pop_back();
    }
    
    void handle_struct(AST::Path path, AST::Struct& str) override
    {
        Decorator_Apply(m_crate, *m_modstack.back(), str.attrs(), path, str);
    }
    
    void handle_trait(AST::Path path, AST::Trait& tr) override
    {
        Decorator_Apply(m_crate, *m_modstack.back(), tr.attrs(), path, tr);
    }
};

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<CDecoratorHandler> handler)
{
    auto res = g_decorators.insert( ::std::make_pair(name, mv$(handler)) );
    if( res.second == false )
    {
        DEBUG("Duplicate definition of decorator '"<<name<<"'");
    }
}

void Process_Decorators(AST::Crate& crate)
{
    CProcessor  processor(crate);
    
    processor.handle_module(AST::Path({}), crate.root_module());
}

