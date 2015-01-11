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
template<typename T>
typename ::std::vector<Item<T> >::const_iterator find_named(const ::std::vector<Item<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Item<T>& x) {
        return x.name == name;
    });
}

void Path::resolve()
{
    DEBUG("*this = " << *this);
    if(m_class != ABSOLUTE)
        throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
    if(m_crate == nullptr)
        throw ParseError::BugCheck("Calling Path::resolve on path with no crate");
    
    const Module* mod = &m_crate->root_module();
    for(unsigned int i = 0; i < m_nodes.size(); i ++ )
    {
        const bool is_last = (i+1 == m_nodes.size());
        const bool is_sec_last = (i+2 == m_nodes.size());
        const PathNode& node = m_nodes[i];
        DEBUG("node = " << node);
        
        // Sub-modules
        {
            auto& sms = mod->submods();
            auto it = ::std::find_if(sms.begin(), sms.end(), [&node](const ::std::pair<Module,bool>& x) {
                    return x.first.name() == node.name();
                });
            if( it != sms.end() )
            {
                DEBUG("Sub-module '" << node.name() << "'");
                if( node.args().size() )
                    throw ParseError::Generic("Generic params applied to module");
                mod = &it->first;
                continue;
            }
        }
        // External crates
        {
            auto& crates = mod->extern_crates();
            auto it = find_named(crates, node.name());
            if( it != crates.end() )
            {
                DEBUG("Extern crate '" << node.name() << "'");
                if( node.args().size() )
                    throw ParseError::Generic("Generic params applied to extern crate");
                mod = &it->data.root_module();
                continue;
            }
        }
        
        // Start searching for:
        // - Re-exports
        {
            auto& imp = mod->imports();
            auto it = find_named(imp, node.name());
            if( it != imp.end() )
            {
                DEBUG("Re-exported path " << it->data);
                throw ParseError::Todo("Path::resolve() re-export");
            }
        }
        // - Functions
        {
            auto& items = mod->functions();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Found function");
                if( is_last ) {
                    throw ParseError::Todo("Path::resolve() bind to function");
                }
                else {
                    throw ParseError::Generic("Import of function, too many extra nodes");
                }
            }
        }
        // - Structs
        {
            auto& items = mod->structs();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Found struct");
                if( is_last ) {
                    bind_struct(it->data, node.args());
                    return;
                }
                else if( is_sec_last ) {
                    throw ParseError::Todo("Path::resolve() struct method");
                }
                else {
                    throw ParseError::Generic("Import of struct, too many extra nodes");
                }
            }
        }
        // - Enums (and variants)
        {
            auto& enums = mod->enums();
            auto it = find_named(enums, node.name());
            if( it != enums.end() )
            {
                DEBUG("Found enum");
                if( is_last ) {
                    bind_enum(it->data, node.args());
                    return ;
                }
                else if( is_sec_last ) {
                    bind_enum_var(it->data, m_nodes[i+1].name(), node.args());
                    return ;
                }
                else {
                    throw ParseError::Generic("Import of enum, too many extra nodes");
                }
            }
        }
        // - Constants / statics
        
        throw ParseError::Generic("Unable to find component '" + node.name() + "'");
    }
    
    // We only reach here if the path points to a module
    bind_module(*mod);
}
void Path::bind_module(const Module& mod)
{
    m_binding_type = MODULE;
    m_binding.module_ = &mod;
}
void Path::bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args)
{
    m_binding_type = ENUM;
    m_binding.enum_ = &ent;
    if( args.size() > 0 )
    {
        if( args.size() != ent.params().size() )
            throw ParseError::Generic("Parameter count mismatch");
        throw ParseError::Todo("Bind enum with params passed");
    }
}
void Path::bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args)
{
    unsigned int idx = 0;
    for( idx = 0; idx < ent.variants().size(); idx ++ )
    {
        if( ent.variants()[idx].first == name ) {
            break;
        }
    }
    if( idx == ent.variants().size() )
        throw ParseError::Generic("Enum variant not found");
    
    if( args.size() > 0 )
    {
        if( args.size() != ent.params().size() )
            throw ParseError::Generic("Parameter count mismatch");
        throw ParseError::Todo("Bind enum variant with params passed");
    }
    
    m_binding_type = ENUM_VAR;
    m_binding.enumvar = {&ent, idx};
}
void Path::bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args)
{
    throw ParseError::Todo("Path::resolve() bind to struct type");
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
void Impl::add_function(bool is_public, ::std::string name, Function fcn)
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
    Module  option(crate.crate(), "option");
    option.add_enum(true, "Option", Enum(
        {
            TypeParam(false, "T"),
        },
        {
            StructItem("None", TypeRef()),
            StructItem("Some", TypeRef(TypeRef::TagArg(), "T")),
        }
        ));
    std_mod.add_submod(true, ::std::move(option));
    
    Module  prelude(crate.crate(), "prelude");
    // Re-exports
    #define USE(mod, name, ...)    do{ Path p({__VA_ARGS__}); p.set_crate(crate.crate()); p.resolve(); mod.add_alias(true, ::std::move(p), name); } while(0)
    USE(prelude, "Option",  PathNode("option", {}), PathNode("Option",{}) );
    USE(prelude, "Some",  PathNode("option", {}), PathNode("Option",{}), PathNode("Some",{}) );
    USE(prelude, "None",  PathNode("option", {}), PathNode("Option",{}), PathNode("None",{}) );
    std_mod.add_submod(true, prelude);
    
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
void Module::add_impl(Impl impl)
{
}

void Module::iterate_functions(fcn_visitor_t *visitor, const Crate& crate)
{
    for( auto fcn_item : this->m_functions )
    {
        visitor(crate, *this, fcn_item.data);
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
