/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"

namespace AST {


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


::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    switch(pat.m_class)
    {
    case Pattern::MAYBE_BIND:
        os << "Pattern(TagMaybeBind, '" << pat.m_path[0].name() << "')";
        break;
    case Pattern::VALUE:
        //os << "Pattern(TagValue, " << *pat.m_node << ")";
        os << "Pattern(TagValue, TODO:ExprNode)";
        break;
    case Pattern::TUPLE:
        os << "Pattern(TagTuple, " << pat.m_sub_patterns << ")";
        break;
    case Pattern::TUPLE_STRUCT:
        os << "Pattern(TagEnumVariant, " << pat.m_path << ", " << pat.m_sub_patterns << ")";
        break;
    }
    return os;
}


Impl::Impl(TypeRef impl_type, TypeRef trait_type)
{
}
void Impl::add_function(bool is_public, Function fcn)
{
}

void Crate::iterate_functions(fcn_visitor_t* visitor)
{
    m_root_module.iterate_functions(visitor, *this);
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
    for( const auto fcn_item : m_functions )
    {
        if( fcn_item.first.name() == func.name() ) {
            throw ParseError::Todo("duplicate function definition");
        }
    }
    m_functions.push_back( ::std::make_pair(func, is_public) );
}
void Module::add_impl(Impl impl)
{
}

void Module::iterate_functions(fcn_visitor_t *visitor, const Crate& crate)
{
    for( auto fcn_item : this->m_functions )
    {
        visitor(crate, *this, fcn_item.first);
    }
}

void Expr::visit_nodes(NodeVisitor& v)
{
    m_node->visit(v);
}
::std::ostream& operator<<(::std::ostream& os, const Expr& pat)
{
    os << "Expr(TODO)";
    return os;
}

ExprNode::~ExprNode() {
}

ExprNode_Block::~ExprNode_Block() {
}
void ExprNode_Block::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Return::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_LetBinding::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Assign::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_CallPath::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_CallObject::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Match::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_If::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Integer::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_StructLiteral::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_NamedValue::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Field::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Cast::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
void ExprNode_BinOp::visit(NodeVisitor& nv) {
    nv.visit(*this);
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
