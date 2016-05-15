/*
 */
#pragma once


namespace HIR {

class ExprNode;

class ExprPtr
{
    ::HIR::ExprNode* node;
    
public:
    ExprPtr();
    ~ExprPtr();
};

}   // namespace HIR
