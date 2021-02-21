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

struct Reloc {
    size_t  ofs;
    size_t  len;
    const ::HIR::Path* p;
    ::std::string   bytes;

    static Reloc new_named(size_t ofs, size_t len, const ::HIR::Path* p) {
        return Reloc { ofs, len, p, "" };
    }
    static Reloc new_bytes(size_t ofs, size_t len, ::std::string bytes) {
        return Reloc { ofs, len, nullptr, ::std::move(bytes) };
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Reloc& x) {
        os << "@" << std::hex << "0x" << x.ofs << "+" << x.len << " = ";
        if(x.p) {
            os << "&" << *x.p;
        }
        else {
            os << "\"" << FmtEscaped(x.bytes) << "\"";
        }
        return os;
    }
};
struct EncodedLiteral {
    static const unsigned PTR_BASE = 0x1000;

    std::vector<uint8_t>    bytes;
    std::vector<Reloc>  relocations;
    // List of fabricated paths used in relocations
    std::vector< std::unique_ptr<HIR::Path> >   paths;
};
EncodedLiteral Trans_EncodeLiteralAsBytes(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Literal& lit, const ::HIR::TypeRef& ty);

extern ::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::HIR::Crate& crate, const ::std::string& outfile);
extern ::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGenerator_MonoMir(const ::HIR::Crate& crate, const ::std::string& outfile);

