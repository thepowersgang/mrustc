/*
 */
#ifndef AST_PATH_HPP_INCLUDED
#define AST_PATH_HPP_INCLUDED

#include "../common.hpp"
#include <string>
#include <stdexcept>
#include <vector>
#include <initializer_list>
#include <cassert>
#include <serialise.hpp>
#include <tagged_enum.hpp>

class TypeRef;

namespace AST {

class Crate;
class Module;
class TypeAlias;
class Enum;
class Struct;
class Trait;
class Static;
class Function;

//TAGGED_ENUM(Binding, Unbound,
//    (BndModule, (const Module* module_; ) ),
//    (BndEnum,   (const Enum* enum_; ) ),
//    (BndStruct, (const Struct* struct_; ) ),
//    (BndTrait,  (const Trait* trait_; ) ),
//    (BndStatic, (const Static* static_; ) ),
//    (BndFunction, (const Function* func_; ) ),
//    (BndEnumVar, (const Enum* enum_; unsigned int idx; ) ),
//    (BndTypeAlias, (const TypeAlias* alias_; ) ),
//    (BndStructMethod, (const Struct* struct_; ::std::string name; ) ),
//    (BndTraitMethod, (const Trait* struct_; ::std::string name; ) )
//    );
class PathBinding
{
public:
    enum BindingType {
        UNBOUND,
        MODULE,
        ALIAS,
        ENUM,
        STRUCT,
        TRAIT,
        
        STRUCT_METHOD,
        TRAIT_METHOD,
        ENUM_VAR,
        FUNCTION,
        STATIC,
    };
    struct EnumVar {
        const Enum* enum_;
        unsigned int idx;
    };
private:
    BindingType m_binding_type = UNBOUND;
    union {
        const Module* module_;
        const Enum* enum_;
        const Struct*   struct_;
        struct {
            const Struct* struct_;
            unsigned int idx;
        } structitem;
        const Trait*    trait_;
        const Static*   static_;
        const Function* func_;
        EnumVar enumvar_;
        const TypeAlias*    alias_;
    } m_binding;

public:
    PathBinding(): m_binding_type(UNBOUND) {}
    
    bool is_bound() const { return m_binding_type != UNBOUND; }
    BindingType type() const { return m_binding_type; }
    #define _(t, n, v)\
        PathBinding(const t* i): m_binding_type(v) { m_binding.n##_ = i; } \
        const t& bound_##n() const { assert(m_binding_type == v); return *m_binding.n##_; }
    _(Module, module, MODULE)
    _(Trait,  trait,  TRAIT)
    _(Struct, struct, STRUCT)
    _(Enum,   enum,   ENUM)
    _(Function, func, FUNCTION)
    _(Static, static, STATIC)
    _(TypeAlias, alias, ALIAS)
    //_(EnumVar, enumvar, ENUM_VAR)
    #undef _
    PathBinding(const Enum* enm, unsigned int i):
        m_binding_type(ENUM_VAR)
    {
        m_binding.enumvar_ = {enm, i};
    }
    const EnumVar& bound_enumvar() const { assert(m_binding_type == ENUM_VAR); return m_binding.enumvar_; }
    
    struct TagItem {};
    PathBinding(TagItem, const Trait* t): m_binding_type(TRAIT_METHOD) { m_binding.trait_ = t; }
    PathBinding(TagItem, const Struct* i): m_binding_type(STRUCT_METHOD) { m_binding.struct_ = i; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const PathBinding& x) {
        switch(x.m_binding_type)
        {
        case UNBOUND:   os << "UNBOUND";    break;
        case MODULE:    os << "Module"; break;
        case TRAIT:     os << "Trait";  break;
        case STRUCT:    os << "Struct"; break;
        case ENUM:      os << "Enum";   break;
        case FUNCTION:  os << "Function";break;
        case STATIC:    os << "Static"; break;
        case ALIAS:     os << "Alias";  break;
        case STRUCT_METHOD: os << "StructMethod";   break;
        case TRAIT_METHOD:  os << "TraitMethod";    break;
        case ENUM_VAR:  os << "EnumVar(" << x.m_binding.enumvar_.idx << ")";  break;
        }
        return os;
    }
};

class PathNode:
    public ::Serialisable
{
    ::std::string   m_name;
    ::std::vector<TypeRef>  m_params;
public:
    PathNode() {}
    PathNode(::std::string name, ::std::vector<TypeRef> args = {});
    const ::std::string& name() const;
    ::std::vector<TypeRef>&   args() { return m_params; }
    const ::std::vector<TypeRef>&   args() const;
    
    Ordering ord(const PathNode& x) const;
    bool operator==(const PathNode& x) const { return ord(x) == OrdEqual; }
    void print_pretty(::std::ostream& os) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const PathNode& pn);
    
