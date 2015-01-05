/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>

namespace AST {


// --- AST::PathNode
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

// --- AST::Path
void Path::resolve(const Crate& crate)
{
    DEBUG("*this = " << *this);
    if(m_class != ABSOLUTE)
        throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
    
    const Module* mod = &crate.root_module();
    for(int i = 0; i < m_nodes.size(); i ++ )
    {
        const PathNode& node = m_nodes[i];
        auto& sms = mod->submods();
        
        auto it = ::std::find_if(sms.begin(), sms.end(), [&node](const ::std::pair<Module,bool>& x) {
                return x.first.name() == node.name();
            });
        if( it != sms.end() )
            continue;
        
        // Start searching for:
        // - Re-exports
        // - Functions
        // - Structs
        // - Enums (and variants)
        
    }
    
    throw ParseError::Todo("Path::resolve");
}

Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.m_nodes)
        append(node);
    return *this;
}
::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    switch(path.m_class)
    {
    case Path::RELATIVE:
        os << "Path({" << path.m_nodes << "})";
        break;
    case Path::ABSOLUTE:
        os << "Path(TagAbsolute, {" << path.m_nodes << "})";
        break;
    case Path::LOCAL:
        os << "Path(TagLocal, " << path.m_nodes[0].name() << ")";
        break;
    }
    return os;
}


::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    switch(pat.m_class)
    {
    case Pattern::ANY:
        os << "Pattern(TagWildcard, '" << pat.m_binding << "' @ _)";
        break;
    case Pattern::MAYBE_BIND:
        os << "Pattern(TagMaybeBind, '" << pat.m_binding << "')";
        break;
    case Pattern::VALUE:
        //os << "Pattern(TagValue, " << *pat.m_node << ")";
        os << "Pattern(TagValue, '" << pat.m_binding << "' @ TODO:ExprNode)";
        break;
    case Pattern::TUPLE:
        os << "Pattern(TagTuple, '" << pat.m_binding << "' @ [" << pat.m_sub_patterns << "])";
        break;
    case Pattern::TUPLE_STRUCT:
        os << "Pattern(TagEnumVariant, '" << pat.m_binding << "' @ " << pat.m_path << ", [" << pat.m_sub_patterns << "])";
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

Crate::Crate():
    m_root_module(*this, ""),
    m_load_std(true)
{
}
void Crate::iterate_functions(fcn_visitor_t* visitor)
{
    m_root_module.iterate_functions(visitor, *this);
}

ExternCrate::ExternCrate()
{
}

ExternCrate::ExternCrate(const char *path)
{
    throw ParseError::Todo("Load extern crate from a file");
}

ExternCrate ExternCrate_std()
{
    ExternCrate crate;
    Module& std_mod = crate.root_module();
    
    // TODO: Add modules
    
    return crate;
}

void Module::add_ext_crate(::std::string ext_name, ::std::string int_name)
{
    DEBUG("add_ext_crate(\"" << ext_name << "\" as " << int_name << ")");
    if( ext_name == "std" )
    {
        // HACK! Load std using a hackjob (included within the compiler)
        m_extern_crates.push_back( Item<ExternCrate>( ::std::move(int_name), ExternCrate_std(), false ) );
    }
    else
    {
        throw ParseError::Todo("'extern crate' (not hackjob std)");
    }
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

void ExprNode_CallMethod::visit(NodeVisitor& nv) {
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
