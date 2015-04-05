/*
 */
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../common.hpp"   // for mv$ and other things
#include <string>
#include <memory>

namespace AST {
    class Crate;
    class MetaItem;
    class Path;
    
    class Module;
    
    class Struct;
    class Trait;
}

class CDecoratorHandler
{
public:
    virtual void handle_item(AST::Crate&, AST::Module&, const AST::MetaItem&, const AST::Path&, AST::Struct& ) const
    {
    }
    virtual void handle_item(AST::Crate&, AST::Module&, const AST::MetaItem&, const AST::Path&, AST::Trait& ) const
    {
    }
};

#define STATIC_SYNEXT(_type, ident, _typename) \
    struct register_##_typename##_c {\
        register_##_typename##_c() {\
            Register_Synext_##_type( ident, ::std::unique_ptr<C##_type##Handler>(new _typename()) ); \
        } \
    } s_register_##_typename;

extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<CDecoratorHandler> handler);

#endif

