#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include "../coretypes.hpp"

class TypeRef;

namespace AST {

class Path
{
public:
    void append(::std::string str) {}
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
};

class Expr
{
public:
    Expr() {}
    Expr(ExprNode node) {}
};

class TypeParam
{
public:
    TypeParam(bool is_lifetime, ::std::string name);
    void addLifetimeBound(::std::string name);
    void addTypeBound(TypeRef type);
};

typedef ::std::vector<TypeParam>    TypeParams;
typedef ::std::pair< ::std::string, TypeRef>    StructItem;

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
