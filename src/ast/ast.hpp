#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>

#include "../parse/tokentree.hpp"
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
    
    const ::std::string& name() const { return m_name; }
};

class ExprNode;

class Pattern
{
public:
    enum BindType {
        MAYBE_BIND,
        ANY,
        VALUE,
        TUPLE,
        TUPLE_STRUCT,
    };
private:
    BindType    m_class;
    ::std::string   m_binding;
    Path    m_path;
    unique_ptr<ExprNode>    m_node;
    ::std::vector<Pattern>  m_sub_patterns;
public:
    Pattern():
        m_class(ANY)
    {}

    struct TagBind {};
    Pattern(TagBind, ::std::string name):
        m_class(ANY),
        m_binding(name)
    {}

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_class(MAYBE_BIND),
        m_binding(name)
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
    
    // Mutators
    void set_bind(::std::string name) {
        m_binding = name;
    }
    
    // Accessors
    const ::std::string& binding() const { return m_binding; }
    BindType type() const { return m_class; }
    ExprNode& node() { return *m_node; }
    const ExprNode& node() const { return *m_node; }
    Path& path() { return m_path; }
    const Path& path() const { return m_path; }
    ::std::vector<Pattern>& sub_patterns() { return m_sub_patterns; }
    const ::std::vector<Pattern>& sub_patterns() const { return m_sub_patterns; }

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


class Crate;
class ExternCrate;
class Module;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

template <typename T>
struct Item
{
    ::std::string   name;
    T   data;
    bool    is_pub;
    
    Item(::std::string&& name, T&& data, bool is_pub):
        name( move(name) ),
        data( move(data) ),
        is_pub( is_pub )
    {
    }
};

/// Representation of a parsed (and being converted) function
class Module
{
    typedef ::std::vector< ::std::pair<Function, bool> >   itemlist_fcn_t;
    typedef ::std::vector< ::std::pair<Module, bool> >   itemlist_mod_t;
    typedef ::std::vector< Item<Path> > itemlist_use_t;
    typedef ::std::vector< Item<ExternCrate> >  itemlist_ext_t;

    const Crate& m_crate;
    ::std::string   m_name;
    ::std::vector<MetaItem> m_attrs;
    itemlist_fcn_t  m_functions;
    itemlist_mod_t  m_submods;
    itemlist_use_t  m_imports;
    itemlist_ext_t  m_extern_crates;
public:
    Module(const Crate& crate, ::std::string name):
        m_crate(crate),
        m_name(name)
    {
    }
    void add_ext_crate(::std::string ext_name, ::std::string imp_name);
    void add_alias(bool is_public, Path path, ::std::string name) {
        m_imports.push_back( Item<Path>( move(name), move(path), is_public) );
    }
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, Function func);
    void add_impl(Impl impl);

    void add_attr(MetaItem item) {
        m_attrs.push_back(item);
    }

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);

    const Crate& crate() const { return m_crate; }  
 
    const ::std::string& name() const { return m_name; }
    
    ::std::vector<MetaItem>& attrs() { return m_attrs; } 
    itemlist_fcn_t& functions() { return m_functions; }
    itemlist_mod_t& submods() { return m_submods; }
    itemlist_use_t& imports() { return m_imports; }
    itemlist_ext_t& extern_crates() { return m_extern_crates; }

    const ::std::vector<MetaItem>& attrs() const { return m_attrs; } 
    const itemlist_fcn_t& functions() const { return m_functions; }
    const itemlist_mod_t& submods() const { return m_submods; }
    const itemlist_use_t& imports() const { return m_imports; }
    const itemlist_ext_t& extern_crates() const { return m_extern_crates; }
};

class Crate
{
public:
    Module  m_root_module;
    bool    m_load_std;

    Crate();

    Module& root_module() { return m_root_module; }
    const Module& root_module() const { return m_root_module; }
    
    void iterate_functions( fcn_visitor_t* visitor );
};

/// Representation of an imported crate
/// - Functions are stored as resolved+typechecked ASTs
class ExternCrate
{
    Crate   m_crate;
public:
    ExternCrate();
    ExternCrate(const char *path);
    Module& root_module() { return m_crate.root_module(); }
    const Module& root_module() const { return m_crate.root_module(); }
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
