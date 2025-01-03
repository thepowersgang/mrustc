/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/main_bindings.hpp
 * - Trans functions called by main()
 */
#pragma once

#include "trans_list.hpp"

namespace HIR {
class Crate;
}

struct TransOptions
{
    ::std::string   mode = "c";
    unsigned int opt_level = 0;
    bool emit_debug_info = false;
    ::std::string   build_command_file;

    ::std::string   panic_crate;

    ::std::vector< ::std::string>   library_search_dirs;
    ::std::vector< ::std::string>   libraries;
};

enum class CodegenOutput {
    Object, // .o
    StaticLibrary,  // .a
    DynamicLibrary, // .so
    Executable, // no suffix, includes main stub (TODO: Can't that just be added earlier?)
};

extern TransList Trans_Enumerate_Main(const ::HIR::Crate& crate);
// NOTE: This also sets the saveout flags
extern TransList Trans_Enumerate_Public(::HIR::Crate& crate);

/// Re-run enumeration on monomorphised functions, removing now-unused items
extern void Trans_Enumerate_Cleanup(const ::HIR::Crate& crate, TransList& list);

extern void Trans_AutoImpls(::HIR::Crate& crate, TransList& trans_list);

extern void Trans_Monomorphise_List(const ::HIR::Crate& crate, TransList& list);

extern void Trans_Codegen(const ::std::string& outfile, CodegenOutput out_ty, const TransOptions& opt, const ::HIR::Crate& crate, TransList list, const ::std::string& hir_file);
