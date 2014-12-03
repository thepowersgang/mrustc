#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"

class TypeRef;

namespace AST {

class ExprNode;

class TypeParam
{
public:
    TypeParam(bool is_lifetime, ::std::string name);
    void addLifetimeBound(::std::string name);
    void addTypeBound(TypeRef type);
};

typedef ::std::vector<TypeParam>    TypeParams;
typedef ::std::pair< ::std::string, TypeRef>    StructItem;

class PathNode
{
    ::std::string   m_name;
    TypeParams  m_params;
public:
    const ::std::string& name() const;
    const TypeParams&   args() const;
};

class Path
{
public:
    Path();
    struct TagAbsolute {};
    Path(TagAbsolute);

    void append(::std::string str) {}
    size_t length() const {return 0;}

    PathNode& operator[](size_t idx) { throw ::std::out_of_range("Path []"); }
};

class Pattern
{
public:
    Pattern();

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name);

    struct TagValue {};
    Pattern(TagValue, ExprNode node);
};

class ExprNode
{
public:
    ExprNode();

    struct TagBlock {};
    ExprNode(TagBlock, ::std::vector<ExprNode> nodes);

    struct TagAssign {};
    ExprNode(TagAssign, ExprNode slot, ExprNode value) {}

    struct TagInteger {};
    ExprNode(TagInteger, uint64_t value, enum eCoreType datatype);

    struct TagCallPath {};
    ExprNode(TagCallPath, Path path, ::std::vector<ExprNode> args);

    struct TagMatch {};
    ExprNode(TagMatch, ExprNode val, ::std::vector< ::std::pair<Pattern,ExprNode> > arms);

    struct TagNamedValue {};
    ExprNode(TagNamedValue, Path path);
};

class Expr
{
public:
    Expr() {}
    Expr(ExprNode node) {}
};

class Module
{
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, ::std::string name, TypeParams params, TypeRef ret_type, ::std::vector<StructItem> args, Expr code);
};

}

#endif // AST_HPP_INCLUDED
