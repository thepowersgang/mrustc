/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen.hpp
 * - Common class and methods for codegen
 */
#pragma once

#include "trans_list.hpp"

namespace HIR {
    class TypeRef;
    class Path;
    class GenericPath;
    
    class Function;
    class Static;
}
namespace MIR {
    class FunctionPointer;
}


class CodeGenerator
{
public:
    virtual ~CodeGenerator() {}
    virtual void finalise() {}
    
    virtual void emit_tuple(const ::HIR::GenericPath& p, const ::std::vector<::HIR::TypeRef>& ) {}
    virtual void emit_struct(const ::HIR::GenericPath& p, const ::HIR::Struct& item) {}
    virtual void emit_union(const ::HIR::GenericPath& p, const ::HIR::Union& item) {}
    virtual void emit_enum(const ::HIR::GenericPath& p, const ::HIR::Enum& item) {}
    
    virtual void emit_static_ext(const ::HIR::Path& p) {}
    virtual void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) {}
    
    virtual void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) {}
    virtual void emit_function_proto(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) {}
    virtual void emit_function_code(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, const ::MIR::FunctionPointer& code) {}
};


extern ::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::std::string& outfile);

