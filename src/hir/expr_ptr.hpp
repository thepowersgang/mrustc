/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr_ptr.hpp
 * - HIR Expression
 */
#pragma once
#include <memory>
#include <vector>
#include <cassert>

#include <mir/mir_ptr.hpp>

struct Span;

namespace HIR {

class TypeRef;
class ExprNode;
class Crate;
class ExprState;

class ExprPtrInner
{
    ::HIR::ExprNode* ptr;
public:
    ExprPtrInner():
        ptr(nullptr)
    {}
    ExprPtrInner(::std::unique_ptr< ::HIR::ExprNode> _);
    ExprPtrInner(ExprPtrInner&& x):
        ptr(x.ptr)
    {
        x.ptr = nullptr;
    }
    ~ExprPtrInner();

    ExprPtrInner& operator=(ExprPtrInner&& x)
    {
        this->~ExprPtrInner();
        ptr = x.ptr;
        x.ptr = nullptr;
        return *this;
    }

    ::std::unique_ptr< ::HIR::ExprNode> into_unique();
    operator bool () const { return ptr != nullptr; }
    ::HIR::ExprNode* get() const { return ptr; }
    void reset(::HIR::ExprNode* p) {
        this->~ExprPtrInner();
        this->ptr = p;
    }

          ::HIR::ExprNode& operator*()       { assert(ptr); return *ptr; }
    const ::HIR::ExprNode& operator*() const { assert(ptr); return *ptr; }
          ::HIR::ExprNode* operator->()       { assert(ptr); return ptr; }
    const ::HIR::ExprNode* operator->() const { assert(ptr); return ptr; }
};
class ExprStatePtr
{
    ::HIR::ExprState*   ptr;
public:
    ExprStatePtr(): ptr(nullptr) {}
    ExprStatePtr(ExprState );
    ExprStatePtr(const ExprStatePtr&) = delete;
    ExprStatePtr(ExprStatePtr&& x): ptr(x.ptr) { x.ptr = nullptr; }
    ~ExprStatePtr();

    ExprStatePtr& operator=(const ExprStatePtr&) = delete;
    ExprStatePtr& operator=(ExprStatePtr&& x) { this->~ExprStatePtr(); ptr = x.ptr; x.ptr = nullptr; return *this; }

    operator bool () const { return ptr != nullptr; }

    ExprStatePtr clone() const;

          ::HIR::ExprState& operator*()       { assert(ptr); return *ptr; }
    const ::HIR::ExprState& operator*() const { assert(ptr); return *ptr; }
          ::HIR::ExprState* operator->()       { assert(ptr); return ptr; }
    const ::HIR::ExprState* operator->() const { assert(ptr); return ptr; }
};

class ExprPtr
{
    //::HIR::Path m_path;
    ::HIR::ExprPtrInner node;


public:
    //::std::vector< ::HIR::TypeRef>  m_type_table;
    ::std::vector< ::HIR::TypeRef>  m_bindings;
    ::std::vector< ::HIR::TypeRef>  m_erased_types;

    // Public because too much relies on access to it
    ::MIR::FunctionPointer  m_mir;

    ::HIR::ExprStatePtr m_state;

public:
    ExprPtr() {}
    ExprPtr(::std::unique_ptr< ::HIR::ExprNode> _);
    ExprPtr(const ExprPtr&) = delete;
    ExprPtr(ExprPtr&&) = default;
    ExprPtr& operator=(ExprPtr&&) = default;

    /// Take the innards and turn into a unique_ptr - used so typecheck can edit the root node.
    ::std::unique_ptr< ::HIR::ExprNode> into_unique();
    operator bool () const { return node; }
    ::HIR::ExprNode* get() const { return node.get(); }
    void reset(::HIR::ExprNode* p) { node.reset(p); }

    const Span& span() const;
          ::HIR::ExprNode& operator*()       { return *node; }
    const ::HIR::ExprNode& operator*() const { return *node; }
          ::HIR::ExprNode* operator->()       { return &*node; }
    const ::HIR::ExprNode* operator->() const { return &*node; }

    //void ensure_typechecked(const ::HIR::Crate& crate) const;
    /// Get MIR (checks if the MIR should be available)
    const ::MIR::Function* get_mir_opt() const;
    const ::MIR::Function& get_mir_or_error(const Span& sp) const;
          ::MIR::Function& get_mir_or_error_mut(const Span& sp);
    /// Get external MIR, returns nullptr if none
    const ::MIR::Function* get_ext_mir() const;
          ::MIR::Function* get_ext_mir_mut();

    void set_mir(::MIR::FunctionPointer mir);
};

}   // namespace HIR
