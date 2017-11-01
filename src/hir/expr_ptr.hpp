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

namespace HIR {

class TypeRef;
class ExprNode;

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

class ExprPtr
{
    ::HIR::ExprPtrInner node;

public:
    ::std::vector< ::HIR::TypeRef>  m_bindings;
    ::std::vector< ::HIR::TypeRef>  m_erased_types;
    ::MIR::FunctionPointer  m_mir;

public:
    ExprPtr() {}
    ExprPtr(::std::unique_ptr< ::HIR::ExprNode> _);

    ::std::unique_ptr< ::HIR::ExprNode> into_unique();
    operator bool () const { return node; }
    ::HIR::ExprNode* get() const { return node.get(); }
    void reset(::HIR::ExprNode* p) { node.reset(p); }

          ::HIR::ExprNode& operator*()       { return *node; }
    const ::HIR::ExprNode& operator*() const { return *node; }
          ::HIR::ExprNode* operator->()       { return &*node; }
    const ::HIR::ExprNode* operator->() const { return &*node; }
};

}   // namespace HIR
