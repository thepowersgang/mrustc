#ifndef _AST_ITERATE_HPP_INCLUDED_
#define _AST_ITERATE_HPP_INCLUDED_

#include <string>
#include "../types.hpp"
#include "../ast/path.hpp"

namespace AST {

class ExprNode;
class Pattern;
class TypeParams;
class Impl;

};

// Iterator for the AST, calls virtual functions on paths/types
class CASTIterator
{
public:
    enum PathMode {
        MODE_EXPR,  // Variables allowed
        MODE_TYPE,
        MODE_BIND,  // Failure is allowed
    };
    virtual void handle_path(AST::Path& path, PathMode mode) = 0;
    virtual void handle_type(TypeRef& type);
    virtual void handle_expr(AST::ExprNode& node);

    virtual void handle_params(AST::TypeParams& params);
    
    virtual void start_scope();
    virtual void local_type(::std::string name);
    virtual void local_variable(bool is_mut, ::std::string name, const TypeRef& type);
    virtual void local_use(::std::string name, AST::Path path);
    virtual void end_scope();
    
    virtual void handle_pattern(AST::Pattern& pat, const TypeRef& type_hint);
    
    virtual void handle_module(AST::Path path, AST::Module& mod);

    virtual void handle_function(AST::Path path, AST::Function& fcn);
    virtual void handle_impl(AST::Path modpath, AST::Impl& impl);
    
    virtual void handle_struct(AST::Path path, AST::Struct& str);
    virtual void handle_enum(AST::Path path, AST::Enum& enm);
    virtual void handle_alias(AST::Path path, AST::TypeAlias& alias);
};

#endif