    SERIALISABLE_PROTOTYPES();
};

class Path:
    public ::Serialisable
{
private:
    enum Class {
        RELATIVE,
        ABSOLUTE,
        LOCAL,
        UFCS,
    };
    
    /// The crate defining the root of this path (used for path resolution)
    ::std::string   m_crate;

    /// Path class (absolute, relative, local)
    /// - Absolute is "relative" to the crate root
    /// - Relative doesn't have a set crate (and can't be resolved)
    /// - Local is a special case to handle possible use of local varaibles
    /// - UFCS is relative to a type
    Class   m_class;
    ::std::vector<TypeRef>  m_ufcs;
    ::std::vector<PathNode> m_nodes;
    
    PathBinding m_binding;
public:
    Path():
        m_class(RELATIVE)
    {}
    struct TagAbsolute {};
    Path(TagAbsolute):
        m_class(ABSOLUTE)
    {}
    struct TagUfcs {};
    Path(TagUfcs, TypeRef type, TypeRef trait);
    struct TagLocal {};
    Path(TagLocal, ::std::string name):
        m_class(LOCAL),
        m_nodes({PathNode(name, {})})
    {}
    struct TagSuper {};
    Path(TagSuper):
        m_class(RELATIVE),
        m_nodes({PathNode("super", {})})
    {}
    
    Path(::std::initializer_list<PathNode> l):
        m_class(ABSOLUTE),
        m_nodes(l)
    {}
    Path(::std::string crate, ::std::vector<PathNode> nodes):
        m_crate( ::std::move(crate) ),
        m_class(ABSOLUTE),
        m_nodes( ::std::move(nodes) )
    {}
    
    void set_crate(::std::string crate) {
        if( m_crate == "" ) {
            m_crate = crate;
            DEBUG("crate set to " << m_crate);
        }
    }
    void set_local() {
        assert(m_class == RELATIVE);
        m_class = LOCAL;
    }
    
    /// Add the all nodes except the first from 'b' to 'a' and return
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        ret.add_tailing(b);
        return ret;
    }
    /// Grab the args from the first node of b, and add the rest to the end of the path
    void add_tailing(const Path& b) {
        if( m_nodes.size() > 0 )
            m_nodes.back().args() = b[0].args();
        else if( b[0].args().size() > 0 )
            throw ::std::runtime_error("add_tail to empty path, but generics in source");
        else
            ;
        for(unsigned int i = 1; i < b.m_nodes.size(); i ++)
            m_nodes.push_back(b.m_nodes[i]);
        m_binding = PathBinding();
    }
    Path operator+(PathNode&& pn) const {
        Path    tmp;
        tmp.append( ::std::move(pn) );
        return Path(*this) += tmp;
    }
    Path operator+(const ::std::string& s) const {
        Path    tmp;
        tmp.append(PathNode(s, {}));
        return Path(*this) += tmp;
    }
    Path operator+(const Path& x) const {
        return Path(*this) += x;
    }
    Path& operator+=(const Path& x);

    void append(PathNode node) {
        m_nodes.push_back(node);
        m_binding = PathBinding();
    }
    
    void resolve(const Crate& crate);
    
    bool is_trivial() const {
        return m_class == RELATIVE && m_nodes.size() == 1 && m_nodes[0].args().size() == 0;
    }
    
    bool is_absolute() const { return m_class == ABSOLUTE; }
    bool is_relative() const { return m_class == RELATIVE; }
    size_t size() const { return m_nodes.size(); }
    
    const PathBinding& binding() const { return m_binding; }
    
    ::std::vector<PathNode>& nodes() { return m_nodes; }
    const ::std::vector<PathNode>& nodes() const { return m_nodes; }
    
    PathNode& operator[](int idx) { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
    const PathNode& operator[](int idx) const { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
   
    Ordering ord(const Path& x) const;
    bool operator==(const Path& x) const { return ord(x) == OrdEqual; }
    
    SERIALISABLE_PROTOTYPES(); 
    void print_pretty(::std::ostream& os) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const Path& path);
    friend ::Serialiser& operator<<(Serialiser& s, Path::Class pc);
    friend void operator>>(Deserialiser& s, Path::Class& pc);
private:
    void bind_module(const Module& mod);
    void bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args);
    void bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args);
    void bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args);
    void bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node);
    void bind_static(const Static& ent);
};

}   // namespace AST

#endif
