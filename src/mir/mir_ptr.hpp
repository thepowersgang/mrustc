/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir_ptr.hpp
 * - Pointer to a blob of MIR
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
    FunctionPointer(FunctionPointer&& x): ptr(x.ptr) { x.ptr = nullptr; }

    ~FunctionPointer() {
        reset();
    }
    FunctionPointer& operator=(FunctionPointer&& x) {
        reset();
        ptr = x.ptr;
        x.ptr = nullptr;
        return *this;
    }

    void reset();

          ::MIR::Function* operator->()       { if(!ptr) throw ""; return ptr; }
    const ::MIR::Function* operator->() const { if(!ptr) throw ""; return ptr; }
          ::MIR::Function& operator*()       { if(!ptr) throw ""; return *ptr; }
    const ::MIR::Function& operator*() const { if(!ptr) throw ""; return *ptr; }

    operator bool() const { return ptr != nullptr; }
};

}

