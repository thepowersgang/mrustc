/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* hir/type.hpp
* - HIR Type representation
*/
#pragma once

#include <rc_string.hpp>
#include <span.hpp>

namespace HIR {

class TypeRef;

struct GenericRef;
struct SimplePath;
class Path;
class ConstGeneric;

class ExprPtr;
struct ExprNode_Closure;
struct ExprNode_Generator;

enum Compare {
    Equal,
    Fuzzy,
    Unequal,
};

typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> t_cb_resolve_type;

class MatchGenerics
{
public:
    //virtual const HIR::TypeRef& resolve_infer(const HIR::TypeRef& ty) const { return ty; }
    //virtual const HIR::TypeRef& resolve_generic(const HIR::TypeRef& ty) const { return ty; }
    virtual ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, t_cb_resolve_type resolve_cb) = 0;
    virtual ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) = 0;
};


enum class InferClass
{
    None,
    Integer,
    Float,
};

enum class CoreType;
enum class BorrowType;
class TypeData;
class TypeInner;
struct FunctionType;
class TypePathBinding;

class TypeRef
{
    TypeInner* m_ptr;
public:
    TypeRef(TypeData d);
    TypeRef();
    explicit TypeRef(const TypeRef& x);
    TypeRef(TypeRef&& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr = nullptr;
    }
    ~TypeRef();

    TypeRef& operator=(const TypeRef& x) = delete;
    TypeRef& operator=(TypeRef&& x) { this->~TypeRef(); this->m_ptr = x.m_ptr; x.m_ptr = nullptr; return *this; }

    const TypeData& data() const;
    TypeData& data_mut();
    TypeData& get_unique();



    TypeRef(::HIR::CoreType ct);
    TypeRef(RcString name, unsigned int slot);
    TypeRef(::std::vector< ::HIR::TypeRef> sts);
    TypeRef(FunctionType ft);

    static TypeRef new_unit();
    static TypeRef new_diverge();
    static TypeRef new_infer(unsigned int idx = ~0u, InferClass ty_class = InferClass::None);
    static TypeRef new_borrow(BorrowType bt, TypeRef inner);
    static TypeRef new_pointer(BorrowType bt, TypeRef inner);
    static TypeRef new_tuple(::std::vector< ::HIR::TypeRef> sts) { return TypeRef(mv$(sts)); }
    static TypeRef new_slice(TypeRef inner);
    static TypeRef new_array(TypeRef inner, uint64_t size);
    static TypeRef new_array(TypeRef inner, ::HIR::ConstGeneric size_expr);
    static TypeRef new_path(::HIR::Path path, TypePathBinding binding);
    static TypeRef new_closure(::HIR::ExprNode_Closure* node_ptr, ::std::vector< ::HIR::TypeRef> args, ::HIR::TypeRef rv);
    static TypeRef new_generator(::HIR::ExprNode_Generator* node_ptr);

    // Duplicate refcount
    TypeRef clone() const;
    // Duplicate data, inner refcount
    TypeRef clone_shallow() const;
    /// Duplicate recursively
    //TypeRef clone_deep() const;
    void fmt(::std::ostream& os) const;

    bool operator==(const ::HIR::TypeRef& x) const;
    bool operator!=(const ::HIR::TypeRef& x) const { return !(*this == x); }
    bool operator<(const ::HIR::TypeRef& x) const { return ord(x) == OrdLess; }
    Ordering ord(const ::HIR::TypeRef& x) const;


    //void match_generics(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, MatchGenerics& callback) const;
    bool match_test_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, MatchGenerics& callback) const;
    // Compares this type with another, calling the first callback to resolve placeholders in the other type, and the second callback for generics in this type
    ::HIR::Compare match_test_generics_fuzz(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, MatchGenerics& callback) const;

    // Compares this type with another, using `resolve_placeholder` to get replacements for generics/infers in `x`
    Compare compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const;

    const ::HIR::SimplePath* get_sort_path() const;
};

}