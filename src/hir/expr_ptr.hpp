/*
 */
#pragma once
#include <memory>

namespace HIR {

class ExprNode;

class ExprPtr
{
    ::HIR::ExprNode* node;
    
public:
    ExprPtr();
    ExprPtr(::std::unique_ptr< ::HIR::ExprNode> _);
    ExprPtr(ExprPtr&& x):
        node(x.node)
    {
        x.node = nullptr;
    }
    ExprPtr& operator=(ExprPtr&& x)
    {
        this->~ExprPtr();
        node = x.node;
        x.node = nullptr;
        return *this;
    }
    ~ExprPtr();
    
    ::std::unique_ptr< ::HIR::ExprNode> into_unique();
    operator bool () const { return node != nullptr; }
    ::HIR::ExprNode* get() const { return node; }
    
          ::HIR::ExprNode& operator*()       { return *node; }
    const ::HIR::ExprNode& operator*() const { return *node; }
          ::HIR::ExprNode* operator->()       { return node; }
    const ::HIR::ExprNode* operator->() const { return node; }
};

}   // namespace HIR
