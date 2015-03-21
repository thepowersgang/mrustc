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
    
    bool operator==(const PathNode& x) const;
    void print_pretty(::std::ostream& os) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const PathNode& pn);
    
    SERIALISABLE_PROTOTYPES();
};

class Path:
    public ::Serialisable
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
private:
    enum Class {
        RELATIVE,
        ABSOLUTE,
        LOCAL,
    };
    
    /// The crate defining the root of this path (used for path resolution)
    ::std::string   m_crate;

    /// Path class (absolute, relative, local)
    /// - Absolute is "relative" to the crate root
    /// - Relative doesn't have a set crate (and can't be resolved)
    /// - Local is a special case to handle possible use of local varaibles
    Class   m_class;
    ::std::vector<PathNode> m_nodes;
    
    BindingType m_binding_type = UNBOUND;
    union {
        const Module* module_;
        const Enum* enum_;
        const Struct*   struct_;
        const Trait*    trait_;
        const Static*   static_;
        const Function* func_;
        struct {
            const Enum* enum_;
            unsigned int idx;
        } enumvar;
        const TypeAlias*    alias_;
    } m_binding;
public:
    Path():
        m_class(RELATIVE)
    {}
    struct TagAbsolute {};
    Path(TagAbsolute):
        m_class(ABSOLUTE)
    {}
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
    
    /// Add the all nodes except the first from 'b' to 'a' and return
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        ret.add_tailing(b);
        return ret;
    }
    /// Grab the args from the first node of b, and add the rest to the end of the path
    void add_tailing(const Path& b) {
        m_nodes.back().args() = b[0].args();
        for(unsigned int i = 1; i < b.m_nodes.size(); i ++)
            m_nodes.push_back(b.m_nodes[i]);
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
    }
    
    void resolve(const Crate& crate);
    
    bool is_absolute() const { return m_class == ABSOLUTE; }
    bool is_relative() const { return m_class == RELATIVE; }
    size_t size() const { return m_nodes.size(); }
    
    bool is_bound() const { return m_binding_type != UNBOUND; }
    BindingType binding_type() const { return m_binding_type; }
    #define _(t, n, v)  const t& bound_##n() const { assert(m_binding_type == v); return *m_binding.n##_; }
    _(Module, module, MODULE)
    _(Trait,  trait,  TRAIT)
    _(Struct, struct, STRUCT)
    //_(Enum,   enum,   ENUM)
    _(Function, func, FUNCTION)
    _(Static, static, STATIC)
    _(TypeAlias, alias, ALIAS)
    #undef _
    const Enum& bound_enum() const {
        assert(m_binding_type == ENUM || m_binding_type == ENUM_VAR);  // Kinda evil, given that it has its own union entry
        return *m_binding.enum_;
    }
    const unsigned int bound_idx() const {
        assert(m_binding_type == ENUM_VAR);
        return m_binding.enumvar.idx; 
    }
    
    ::std::vector<PathNode>& nodes() { return m_nodes; }
    const ::std::vector<PathNode>& nodes() const { return m_nodes; }
    
    PathNode& operator[](int idx) { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
    const PathNode& operator[](int idx) const { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
   
    bool operator==(const Path& x) const;
    
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
    void bind_static(const Static& ent);
};

}   // namespace AST

#endif
