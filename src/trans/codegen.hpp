/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen.hpp
 * - Common class and methods for codegen
 */
#pragma once

#include "trans_list.hpp"
#include "main_bindings.hpp"    // TransOptions

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
    virtual void finalise(const TransOptions& opt, CodegenOutput out_ty, const ::std::string& hir_file) {}

    // Called on all types directly mentioned (e.g. variables, arguments, and fields)
    // - Inner-most types are visited first.
    virtual void emit_type_proto(const ::HIR::TypeRef& ) {}
    virtual void emit_type(const ::HIR::TypeRef& ) {}
    virtual void emit_type_id(const ::HIR::TypeRef& ) {}

    // Called when a TypeRef::Path is encountered (after visiting inner types)
    virtual void emit_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) {}
    virtual void emit_union(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Union& item) {}
    virtual void emit_enum(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Enum& item) {}

    virtual void emit_constructor_enum(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::Enum& item, size_t var_idx) {}
    virtual void emit_constructor_struct(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::Struct& item) {}

    virtual void emit_static_ext(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) {}
    virtual void emit_static_proto(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) {}
    virtual void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) {}

    virtual void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) {}
    virtual void emit_function_proto(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def) {}
    virtual void emit_function_code(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def, const ::MIR::FunctionPointer& code) {}
};

extern ::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::HIR::Crate& crate, const ::std::string& outfile);
extern ::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGenerator_MonoMir(const ::HIR::Crate& crate, const ::std::string& outfile);

