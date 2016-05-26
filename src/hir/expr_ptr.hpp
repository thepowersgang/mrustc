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
    ~ExprPtr();
    
          ::HIR::ExprNode& operator*()       { return *node; }
    const ::HIR::ExprNode& operator*() const { return *node; }
          ::HIR::ExprNode* operator->()       { return node; }
    const ::HIR::ExprNode* operator->() const { return node; }
};

}   // namespace HIR
