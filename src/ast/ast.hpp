#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>

#include "../types.hpp"

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class Crate;

class MetaItem
{
    ::std::string   m_name;
    ::std::vector<MetaItem> m_items;
    ::std::string   m_str_val;
public:
    MetaItem(::std::string name):
        m_name(name)
    {
    }
    MetaItem(::std::string name, ::std::vector<MetaItem> items):
        m_name(name),
        m_items(items)
    {
    }
};

class ExprNode;

class Pattern
{
    enum BindType {
        MAYBE_BIND,
        VALUE,
        TUPLE,
        TUPLE_STRUCT,
    };
    BindType    m_class;
    Path    m_path;
    unique_ptr<ExprNode>    m_node;
    ::std::vector<Pattern>  m_sub_patterns;
public:
    Pattern();

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_class(MAYBE_BIND),
        m_path(Path::TagLocal(), name)
    {}

    struct TagValue {};
    Pattern(TagValue, unique_ptr<ExprNode> node):
        m_class(VALUE),
        m_node( ::std::move(node) )
    {}

    struct TagTuple {};
    Pattern(TagTuple, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE_STRUCT),
        m_path( ::std::move(path) ),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);
};

#include "ast_expr.hpp"

class Expr
{
    ::std::shared_ptr<ExprNode> m_node;
public:
    Expr(unique_ptr<ExprNode> node):
        m_node(node.release())
    {
    }
    Expr(ExprNode* node):
        m_node(node)
    {
    }

    void visit_nodes(NodeVisitor& v);

    friend ::std::ostream& operator<<(::std::ostream& os, const Expr& pat);
};

class Function
{
public:
    enum Class
    {
        CLASS_UNBOUND,
        CLASS_REFMETHOD,
        CLASS_MUTMETHOD,
        CLASS_VALMETHOD,
    };
    typedef ::std::vector<StructItem>   Arglist;

private:
    ::std::string   m_name;
    TypeParams  m_generic_params;
    Class   m_fcn_class;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:
    Function(::std::string name, TypeParams params, Class fcn_class, TypeRef ret_type, Arglist args, Expr code):
        m_name(name),
        m_generic_params(params),
        m_fcn_class(fcn_class),
        m_code(code),
        m_rettype(ret_type),
        m_args(args)
    {
    }
    
    const ::std::string& name() const { return m_name; }
    
    TypeParams& generic_params() { return m_generic_params; }
    const TypeParams& generic_params() const { return m_generic_params; }

    Expr& code() { return m_code; }
    const Expr& code() const { return m_code; }

    const TypeRef& rettype() const { return m_rettype; }
    TypeRef& rettype() { return m_rettype; }

    const Arglist& args() const { return m_args; }
    Arglist& args() { return m_args; }
};

class Impl
{
public:
    Impl(TypeRef impl_type, TypeRef trait_type);

    void add_function(bool is_public, Function fcn);
};


class Module;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

class Module
{
    ::std::vector< ::std::pair<Function,bool> > m_functions;
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, Function func);
    void add_impl(Impl impl);

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);
};

class Crate
{
    Module  m_root_module;
public:
    Crate(Module root_module):
        m_root_module(root_module)
    {
    }

    void iterate_functions( fcn_visitor_t* visitor );
};

class CStruct
{
    ::std::vector<StructItem>   m_fields;
public:
    const char* name() const { return "TODO"; }
    const char* mangled_name() const { return "TODO"; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
};

class Flat
{
    ::std::vector<CStruct>  m_structs;
    ::std::vector<Function> m_functions;
public:
    
    const ::std::vector<Function>& functions() const { return m_functions; }
    const ::std::vector<CStruct>& structs() const { return m_structs; }
};

}

#endif // AST_HPP_INCLUDED
