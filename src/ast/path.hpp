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
#include <tagged_union.hpp>
#include <string>
#include "../include/span.hpp"

class TypeRef;

namespace AST {

class GenericParams;
class Crate;
class Module;
class TypeAlias;
class Enum;
class Struct;
class Trait;
class Static;
class Function;

TAGGED_UNION(PathBinding, Unbound,
    (Unbound, (
        )),
    (Module, (
        const Module* module_;
        )),
    (Enum,   (
        const Enum* enum_;
        )),
    (Struct, (
        const Struct* struct_;
        )),
    (Trait,  (
        const Trait* trait_;
        )),
    (Static, (
        const Static* static_;
        )),
    (Function, (
        const Function* func_;
        )),
    (EnumVar, (
        const Enum* enum_;
        unsigned int idx;
        )),
    (TypeAlias, (
        const TypeAlias* alias_;
        )),
    (StructMethod, (
        const Struct* struct_;
        ::std::string name;
        )),
    (TraitMethod, (
        const Trait* struct_;
        ::std::string name;
        )),

    (TypeParameter, (
        unsigned int level;
        unsigned int idx;
        )),
    (Variable, (
        unsigned int slot;
        ))
    );

extern ::std::ostream& operator<<(::std::ostream& os, const PathBinding& x);


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
public:
    TAGGED_UNION(Class, Invalid,
        (Invalid, ()),
        (Local, (   // Variable / Type param (resolved)
            ::std::string name;
            ) ),
        (Relative, (    // General relative
            ::std::vector<PathNode> nodes;
            ) ),
        (Self, (    // Module-relative
            ::std::vector<PathNode> nodes;
            ) ),
        (Super, (   // Parent-relative
            ::std::vector<PathNode> nodes;
            ) ),
        (Absolute, (    // Absolute
            ::std::vector<PathNode> nodes;
            ) ),
        (UFCS, (    // Type-relative
            ::std::unique_ptr<TypeRef> type;
            ::std::unique_ptr<TypeRef> trait;
            ::std::vector<PathNode> nodes;
            ) )
        );
    
private:
    /// The crate defining the root of this path (used for path resolution)
    ::std::string   m_crate;

public:
    Class   m_class;

private:
    PathBinding m_binding;
public:
    // INVALID
    Path():
        m_class()
    {}
    Path(Path&&) noexcept = default;
    Path& operator=(AST::Path&& x) {
        m_crate = mv$(x.m_crate);
        m_class = mv$(x.m_class);
        m_binding = mv$(x.m_binding);
        //DEBUG("Path, " << x);
        return *this;
    }
    
    Path(const Path& x);
    
    // ABSOLUTE
    Path(::std::string crate, ::std::vector<PathNode> nodes):
        m_crate( ::std::move(crate) ),
        m_class( Class::make_Absolute({nodes: mv$(nodes)}) )
    {}
    
    // UFCS
    struct TagUfcs {};
    // TODO: Replace with "Path(TagUfcs, TypeRef, Path, PathNode)" and "Path(TagUfcs, TypeRef, PathNode)"
    // - Making Class::UFCS.trait be a nullable Path pointer
    Path(TagUfcs, TypeRef type, TypeRef trait, ::std::vector<PathNode> nodes={});
    
    // VARIABLE
    struct TagLocal {};
    Path(TagLocal, ::std::string name):
        m_class( Class::make_Local({ mv$(name) }) )
    {}
    Path(::std::string name):
        m_class( Class::make_Local({name: mv$(name)}) )
    {}
    
    // RELATIVE
    struct TagRelative {};
    Path(TagRelative, ::std::vector<PathNode> nodes):
        m_class( Class::make_Relative({nodes: mv$(nodes)}) )
    {}
    // SELF
    struct TagSelf {};
    Path(TagSelf, ::std::vector<PathNode> nodes):
        m_class( Class::make_Self({nodes: nodes}) )
    {}
    // SUPER
    struct TagSuper {};
    Path(TagSuper, ::std::vector<PathNode> nodes):
        m_class( Class::make_Super({nodes: nodes}) )
    {}
    
    void set_crate(::std::string crate) {
        if( m_crate == "" ) {
            m_crate = crate;
            DEBUG("crate set to " << m_crate);
        }
    }

    
    Class::Tag class_tag() const {
        return m_class.tag();
    }
    
