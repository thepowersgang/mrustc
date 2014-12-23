#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>

#include "../types.hpp"

namespace AST {

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
public:
    Pattern();

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name);

    struct TagValue {};
    Pattern(TagValue, ExprNode node);

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns);
};

class ExprNode
{
public:
    ExprNode();

    struct TagBlock {};
    ExprNode(TagBlock, ::std::vector<ExprNode> nodes);

    struct TagLetBinding {};
    ExprNode(TagLetBinding, Pattern pat, ExprNode value);

    struct TagReturn {};
    ExprNode(TagReturn, ExprNode val);

    struct TagAssign {};
    ExprNode(TagAssign, ExprNode slot, ExprNode value) {}

    struct TagCast {};
    ExprNode(TagCast, ExprNode value, TypeRef dst_type);

    struct TagInteger {};
    ExprNode(TagInteger, uint64_t value, enum eCoreType datatype);

    struct TagStructLiteral {};
    ExprNode(TagStructLiteral, Path path, ExprNode base_value, ::std::vector< ::std::pair< ::std::string,ExprNode> > values );

    struct TagCallPath {};
    ExprNode(TagCallPath, Path path, ::std::vector<ExprNode> args);

    struct TagCallObject {};
    ExprNode(TagCallObject, ExprNode val, ::std::vector<ExprNode> args);

    struct TagMatch {};
    ExprNode(TagMatch, ExprNode val, ::std::vector< ::std::pair<Pattern,ExprNode> > arms);

    struct TagIf {};
    ExprNode(TagIf, ExprNode cond, ExprNode true_code, ExprNode false_code);

    struct TagNamedValue {};
    ExprNode(TagNamedValue, Path path);

    struct TagField {};
    ExprNode(TagField, ::std::string name);

    enum BinOpType {
        BINOP_CMPEQU,
        BINOP_CMPNEQU,

        BINOP_BITAND,
        BINOP_BITOR,
        BINOP_BITXOR,

        BINOP_SHL,
        BINOP_SHR,
    };
    struct TagBinOp {};
    ExprNode(TagBinOp, BinOpType type, ExprNode left, ExprNode right);
};

class NodeVisitor
{
public:
    virtual void visit(ExprNode::TagBlock, ExprNode& node) {}
    virtual void visit(ExprNode::TagNamedValue, ExprNode& node) {}
};

class Expr
{
public:
    Expr() {}
    Expr(ExprNode node) {}

    void visit_nodes(const NodeVisitor& v);
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
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:

    Function(::std::string name, TypeParams params, Class fcn_class, TypeRef ret_type, Arglist args, Expr code);

    Expr& code() { return m_code; }
    const Expr code() const { return m_code; }

    const TypeRef& rettype() const { return m_rettype; }
    TypeRef& rettype() { return m_rettype; }

    const Arglist& args() const { return m_args; }
    Arglist& args() { return m_args; }
    
    const char* name() const { return "TODO"; }
};

class Impl
{
public:
    Impl(TypeRef impl_type, TypeRef trait_type);

    void add_function(bool is_public, Function fcn);
};

class Module
{
    ::std::vector<Function> m_functions;
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, Function func);
    void add_impl(Impl impl);
};

class Crate
{
    Module  m_root_module;
public:
    Crate(Module root_module):
        m_root_module(root_module)
    {
    }

    typedef void fcn_visitor_t(const AST::Crate& crate, Function& fcn);

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
    
    const ::std::vector<Function>& functions() const;
    const ::std::vector<CStruct>& structs() const;
};

}

#endif // AST_HPP_INCLUDED
