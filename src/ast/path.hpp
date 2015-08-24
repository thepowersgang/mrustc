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

class TypeParams;
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

//TAGGED_ENUM(Class, Local,
//    (Local, (::std:string name) ),
//    (Variable, (::std:string name) ),
//    (Relative, (::std::vector<PathNode> nodes) ),
//    (Self, (::std::vector<PathNode> nodes) ),
//    (Super, (::std::vector<PathNode> nodes) ),
//    (Absolute, (::std::vector<PathNode> nodes) ),
//    (UFCS, (TypeRef type; TypeRef trait; ::std::vector<PathNode> nodes) ),
//    );
class Path:
    public ::Serialisable
{
public:
    enum Class {
        INVALID,    // An empty path, usually invalid
        ABSOLUTE,   // root-relative path ("::path")
        UFCS,   // type-relative path ("<Type>::such")
        VARIABLE,   // Reference to a local variable
        
        RELATIVE,   // Unadorned relative path (e.g. "path::to::item" or "generic_item::<>")
        SELF,   // module-relative path ("self::path")
        SUPER,  // parent-relative path ("super::path")
    };
    
private:
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
    // INVALID
    Path():
        m_class(INVALID)
    {}
    
    // ABSOLUTE
    struct TagAbsolute {};
    Path(TagAbsolute):
        m_class(ABSOLUTE)
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
    
    // UFCS
    struct TagUfcs {};
    Path(TagUfcs, TypeRef type, TypeRef trait);
    
    // VARIABLE
    struct TagVariable {};
    Path(TagVariable, ::std::string name):
        m_class(VARIABLE),
        m_nodes( {PathNode( ::std::move(name), {} )} )
    {}
    
    // RELATIVE
    struct TagRelative {};
    Path(TagRelative):
        m_class(RELATIVE),
        m_nodes({})
    {}
    Path(::std::string name):
        m_class(RELATIVE),
        m_nodes( {PathNode( ::std::move(name), {} )} )
    {}
    // SELF
    struct TagSelf {};
    Path(TagSelf):
        m_class(SELF),
        m_nodes({})
    {}
    // SUPER
    struct TagSuper {};
    Path(TagSuper):
        m_class(SUPER),
        m_nodes({})
    {}
    
    void set_crate(::std::string crate) {
        if( m_crate == "" ) {
            m_crate = crate;
            DEBUG("crate set to " << m_crate);
        }
    }
    void set_local() {
        assert(m_class == RELATIVE);
    }
    
    /// Add the all nodes except the first from 'b' to 'a' and return
    static Path add_tailing(const Path& a, const Path& b) {
        Path    ret(a);
        ret.add_tailing(b);
        return ret;
    }
    /// Grab the args from the first node of b, and add the rest to the end of the path
    void add_tailing(const Path& b) {
        assert(this->m_class != INVALID);
        assert(b.m_class != INVALID);
        if( b.m_nodes.size() == 0 )
            ;
        else if( m_nodes.size() > 0 )
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
        Path tmp = Path(TagRelative());
        tmp.append( ::std::move(pn) );
        return Path(*this) += tmp;
    }
    Path operator+(const ::std::string& s) const {
        Path tmp = Path(TagRelative());
        tmp.append(PathNode(s, {}));
        return Path(*this) += tmp;
    }
    Path operator+(const Path& x) const {
        return Path(*this) += x;
    }
    Path& operator+=(const Path& x);

    void append(PathNode node) {
        assert(this->m_class != INVALID);
        assert(this->m_class != VARIABLE);
        m_nodes.push_back(node);
        m_binding = PathBinding();
    }
    
    /// Resolve the path, and set up binding
    ///
    /// expect_params enables checking of param counts (clear for handling 'use')
    void resolve(const Crate& crate, bool expect_params=true);
    void resolve_absolute(const Crate& root_crate, bool expect_params);
    void resolve_ufcs(const Crate& root_crate, bool expect_params);
    void resolve_ufcs_trait(const AST::Path& trait_path, AST::PathNode& node);
    
    /// Resolve generic arguments within the path
    void resolve_args(::std::function<TypeRef(const char*)> fcn);
    
    /// Match args
    void match_args(const Path& other, ::std::function<void(const char*,const TypeRef&)> fcn) const;
    
    bool is_trivial() const {
        switch(m_class)
        {
        case RELATIVE:  return m_nodes.size() == 1 && m_nodes[0].args().size() == 0;
        default:    return false;
        }
    }
    
    bool is_valid() const { return *this != Path(); }
    Class type() const { return m_class; }
    bool is_absolute() const { return m_class == ABSOLUTE; }
    bool is_relative() const { return m_class == RELATIVE; }
    size_t size() const { return m_nodes.size(); }

    bool is_concrete() const;
    
    const PathBinding& binding() const { return m_binding; }
    
    ::std::vector<TypeRef>& ufcs() { return m_ufcs; }
    
    ::std::vector<PathNode>& nodes() { return m_nodes; }
    const ::std::vector<PathNode>& nodes() const { return m_nodes; }
    
    PathNode& operator[](int idx) { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
    const PathNode& operator[](int idx) const { if(idx>=0) return m_nodes[idx]; else return m_nodes[size()+idx]; }
   
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
    void check_param_counts(const TypeParams& params, bool expect_params, PathNode& node);
    void bind_module(const Module& mod);
    void bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args);
    void bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args);
    void bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args);
    void bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node);
    void bind_static(const Static& ent);
};

}   // namespace AST

#endif
