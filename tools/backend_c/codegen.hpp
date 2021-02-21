/*
 */
#pragma once

#include <hir_sim.hpp>
#include <module_tree.hpp>
#include <fstream>

class Codegen_C
{
    std::ofstream   m_of;
public:
    Codegen_C(const char* outfile);
    ~Codegen_C();

    void emit_type_proto(const HIR::TypeRef& ty);
    void emit_static_proto(const RcString& name, const Static& s);
    void emit_function_proto(const RcString& name, const Function& s);

    void emit_composite(const RcString& name, const DataType& s);

    void emit_static(const RcString& name, const Static& s);
    void emit_function(const RcString& name, const ModuleTree& tree, const Function& s);

private:
    void emit_ctype(const HIR::TypeRef& ty, unsigned depth=0);
};
