/*
 */
#pragma once


namespace HIR {

class Visitor;

class ExprNode
{
public:
    virtual void visit(Visitor& v) = 0;
    virtual ~ExprNode() = 0;
};

class ExprNode_Block:
    public ExprNode
{
};

class Visitor
{
public:
    virtual void visit(ExprNode_Block& n) = 0;
};

}

