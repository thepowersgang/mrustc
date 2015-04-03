/*
 */
#include "ast_iterate.hpp"
#include "../ast/ast.hpp"
#include <main_bindings.hpp>
#include <unordered_map>    // C++11, hashmap
#include <synext.hpp>

::std::unordered_map< ::std::string, ::std::unique_ptr<CDecoratorHandler> > g_decorators;

template<typename T>
bool Decorator_Apply(AST::Module& mod, const AST::MetaItem& attr, const AST::Path& path, T& ent)
{
    auto it = g_decorators.find(attr.name());
    if( it == g_decorators.end() )
    {
        return false;
    }
    
    const CDecoratorHandler&    handler = *it->second;
    
    handler.handle_item(mod, attr, path, ent);
    return true;
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
        // For all attributes on the struct, search for a handler and call handler
        auto& attrs = str.attrs();
        for( auto& attr : attrs.m_items )
        {
            Decorator_Apply(*m_modstack.back(), attr, path, str);
        }
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

