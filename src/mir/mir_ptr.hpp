/*
 */
#pragma once


namespace MIR {

class Function;

class FunctionPointer
{
    ::MIR::Function*    ptr;
public:
    FunctionPointer(): ptr(nullptr) {}
    FunctionPointer(::MIR::Function* p): ptr(p) {}
    ~FunctionPointer();
    
    ::MIR::Function& operator->() { return *ptr; }
    ::MIR::Function& operator*() { return *ptr; }
};

}

