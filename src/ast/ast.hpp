#ifndef AST_HPP_INCLUDED
#define AST_HPP_INCLUDED

#include <string>
#include <vector>
#include <stdexcept>
#include "../coretypes.hpp"
#include <memory>

#include "../types.hpp"

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class Crate;

class MetaItem
{
    ::std::string   m_name;
    ::std::vector<MetaItem> m_items;
    ::std::string   m_str_val;
public:
    MetaItem(::std::string name):
        m_name(name)
    {
    }
    MetaItem(::std::string name, ::std::vector<MetaItem> items):
        m_name(name),
        m_items(items)
    {
    }
};

class ExprNode;

class Pattern
{
public:
    Pattern();

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name);

    struct TagValue {};
    Pattern(TagValue, unique_ptr<ExprNode> node);

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns);
};

class NodeVisitor;

class ExprNode
{
public:
    virtual ~ExprNode() = 0;
    
    virtual void visit(NodeVisitor& nv) = 0;
};

class ExprNode_Block:
    public ExprNode
{
    ::std::vector< ::std::unique_ptr<ExprNode> >    m_nodes;
public:
    ExprNode_Block(const ExprNode_Block& x) = delete;
    ExprNode_Block(::std::vector< ::std::unique_ptr<ExprNode> >&& nodes):
        m_nodes( move(nodes) )
    {
    }
    virtual ~ExprNode_Block() override;
    
    virtual void visit(NodeVisitor& nv) override;
};

