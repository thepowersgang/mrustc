/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/path.hpp
 * - AST::Path and helper types
 */
#ifndef AST_PATH_HPP_INCLUDED
#define AST_PATH_HPP_INCLUDED

#include "../common.hpp"
#include <string>
#include <stdexcept>
#include <vector>
#include <initializer_list>
#include <cassert>
#include <tagged_union.hpp>
#include <string>
#include "../include/span.hpp"
#include "../include/ident.hpp"
#include "lifetime_ref.hpp"
#include "types.hpp"
#include "expr_ptr.hpp"

#ifndef TYPES_HPP_COMPLETE
# error "Expected TYPES_HPP_COMPLETE set"
#endif

class MacroRules;

namespace HIR {
class Module;
class Trait;
struct TraitAlias;
class Enum;
class Struct;
class Union;
class Static;
}   // namespace HIR

namespace AST {

class LifetimeRef;
class GenericParams;
class Crate;
class Module;
class TypeAlias;
class Enum;
class Struct;
class Union;
class Trait;
class TraitAlias;
class Static;
class Function;
class ExternCrate;

struct AbsolutePath
{
    RcString    crate;
    ::std::vector<RcString>   nodes;

    AbsolutePath()
    {
    }
    AbsolutePath(RcString crate, ::std::vector<RcString> nodes)
        : crate(::std::move(crate))
        , nodes(::std::move(nodes))
    {
    }

    AbsolutePath operator+(RcString n) const {
        // Maybe being overly efficient here, but meh.
        AbsolutePath    rv;
        rv.crate = this->crate;
        rv.nodes.reserve(this->nodes.size() + 1);
        rv.nodes.insert(rv.nodes.end(), this->nodes.begin(), this->nodes.end());
        rv.nodes.push_back(::std::move(n));
        return rv;
    }

    bool operator==(const AbsolutePath& x) const {
        if( this->crate != x.crate )
            return false;
        if( this->nodes != x.nodes )
            return false;
        return true;
    }
    bool operator!=(const AbsolutePath& x) const { return !(*this == x); }

    friend ::std::ostream& operator<<(::std::ostream& os, const AbsolutePath& x) {
        if(x.crate == "") {
            os << "::\"" << x.crate << "\"";
        }
        else {
            os << "crate";
        }
        for(const auto& n : x.nodes)
            os << "::" << n;
        return os;
    }

    // Returns true if this path is a prefix of the other path (or equal)
    bool is_parent_of(const AbsolutePath& other) const {
        if(this->crate != other.crate)
            return false;
        if(this->nodes.size() > other.nodes.size())
            return false;
        for(size_t i = 0; i < this->nodes.size(); i ++)
        {
            if(this->nodes[i] != other.nodes[i]) {
                return false;
            }
        }
        return true;
    }
};

TAGGED_UNION_EX(PathBinding_Value, (), Unbound, (
    (Unbound, struct {
        }),
    (Struct, struct {
        const Struct* struct_;
        const ::HIR::Struct* hir;
        }),
    (Static, struct {
        const Static* static_;
        const ::HIR::Static* hir; // if nullptr and static_ == nullptr, points to a `const`
        }),
    (Function, struct {
        const Function* func_;
        }),
    (EnumVar, struct {
        const Enum* enum_;
        unsigned int idx;
        const ::HIR::Enum*  hir;
        }),
    (Generic, struct {
        unsigned int index;
        }),
    (Variable, struct {
        unsigned int slot;
        })
    ),
    (), (),
    (
    public:
        PathBinding_Value clone() const;
        )
    );
TAGGED_UNION_EX(PathBinding_Type, (), Unbound, (
    (Unbound, struct {
        }),
    (Crate, struct {
        const ExternCrate* crate_;
        }),
    (Module, struct {
        const Module* module_;
        struct Hir {
            const ::AST::ExternCrate* crate;
            const ::HIR::Module* mod;
        } hir;
        }),
    (Struct, struct {
        const Struct* struct_;
        const ::HIR::Struct* hir;
        }),
    (Enum,   struct {
        const Enum* enum_;
        const ::HIR::Enum*  hir;
        }),
    (Union,   struct {
        const Union* union_;
        const ::HIR::Union*  hir;
        }),
    (Trait,  struct {
        const Trait* trait_;
        const ::HIR::Trait* hir;
        }),
    (TraitAlias, struct {
        const TraitAlias* trait_;
        const ::HIR::TraitAlias* hir;
        }),

    (EnumVar, struct {
        const Enum* enum_;
        unsigned int idx;
        const ::HIR::Enum*  hir;
        }),
    (TypeAlias, struct {
        const TypeAlias* alias_;
        }),

    (TypeParameter, struct {
        unsigned int slot;
        })
    ),
    (), (),
    (
    public:
        PathBinding_Type clone() const;
        )
    );
TAGGED_UNION_EX(PathBinding_Macro, (), Unbound, (
    (Unbound, struct {
        }),
    (ProcMacroDerive, struct {
        const ExternCrate* crate_;
        RcString mac_name;
        }),
    (ProcMacroAttribute, struct {
        const ExternCrate* crate_;
        RcString mac_name;
        }),
    (ProcMacro, struct {
        const ExternCrate* crate_;
        RcString mac_name;
        }),
    (MacroRules, struct {
        const ExternCrate* crate_;  // Can be NULL
        const MacroRules* mac;
        })
    ),
    (), (),
    (
    public:
        PathBinding_Macro clone() const;
        )
    );

extern ::std::ostream& operator<<(::std::ostream& os, const PathBinding_Value& x);
extern ::std::ostream& operator<<(::std::ostream& os, const PathBinding_Type& x);
extern ::std::ostream& operator<<(::std::ostream& os, const PathBinding_Macro& x);

/// <summary>
/// Wrapper for PathBinding_* that also includes an item path
/// </summary>
/// <typeparam name="T">PathBinding_*</typeparam>
template<typename T>
struct PathBinding
{
    AbsolutePath    path;
    T   binding;