    /// Add the all nodes except the first from 'b' to 'a' and return
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        ret.add_tailing(b);
        return ret;
    }
    /// Grab the args from the first node of b, and add the rest to the end of the path
    // TODO: Args should probably be moved to the path, not the nodes
    void add_tailing(const Path& b) {
        assert( !this->m_class.is_Invalid() );
        assert( b.m_class.is_Relative() );
        const auto& b_r = b.m_class.as_Relative();
        if( b_r.nodes.size() == 0 )
            ;
        else if( nodes().size() > 0 )
            nodes().back().args() = b[0].args();
        else if( b[0].args().size() > 0 )
            throw ::std::runtime_error("add_tail to empty path, but generics in source");
        else
            ;
        for(unsigned int i = 1; i < b_r.nodes.size(); i ++)
            nodes().push_back(b_r.nodes[i]);
        m_binding = PathBinding();
    }
    Path operator+(PathNode&& pn) const {
        Path tmp = Path(*this);
        tmp.nodes().push_back( pn );
        return tmp;
    }
    Path operator+(const ::std::string& s) const {
        Path tmp = Path(*this);
        tmp.append(PathNode(s, {}));
        return tmp;
    }
    Path operator+(const Path& x) const {
        return Path(*this) += x;
    }
    Path& operator+=(const Path& x);

    void append(PathNode node) {
        if( m_class.is_Invalid() )
            m_class = Class::make_Relative({});
        nodes().push_back(node);
        m_binding = PathBinding();
    }

    /// Resolve generic arguments within the path
    void resolve_args(::std::function<TypeRef(const char*)> fcn);
    
    /// Match args
    void match_args(const Path& other, ::std::function<void(const char*,const TypeRef&)> fcn) const;
    
    bool is_trivial() const {
        TU_MATCH_DEF(Class, (m_class), (e),
        (
            return false;
            ),
        (Local,
            return true;
            ),
        (Relative,
            return e.nodes.size() == 1 && e.nodes[0].args().size() == 0;
            )
        )
    }
    
    bool is_valid() const { return !m_class.is_Invalid(); }
    bool is_absolute() const { return m_class.is_Absolute(); }
    bool is_relative() const { return m_class.is_Relative() || m_class.is_Super() || m_class.is_Self(); }
    
    size_t size() const {
        TU_MATCH(Class, (m_class), (ent),
        (Invalid,  assert(!m_class.is_Invalid()); throw ::std::runtime_error("Path::nodes() on Invalid"); ),
        (Local,    return 1;),
        (Relative, return ent.nodes.size();),
        (Self,     return ent.nodes.size();),
        (Super,    return ent.nodes.size();),
        (Absolute, return ent.nodes.size();),
        (UFCS,     return ent.nodes.size();)
        )
        throw ::std::runtime_error("Path::nodes() fell off");
    }
    const ::std::string& crate() const { return m_crate; }

    bool is_concrete() const;
    
    bool is_bound() const { return !m_binding.is_Unbound(); }
    const PathBinding& binding() const { return m_binding; }
    void bind_variable(unsigned int slot);
    
    ::std::vector<PathNode>& nodes() {
        TU_MATCH(Class, (m_class), (ent),
        (Invalid,  assert(!m_class.is_Invalid()); throw ::std::runtime_error("Path::nodes() on Invalid"); ),
        (Local,    assert(!m_class.is_Local()); throw ::std::runtime_error("Path::nodes() on Local"); ),
        (Relative, return ent.nodes;),
        (Self,     return ent.nodes;),
        (Super,    return ent.nodes;),
        (Absolute, return ent.nodes;),
        (UFCS,     return ent.nodes;)
        )
        throw ::std::runtime_error("Path::nodes() fell off");
    }
    const ::std::vector<PathNode>& nodes() const {
        return ((Path*)this)->nodes();
    }
    
    PathNode& operator[](int idx) { if(idx>=0) return nodes()[idx]; else return nodes()[size()+idx]; }
    const PathNode& operator[](int idx) const { return (*(Path*)this)[idx]; }
   
    /// Returns 0 if paths are identical, 1 if TypeRef::TagArg is present in one, and -1 if a node differs
    int equal_no_generic(const Path& x) const;
    
    Ordering ord(const Path& x) const;
    bool operator==(const Path& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const Path& x) const { return ord(x) != OrdEqual; }
    bool operator<(const Path& x) const { return ord(x) != OrdLess; }
    
    SERIALISABLE_PROTOTYPES(); 
    void print_pretty(::std::ostream& os) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const Path& path);
    friend ::Serialiser& operator<<(Serialiser& s, Path::Class pc);
    friend void operator>>(Deserialiser& s, Path::Class& pc);
private:
    static void resolve_args_nl(::std::vector<PathNode>& nodes, ::std::function<TypeRef(const char*)> fcn);
    static void match_args_nl(const ::std::vector<PathNode>& nodes_a, const ::std::vector<PathNode>& nodes_b, ::std::function<void(const char*,const TypeRef&)> fcn);
    static int node_lists_equal_no_generic(const ::std::vector<PathNode>& nodes_a, const ::std::vector<PathNode>& nodes_b);
    
    void check_param_counts(const GenericParams& params, bool expect_params, PathNode& node);
public:
    void bind_module(const Module& mod);
    void bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args={});
    void bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args={});
    void bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args={});
    void bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node);
    void bind_static(const Static& ent);
    void bind_trait(const Trait& ent, const ::std::vector<TypeRef>& args={});
    void bind_function(const Function& ent, const ::std::vector<TypeRef>& args={}) {
        m_binding = PathBinding::make_Function({&ent});
    }
    void bind_type_alias(const TypeAlias& ent, const ::std::vector<TypeRef>& args={}) {
        m_binding = PathBinding::make_TypeAlias({&ent});
    }
};

}   // namespace AST

#endif
