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
    struct TagAssign {};
    ExprNode(TagAssign, ExprNode slot, ExprNode value) {}
    struct TagInteger {};

    ExprNode(TagInteger, uint64_t value, enum eCoreType datatype);
};

class Expr
{
public:
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

class Module
{
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
};

}

#endif // AST_HPP_INCLUDED
