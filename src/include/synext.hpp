/*
 */
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../common.hpp"   // for mv$ and other things
#include <string>
#include <memory>

namespace AST {
    class MetaItem;
    class Path;
    class Module;
    class Struct;
}

class CDecoratorHandler
{
public:
    virtual void handle_item(AST::Module& mod, const AST::MetaItem& attr, const AST::Path& path, AST::Struct& str) const = 0;
};

#define STATIC_SYNEXT(_type, ident, _typename) \
    struct register_##_typename##_c {\
        register_##_typename##_c() {\
            Register_Synext_##_type( ident, ::std::unique_ptr<C##_type##Handler>(new _typename()) ); \
        } \
    } s_register_##_typename;

extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<CDecoratorHandler> handler);

#endif

