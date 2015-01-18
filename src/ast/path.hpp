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
        ENUM_VAR,
        FUNCTION,
        STATIC,
    };
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
        const Trait*    trait;
        const Static*   static_;
        const Function* func_;
        struct {
            const Enum* enum_;
            unsigned int idx;
        } enumvar;
        const TypeAlias*    alias;
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
    
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        ret[ret.size()-1].args() = b[0].args();
        for(unsigned int i = 1; i < b.m_nodes.size(); i ++)
            ret.m_nodes.push_back(b.m_nodes[i]);
        return ret;
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
    const Module& bound_module() const { assert(m_binding_type == MODULE); return *m_binding.module_; }
    const Trait& bound_trait() const { assert(m_binding_type == TRAIT); return *m_binding.trait; }
    
    ::std::vector<PathNode>& nodes() { return m_nodes; }
    const ::std::vector<PathNode>& nodes() const { return m_nodes; }
    
    PathNode& operator[](size_t idx) { return m_nodes[idx]; }
    const PathNode& operator[](size_t idx) const { return m_nodes[idx]; }
   
    SERIALISABLE_PROTOTYPES(); 
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