// Return a value
class ExprNode_Return:
    public ExprNode
{
    unique_ptr<ExprNode>    m_value;
public:
    ExprNode_Return(unique_ptr<ExprNode>&& value):
        m_value( move(value) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;
};
class ExprNode_LetBinding:
    public ExprNode
{
    Pattern m_pat;
    unique_ptr<ExprNode>    m_value;
public:
    ExprNode_LetBinding(Pattern pat, unique_ptr<ExprNode>&& value):
        m_pat( move(pat) ),
        m_value( move(value) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;
};
class ExprNode_Assign:
    public ExprNode
{
    unique_ptr<ExprNode>    m_slot;
    unique_ptr<ExprNode>    m_value;
public:
    ExprNode_Assign(unique_ptr<ExprNode>&& slot, unique_ptr<ExprNode>&& value):
        m_slot( move(slot) ),
        m_value( move(value) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;
};
class ExprNode_CallPath:
    public ExprNode
{
    Path    m_path;
    ::std::vector<unique_ptr<ExprNode>> m_args;
public:
    ExprNode_CallPath(Path&& path, ::std::vector<unique_ptr<ExprNode>>&& args):
        m_path( move(path) ),
        m_args( move(args) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;
};
// Call an object (Fn/FnMut/FnOnce)
class ExprNode_CallObject:
    public ExprNode
{
    unique_ptr<ExprNode>    m_val;
    ::std::vector<unique_ptr<ExprNode>> m_args;
public:
    ExprNode_CallObject(unique_ptr<ExprNode>&& val, ::std::vector< unique_ptr<ExprNode> >&& args):
        m_val( move(val) ),
        m_args( move(args) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};

class ExprNode_Match:
    public ExprNode
{
    typedef ::std::vector< ::std::pair<Pattern,unique_ptr<ExprNode> > > arm_t;
    unique_ptr<ExprNode>    m_val;
    arm_t   m_arms;
public:
    ExprNode_Match(unique_ptr<ExprNode>&& val, arm_t&& arms):
        m_val( ::std::move(val) ),
        m_arms( ::std::move(arms) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};

class ExprNode_If:
    public ExprNode
{
    unique_ptr<ExprNode>    m_cond;
    unique_ptr<ExprNode>    m_true;
    unique_ptr<ExprNode>    m_false;
public:
    ExprNode_If(unique_ptr<ExprNode>&& cond, unique_ptr<ExprNode>&& true_code, unique_ptr<ExprNode>&& false_code):
        m_cond( ::std::move(cond) ),
        m_true( ::std::move(true_code) ),
        m_false( ::std::move(false_code) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};
// Literal integer
class ExprNode_Integer:
    public ExprNode
{
    enum eCoreType  m_datatype;
    uint64_t    m_value;
public:
    ExprNode_Integer(uint64_t value, enum eCoreType datatype):
        m_datatype(datatype),
        m_value(value)
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;
};
// Literal structure
class ExprNode_StructLiteral:
    public ExprNode
{
    typedef ::std::vector< ::std::pair< ::std::string, unique_ptr<ExprNode> > > t_values;
    Path    m_path;
    unique_ptr<ExprNode>    m_base_value;
    t_values    m_values;
public:
    ExprNode_StructLiteral(Path path, unique_ptr<ExprNode>&& base_value, t_values&& values ):
        m_path( move(path) ),
        m_base_value( move(base_value) ),
        m_values( move(values) )
    {}
    
    virtual void visit(NodeVisitor& nv) override;
};
// Variable / Constant
class ExprNode_NamedValue:
    public ExprNode
{
    Path    m_path;
public:
    ExprNode_NamedValue(Path&& path):
        m_path( ::std::move(path) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};
// Field dereference
class ExprNode_Field:
    public ExprNode
{
    ::std::unique_ptr<ExprNode> m_obj;
    ::std::string   m_name;
public:
    ExprNode_Field(::std::unique_ptr<ExprNode>&& obj, ::std::string&& name):
        m_obj( ::std::move(obj) ),
        m_name( ::std::move(name) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};

// Type cast ('as')
class ExprNode_Cast:
    public ExprNode
{
    unique_ptr<ExprNode>    m_value;
    TypeRef m_type;
public:
    ExprNode_Cast(unique_ptr<ExprNode>&& value, TypeRef&& dst_type):
        m_value( move(value) ),
        m_type( move(dst_type) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;
};

// Binary operation
class ExprNode_BinOp:
    public ExprNode
{
public:
    enum Type {
        CMPEQU,
        CMPNEQU,

        BITAND,
        BITOR,
        BITXOR,

        SHL,
        SHR,
    };
private:
    Type    m_type;
    ::std::unique_ptr<ExprNode> m_left;
    ::std::unique_ptr<ExprNode> m_right;
public:
    ExprNode_BinOp(const ExprNode_Block& x) = delete;
    ExprNode_BinOp(Type type, ::std::unique_ptr<ExprNode> left, ::std::unique_ptr<ExprNode> right):
        m_type(type),
        m_left( ::std::move(left) ),
        m_right( ::std::move(right) )
    {
    }
    virtual ~ExprNode_BinOp() override {}
    
    virtual void visit(NodeVisitor& nv) override;
};

class NodeVisitor
{
public:
    virtual void visit(ExprNode_Block& node) {}
    virtual void visit(ExprNode_Return& node) {}
    virtual void visit(ExprNode_LetBinding& node) {}
    virtual void visit(ExprNode_Assign& node) {}
    virtual void visit(ExprNode_CallPath& node) {}
    virtual void visit(ExprNode_CallObject& node) {}
    virtual void visit(ExprNode_Match& node) {}
    virtual void visit(ExprNode_If& node) {}
    
    virtual void visit(ExprNode_Integer& node) {}
    virtual void visit(ExprNode_StructLiteral& node) {}
    virtual void visit(ExprNode_NamedValue& node) {}
    
    virtual void visit(ExprNode_Field& node) {}
    virtual void visit(ExprNode_Cast& node) {}
    virtual void visit(ExprNode_BinOp& node) {}
};

class Expr
{
    ::std::shared_ptr<ExprNode> m_node;
public:
    Expr(unique_ptr<ExprNode> node):
        m_node(node.release())
    {
    }
    Expr(ExprNode* node):
        m_node(node)
    {
    }

    void visit_nodes(NodeVisitor& v);
};

class Function
{
public:
    enum Class
    {
        CLASS_UNBOUND,
        CLASS_REFMETHOD,
        CLASS_MUTMETHOD,
        CLASS_VALMETHOD,
    };
    typedef ::std::vector<StructItem>   Arglist;

private:
    ::std::string   m_name;
    TypeParams  m_generic_params;
    Class   m_fcn_class;
    Expr    m_code;
    TypeRef m_rettype;
    Arglist m_args;
public:
    Function(::std::string name, TypeParams params, Class fcn_class, TypeRef ret_type, Arglist args, Expr code):
        m_name(name),
        m_generic_params(params),
        m_fcn_class(fcn_class),
        m_code(code),
        m_rettype(ret_type),
        m_args(args)
    {
    }
    
    const ::std::string& name() const { return m_name; }
    
    TypeParams& generic_params() { return m_generic_params; }
    const TypeParams& generic_params() const { return m_generic_params; }

    Expr& code() { return m_code; }
    const Expr& code() const { return m_code; }

    const TypeRef& rettype() const { return m_rettype; }
    TypeRef& rettype() { return m_rettype; }

    const Arglist& args() const { return m_args; }
    Arglist& args() { return m_args; }
};

class Impl
{
public:
    Impl(TypeRef impl_type, TypeRef trait_type);

    void add_function(bool is_public, Function fcn);
};


class Module;

typedef void fcn_visitor_t(const AST::Crate& crate, const AST::Module& mod, Function& fcn);

class Module
{
    ::std::vector< ::std::pair<Function,bool> > m_functions;
public:
    void add_alias(bool is_public, Path path) {}
    void add_constant(bool is_public, ::std::string name, TypeRef type, Expr val);
    void add_global(bool is_public, bool is_mut, ::std::string name, TypeRef type, Expr val);
    void add_struct(bool is_public, ::std::string name, TypeParams params, ::std::vector<StructItem> items);
    void add_function(bool is_public, Function func);
    void add_impl(Impl impl);

    void iterate_functions(fcn_visitor_t* visitor, const Crate& crate);
};

class Crate
{
    Module  m_root_module;
public:
    Crate(Module root_module):
        m_root_module(root_module)
    {
    }

    void iterate_functions( fcn_visitor_t* visitor );
};

class CStruct
{
    ::std::vector<StructItem>   m_fields;
public:
    const char* name() const { return "TODO"; }
    const char* mangled_name() const { return "TODO"; }
    const ::std::vector<StructItem>& fields() const { return m_fields; }
};

class Flat
{
    ::std::vector<CStruct>  m_structs;
    ::std::vector<Function> m_functions;
public:
    
    const ::std::vector<Function>& functions() const { return m_functions; }
    const ::std::vector<CStruct>& structs() const { return m_structs; }
};

}

#endif // AST_HPP_INCLUDED
