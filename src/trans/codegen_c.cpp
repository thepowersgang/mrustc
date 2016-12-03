/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen_c.cpp
 * - Code generation emitting C code
 */
#include "codegen.hpp"
#include "mangling.hpp"
#include <fstream>

namespace {
    class CodeGenerator_C:
        public CodeGenerator
    {
        ::std::ofstream m_of;
    public:
        CodeGenerator_C(const ::std::string& outfile):
            m_of(outfile)
        {
        }
        
        ~CodeGenerator_C() {}

        void finalise() override
        {
        }
        
        void emit_struct(const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            m_of << "struct s_" << Trans_Mangle(p) << " {\n";
            m_of << "}\n";
        }
        //virtual void emit_union(const ::HIR::GenericPath& p, const ::HIR::Union& item);
        //virtual void emit_enum(const ::HIR::GenericPath& p, const ::HIR::Enum& item);
        
        //virtual void emit_static_ext(const ::HIR::Path& p);
        //virtual void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params);
        
        //virtual void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params);
        //virtual void emit_function_proto(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params);
        //virtual void emit_function_code(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params);
    private:
        
    };
}

::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::std::string& outfile)
{
    return ::std::unique_ptr<CodeGenerator>(new CodeGenerator_C(outfile));
}