    PathBinding()
    {
    }
    PathBinding(AbsolutePath p, T b)
        : path(::std::move(p))
        , binding(::std::move(b))
    {
    }

    void set(AbsolutePath p, T b) {
        path = ::std::move(p);
        binding = ::std::move(b);
    }

    bool is_Unbound() const {
        return this->binding.is_Unbound();
    }
    PathBinding<T> clone() const {
        return PathBinding(path, binding.clone());
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const PathBinding<T>& x) {
        if(!x.is_Unbound()) {
            os << x.binding << "[" << x.path << "]";
        }
        else {
            os << "Unbound";
        }
        return os;
    }
};

class PathParamEnt;

struct PathParams
{
    ::std::vector< PathParamEnt >  m_entries;

    PathParams(PathParams&& x) = default;
    PathParams(const PathParams& x);
    PathParams() {}

    PathParams& operator=(PathParams&& x) = default;
    PathParams& operator=(const PathParams& x) = delete;

    bool is_empty() const {
        return m_entries.empty();
    }

    Ordering ord(const PathParams& x) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const PathParams& x);
};

class PathNode
{
    RcString    m_name;
    PathParams  m_params;
public:
    PathNode() {}
    PathNode(RcString name, PathParams args = {});
    const RcString& name() const { return m_name; }

    const ::AST::PathParams& args() const { return m_params; }
          ::AST::PathParams& args()       { return m_params; }

    Ordering ord(const PathNode& x) const;
    void print_pretty(::std::ostream& os, bool is_type_context) const;

    bool operator==(const PathNode& x) const { return ord(x) == OrdEqual; }
    friend ::std::ostream& operator<<(::std::ostream& os, const PathNode& pn);
};

/*
* TODO: New Path structure:
* - Local(Ident): a resolved local name (variable, generic, ...)
* - Relative(Ident, vector<RcString>, Generics): `foo::bar<...>`
* - ModRelative(unsigned, vector<RcString>, Generics): `super::foo::bar<...>` or `self::foo`
* - Absolute(RcString, vector<RcString>, Generics): `::foocrate::bar<...>`
* - FullyQualified(Type, Path, Ident, Generics): `<Foo as Bar>::baz<...>`
* - TypeQualified(Type, Ident, Generics): `<FooType>::baz<...>`
* - UnknownQualified(Path, Ident, Generics): `FooTrait<...>::baz<...>` 
* 
* Goal:
* - Reduce memory usage of AST (avoids `PathParams` everywhere)
* - Simplify manipulation?
* 
* Downsides:
* - Resolve uses append methods etc
*/
#if 0
class ItemPath
{
public:
    TAGGED_UNION(Origin, None,
        (None, struct {}),  // No prefix (TODO: Hygiene?)
        (Crate, struct {}), // crate::
        (Extern, RcString), // ::"foo"::
        (Self, struct {}),  // self::
        (Super, unsigned)   // super::super::   - Count must be non-zero
        );

    Origin  m_origin;
    std::vector<RcString>   m_nodes;

    ItemPath(Origin origin, std::vector<RcString> nodes);
    ItemPath(AbsolutePath ap, PathParams pp={});

    Ordering ord(const ItemPath& x) const;
};
class Path
{
public:
    TAGGED_UNION(Binding, Unbound,
        (Unbound, struct{}),
        (Value, PathBinding_Value),
        (Type, PathBinding_Type),
        (Macro, PathBinding_Macro)
        );

