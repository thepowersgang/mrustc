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


PathNode::PathNode(::std::string name, ::std::vector<TypeRef> args):
    m_name(name),
    m_params(args)
{
}
const ::std::string& PathNode::name() const
{
    return m_name;
}
const ::std::vector<TypeRef>& PathNode::args() const
{
    return m_params;
}

Pattern::Pattern(TagMaybeBind, ::std::string name)
{
}
Pattern::Pattern(TagValue, ExprNode node)
{
}
Pattern::Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns)
{
}


Function::Function(::std::string name, TypeParams params, Class fcn_class, TypeRef ret_type, ::std::vector<StructItem> args, Expr code)
{
}

Impl::Impl(TypeRef impl_type, TypeRef trait_type)
{
}
void Impl::add_function(bool is_public, Function fcn)
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
void Module::add_function(bool is_public, Function func)
{
}
void Module::add_impl(Impl impl)
{
}

ExprNode::ExprNode()
{

}
ExprNode::ExprNode(TagBlock, ::std::vector<ExprNode> nodes)
{
}
ExprNode::ExprNode(TagLetBinding, Pattern pat, ExprNode value)
{
}
ExprNode::ExprNode(TagReturn, ExprNode val)
{
}
ExprNode::ExprNode(TagCast, ExprNode value, TypeRef dst_type)
{
}
ExprNode::ExprNode(TagInteger, uint64_t value, enum eCoreType datatype)
{
}
ExprNode::ExprNode(TagStructLiteral, Path path, ExprNode base_value, ::std::vector< ::std::pair< ::std::string,ExprNode> > values )
{
}
ExprNode::ExprNode(TagCallPath, Path path, ::std::vector<ExprNode> args)
{
}
ExprNode::ExprNode(TagCallObject, ExprNode val, ::std::vector<ExprNode> args)
{
}
ExprNode::ExprNode(TagMatch, ExprNode val, ::std::vector< ::std::pair<Pattern,ExprNode> > arms)
{
}
ExprNode::ExprNode(TagIf, ExprNode cond, ExprNode true_code, ExprNode false_code)
{
}
ExprNode::ExprNode(TagNamedValue, Path path)
{
}
ExprNode::ExprNode(TagField, ::std::string name)
{
}
ExprNode::ExprNode(TagBinOp, BinOpType type, ExprNode left, ExprNode right)
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
