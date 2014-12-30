/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"

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
Pattern::Pattern(TagValue, ::std::unique_ptr<ExprNode> node)
{
}
Pattern::Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns)
{
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

ExprNode::~ExprNode() {
}

ExprNode_Block::~ExprNode_Block() {
}
void ExprNode_Block::visit(NodeVisitor& nv) {
    for( auto& node : m_nodes ) {
        if( node.get() )
            node->visit(nv);
    }
    nv.visit(*this);
}

void ExprNode_Return::visit(NodeVisitor& nv) {
    if( m_value.get() )
        m_value->visit(nv);
    nv.visit(*this);
}

void ExprNode_LetBinding::visit(NodeVisitor& nv) {
    if( m_value.get() )
        m_value->visit(nv);
    nv.visit(*this);
}

void ExprNode_Assign::visit(NodeVisitor& nv) {
    if( m_slot.get() )
        m_slot->visit(nv);
    if( m_value.get() )
        m_value->visit(nv);
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
    m_left->visit(nv);
    m_right->visit(nv);
    nv.visit(*this);
}

#if 0
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
#endif

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