    TAGGED_UNION(Class, Invalid,
        (Invalid, struct {}),
        (Local, RcString),
        // foo::<...>
        (Item, struct {
            ItemPath    item;
            PathParams  params;
            }),
        // T::<...>::foo
        (UfcsUnknown, struct {
            ::std::unique_ptr<Path> inner;
            RcString    name;
            PathParams  params;
            }),
        // <T>::foo
        // <_ as T>::foo
        (Ufcs, struct {
            ::std::unique_ptr<TypeRef> type;    // always non-null
            ::std::unique_ptr<ItemPath> trait;  // nullptr = inherent, Invalid = unknown trait
            RcString    name;
            PathParams  params;
            })
        );

    Class m_class;
    Binding m_binding;

public:
    Path(Path&&) = default;
    Path& operator=(AST::Path&& x) = default;

    /*explicit*/ Path(const Path& x);
    Path& operator=(const AST::Path&) = delete;

    Path() {}
    Path(Class c): m_class(std::move(c)) {}


    Path(RcString name):
        m_class( Class::make_Local({ mv$(name) }) )
    {}

    // ABSOLUTE
#if 0
    Path(RcString crate, ::std::vector<RcString> nodes):
        m_class( Class::make_Absolute({ mv$(crate), mv$(nodes)}) )
    {}
#endif
    Path(const AbsolutePath& p, ::AST::PathParams pp={}):
        m_class(Class::make_Item({ p, std::move(pp) }))
    {
    }
    Path(const PathBinding<PathBinding_Value>& pb):
        Path(pb.path)
    {
        this->m_binding = pb.binding.clone();
    }
    Path(const PathBinding<PathBinding_Type>& pb):
        Path(pb.path)
    {
        this->m_binding = pb.binding.clone();
    }
    Path(const PathBinding<PathBinding_Macro>& pb):
        Path(pb.path)
    {
        this->m_binding = pb.binding.clone();
    }


    Ordering ord(const Path& x) const;
    bool operator==(const Path& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const Path& x) const { return ord(x) != OrdEqual; }
    bool operator<(const Path& x) const { return ord(x) != OrdLess; }

    void print_pretty(::std::ostream& os, bool is_type_context, bool is_debug=false) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const Path& path);
};

#else

class Path
{
public:
    TAGGED_UNION(Class, Invalid,
        (Invalid, struct {}),
        (Local, struct {   // Variable / Type param (resolved)
            RcString name;
            } ),
        (Relative, struct {    // General relative
            Ident::Hygiene hygiene;
            ::std::vector<PathNode> nodes;
            } ),
        (Self, struct {    // Module-relative
            ::std::vector<PathNode> nodes;
            } ),
        (Super, struct {   // Parent-relative
            unsigned int count; // Number of `super` keywords, must be >= 1
            ::std::vector<PathNode> nodes;
            } ),
        (Absolute, struct {    // Absolute
            RcString    crate;
            ::std::vector<PathNode> nodes;
            } ),
        (UFCS, struct {    // Type-relative
            ::std::unique_ptr<TypeRef> type;    // always non-null
            ::std::unique_ptr<Path> trait;   // nullptr = inherent, Invalid = unknown trait
            ::std::vector<PathNode> nodes;
            } )
        );
    struct Bindings {
        PathBinding<PathBinding_Value> value;
        PathBinding<PathBinding_Type> type;
        PathBinding<PathBinding_Macro> macro;

        Bindings clone() const {
            return Bindings {
                value.clone(), type.clone(), macro.clone()
            };
        }
        bool has_binding() const {
            return !value.is_Unbound() || !type.is_Unbound() || !macro.is_Unbound();
        }
        void merge_from(const Bindings& x) {
            if(value.is_Unbound())
                value = x.value.clone();
            if(type.is_Unbound())
                type = x.type.clone();
            if(macro.is_Unbound())
                macro = x.macro.clone();
        }
    };

public:
    Class   m_class;
    Bindings m_bindings;

    virtual ~Path();

    Path(Class c):
        m_class(::std::move(c))
    {}

    // INVALID
    Path():
        m_class()
    {}
    Path(Path&&) = default;
    Path& operator=(AST::Path&& x) = default;

    /*explicit*/ Path(const Path& x);
    Path& operator=(const AST::Path&) = delete;

