/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>

namespace AST {

Path::Path()
{
}
Path::Path(Path::TagAbsolute)
{
}

const ::std::string& PathNode::name() const
{
    return m_name;
}
const TypeParams& PathNode::args() const
{
    return m_params;
}

Pattern::Pattern(TagMaybeBind, ::std::string name)
{
}
Pattern::Pattern(TagValue, ExprNode node)
{
}

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
ExprNode::ExprNode(TagCallPath, Path path, ::std::vector<ExprNode> args)
{
}
ExprNode::ExprNode(TagMatch, ExprNode val, ::std::vector< ::std::pair<Pattern,ExprNode> > arms)
{
}
ExprNode::ExprNode(TagNamedValue, Path path)
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
