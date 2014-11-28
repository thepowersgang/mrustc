/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>

namespace AST {

void Module::add_constant(bool is_public, ::std::string name, TypeRef type, Expr val)
{
    ::std::cout << "add_constant()" << ::std::endl;
}

void Module::add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val)
{
    ::std::cout << "add_global()" << ::std::endl;
}
void Module::add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items)
{
}
void Module::add_function(bool is_public, ::std::string name, TypeParams params, TypeRef ret_type, ::std::vector<StructItem> args, Expr code)
{
}

ExprNode::ExprNode()
{

}
ExprNode::ExprNode(TagBlock, ::std::vector<ExprNode> nodes)
{
}
ExprNode::ExprNode(TagInteger, uint64_t value, enum eCoreType datatype)
{

}

TypeParam::TypeParam(bool is_lifetime, ::std::string name)
{

}
void TypeParam::addLifetimeBound(::std::string name)
{

}
void TypeParam::addTypeBound(TypeRef type)
{

}

}