    // ABSOLUTE
    Path(RcString crate, ::std::vector<PathNode> nodes):
        m_class( Class::make_Absolute({ mv$(crate), mv$(nodes)}) )
    {}
    Path(const AbsolutePath& p):
        m_class(Class::make_Absolute({ p.crate, {} }))
    {
        auto& n = m_class.as_Absolute().nodes;
        n.reserve(p.nodes.size());
        for(const auto& v : p.nodes)
            n.push_back(v);
    }
    Path(const PathBinding<PathBinding_Value>& pb):
        Path(pb.path)
    {
        this->m_bindings.value = pb.clone();
    }
    Path(const PathBinding<PathBinding_Type>& pb):
        Path(pb.path)
    {
        this->m_bindings.type = pb.clone();
    }
    Path(const PathBinding<PathBinding_Macro>& pb):
        Path(pb.path)
    {
        this->m_bindings.macro = pb.clone();
    }
    Path(const AbsolutePath& p, ::AST::PathParams pp):
        Path(p)
    {
        auto& n = m_class.as_Absolute().nodes;
        assert(n.size() > 0);
        n.back().args() = ::std::move(pp);
    }
    // Local (variable/type param)
    Path(RcString name):
        m_class(Class::make_Local({ mv$(name) }))
    {
    }

    // UFCS
    static Path new_ufcs_ty(TypeRef type, ::std::vector<PathNode> nodes={});
    static Path new_ufcs_trait(TypeRef type, Path trait, ::std::vector<PathNode> nodes={});

    // VARIABLE
    static Path new_local(RcString name) {
        return Path(mv$(name));
    }

    // RELATIVE
    static Path new_relative(Ident::Hygiene hygiene, ::std::vector<PathNode> nodes) {
        return Path( Class::make_Relative({ mv$(hygiene), mv$(nodes) }) );
    }
    static Path new_self(::std::vector<PathNode> nodes) {
        return Path( Class::make_Self({ mv$(nodes) }) );
    }
    static Path new_super(unsigned int count, ::std::vector<PathNode> nodes) {
        return Path( Class::make_Super({ count, mv$(nodes) }) );
    }

    Path operator+(PathNode pn) const {
        Path tmp = Path(*this);
        tmp.append( mv$(pn) );
        return tmp;
    }
    Path operator+(const RcString& s) const {
        Path tmp = Path(*this);
        tmp.append(PathNode(s, {}));
        return tmp;
    }
    Path operator+(const Path& x) const {
        return Path(*this) += x;
    }
    Path& operator+=(const Path& x);
    Path& operator+=(PathNode pn) {
        this->append(mv$(pn));
        return *this;
    }

#if 1
    void append(PathNode node) {
        assert( !m_class.is_Invalid() );
        //if( m_class.is_Invalid() )
        //    m_class = Class::make_Relative({});
        nodes().push_back( mv$(node) );
        m_bindings = Bindings();
    }
#endif

    bool is_trivial() const {
        TU_MATCH_DEF(Class, (m_class), (e),
        (
            return false;
            ),
        (Local,
            return true;
            ),
        (Relative,
            return e.nodes.size() == 1 && e.nodes[0].args().is_empty();
            )
        )
    }
    const RcString& as_trivial() const {
        TU_MATCH_HDRA( (m_class), {)
        default:
            break;
        TU_ARMA(Local, e) {
            return e.name;
            }
        TU_ARMA(Relative, e) {
            return e.nodes[0].name();
            }
        }
        throw std::runtime_error("as_trivial on non-trivial path");
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

    bool is_parent_of(const Path& x) const;

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

#if 0
    PathNode& operator[](int idx) { if(idx>=0) return nodes()[idx]; else return nodes()[size()+idx]; }
    const PathNode& operator[](int idx) const { return (*(Path*)this)[idx]; }
#endif

    Ordering ord(const Path& x) const;
    bool operator==(const Path& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const Path& x) const { return ord(x) != OrdEqual; }
    bool operator<(const Path& x) const { return ord(x) != OrdLess; }

    void print_pretty(::std::ostream& os, bool is_type_context, bool is_debug=false) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const Path& path);
private:
    static void resolve_args_nl(::std::vector<PathNode>& nodes, ::std::function<TypeRef(const char*)> fcn);

    void check_param_counts(const GenericParams& params, bool expect_params, PathNode& node);
public:
    //void bind_enum_var(const Enum& ent, const RcString& name);
    //void bind_function(const Function& ent) {
    //    m_bindings.value = PathBinding_Value::make_Function({&ent});
    //}
};
#endif

TAGGED_UNION_EX(PathParamEnt, (), Null, (
    (Null, struct {
        }),
    (Lifetime, LifetimeRef),
    (Type, TypeRef),
    (Value, AST::ExprNodeP),
    (AssociatedTyEqual, ::std::pair<RcString, TypeRef>),
    (AssociatedTyBound, ::std::pair<RcString, Path>)
    ),
    (), (),
    (
    public:
        PathParamEnt clone() const;
        Ordering ord(const PathParamEnt& x) const;
        void fmt(::std::ostream& os) const;
        )
    );

}   // namespace AST

#define AST_PATH_HPP_COMPLETE

#endif
