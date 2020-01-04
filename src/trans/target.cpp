/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/target.cpp
 * - Target-specific information
 */
#include "target.hpp"
#include <algorithm>
#include "../expand/cfg.hpp"
#include <fstream>
#include <map>
#include <hir/hir.hpp>
#include <hir_typeck/helpers.hpp>
#include <toml.h>   // tools/common

const TargetArch ARCH_X86_64 = {
    "x86_64",
    64, false,
    TargetArch::Atomics(/*atomic(u8)=*/true, false, true, true,  true),
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 8)
    };
const TargetArch ARCH_X86 = {
    "x86",
    32, false,
    { /*atomic(u8)=*/true, false, true, false,  true },
    TargetArch::Alignments(2, 4, /*u64*/4, /*u128*/4, 4, 4, /*ptr*/4)    // u128 has the same alignment as u64, which is u32's alignment. And f64 is 4 byte aligned
};
const TargetArch ARCH_ARM64 = {
    "aarch64",
    64, false,
    { /*atomic(u8)=*/true, true, true, true,  true },
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 8)
};
const TargetArch ARCH_ARM32 = {
    "arm",
    32, false,
    { /*atomic(u8)=*/true, false, true, false,  true },
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 4) // Note, all types are natively aligned (but i128 will be emulated)
};
const TargetArch ARCH_M68K = {
    "m68k",
    32, true,
    { /*atomic(u8)=*/true, false, true, false,  true },
    TargetArch::Alignments(2, 2, 2, 2, 2, 2, 2)
};
TargetSpec  g_target;


bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align);

namespace
{
    TargetSpec load_spec_from_file(const ::std::string& filename)
    {
        TargetSpec  rv;

        TomlFile    toml_file(filename);
        for(auto key_val : toml_file)
        {
            // Assertion: The way toml works, there has to be at least two entries in every path.
            assert(key_val.path.size() > 1);
            DEBUG(key_val.path << " = " << key_val.value);

            auto check_path_length = [&](const TomlKeyValue& kv, unsigned len) {
                if( kv.path.size() != len ) {
                    if( kv.path.size() > len ) {
                        ::std::cerr << "ERROR: Unexpected sub-node to  " << kv.path << " in " << filename << ::std::endl;
                    }
                    else {
                        ::std::cerr << "ERROR: Expected sub-nodes in  " << kv.path << " in " << filename << ::std::endl;
                    }
                    exit(1);
                }
                };
            auto check_path_length_min = [&](const TomlKeyValue& kv, unsigned len) {
                if( kv.path.size() < len ) {
                    ::std::cerr << "ERROR: Expected sub-nodes in " << kv.path << " in " << filename << ::std::endl;
                }
                };

            try
            {
                if( key_val.path[0] == "target" )
                {
                    check_path_length_min(key_val, 2);
                    if( key_val.path[1] == "family" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_family = key_val.value.as_string();
                    }
                    else if( key_val.path[1] == "os-name" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_os_name = key_val.value.as_string();
                    }
                    else if( key_val.path[1] == "env-name" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_env_name = key_val.value.as_string();
                    }
                    else if( key_val.path[1] == "arch" )
                    {
                        check_path_length(key_val, 2);
                        if( key_val.value.as_string() == ARCH_ARM32.m_name )
                        {
                            rv.m_arch = ARCH_ARM32;
                        }
                        else if( key_val.value.as_string() == ARCH_ARM64.m_name )
                        {
                            rv.m_arch = ARCH_ARM64;
                        }
                        else if( key_val.value.as_string() == ARCH_X86.m_name )
                        {
                            rv.m_arch = ARCH_X86;
                        }
                        else if( key_val.value.as_string() == ARCH_X86_64.m_name )
                        {
                            rv.m_arch = ARCH_X86_64;
                        }
                        else if( key_val.value.as_string() == ARCH_M68K.m_name )
                        {
                            rv.m_arch = ARCH_M68K;
                        }
                        else
                        {
                            // Error.
                            ::std::cerr << "ERROR: Unknown architecture name '" << key_val.value.as_string() << "' in " << filename << ::std::endl;
                            exit(1);
                        }
                    }
                    else
                    {
                        // Warning
                        ::std::cerr << "Warning: Unknown configuration item " << key_val.path[0] << "." << key_val.path[1] << " in " << filename << ::std::endl;
                    }
                }
                else if( key_val.path[0] == "backend" )
                {
                    check_path_length_min(key_val, 2);
                    if( key_val.path[1] == "c" )
                    {
                        check_path_length_min(key_val, 3);

                        if( key_val.path[2] == "variant" )
                        {
                            check_path_length(key_val, 3);
                            if( key_val.value.as_string() == "msvc" )
                            {
                                rv.m_backend_c.m_codegen_mode = CodegenMode::Msvc;
                            }
                            else if( key_val.value.as_string() == "gnu" )
                            {
                                rv.m_backend_c.m_codegen_mode = CodegenMode::Gnu11;
                            }
                            else
                            {
                                ::std::cerr << "ERROR: Unknown C variant name '" << key_val.value.as_string() << "' in " << filename << ::std::endl;
                                exit(1);
                            }
                        }
                        else if( key_val.path[2] == "target" )
                        {
                            check_path_length(key_val, 3);
                            rv.m_backend_c.m_c_compiler = key_val.value.as_string();
                        }
                        else if( key_val.path[2] == "emulate-i128" )
                        {
                            check_path_length(key_val, 3);
                            rv.m_backend_c.m_emulated_i128 = key_val.value.as_bool();
                        }
                        else if( key_val.path[2] == "compiler-opts" )
                        {
                            check_path_length(key_val, 3);
                            for(const auto& v : key_val.value.as_list())
                            {
                                rv.m_backend_c.m_compiler_opts.push_back( v.as_string() );
                            }
                        }
                        else if( key_val.path[2] == "linker-opts" )
                        {
                            check_path_length(key_val, 3);
                            for(const auto& v : key_val.value.as_list())
                            {
                                rv.m_backend_c.m_linker_opts.push_back( v.as_string() );
                            }
                        }
                        else
                        {
                            ::std::cerr << "WARNING: Unknown field backend.c." << key_val.path[2] << " in " << filename << ::std::endl;
                        }
                    }
                    // Does MMIR need configuration?
                    else
                    {
                        ::std::cerr << "WARNING: Unknown configuration item backend." << key_val.path[1] << " in " << filename << ::std::endl;
                    }
                }
                else if( key_val.path[0] == "arch" )
                {
                    check_path_length_min(key_val, 2);
                    if( key_val.path[1] == "name" )
                    {
                        check_path_length(key_val, 2);
                        if( rv.m_arch.m_name != "" ) {
                            ::std::cerr << "ERROR: Architecture already specified to be '" << rv.m_arch.m_name << "'" << ::std::endl;
                            exit(1);
                        }
                        rv.m_arch.m_name = key_val.value.as_string();
                    }
                    else if( key_val.path[1] == "pointer-bits" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_pointer_bits = key_val.value.as_int();
                    }
                    else if( key_val.path[1] == "is-big-endian" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_big_endian = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "has-atomic-u8" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_atomics.u8 = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "has-atomic-u16" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_atomics.u16 = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "has-atomic-u32" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_atomics.u32 = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "has-atomic-u64" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_atomics.u64 = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "has-atomic-ptr" )
                    {
                        check_path_length(key_val, 2);
                        rv.m_arch.m_atomics.ptr = key_val.value.as_bool();
                    }
                    else if( key_val.path[1] == "alignments" )
                    {
                        check_path_length(key_val, 3);
                        if( key_val.path[2] == "u16" )
                        {
                            rv.m_arch.m_alignments.u16 = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "u32" )
                        {
                            rv.m_arch.m_alignments.u32 = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "u64" )
                        {
                            rv.m_arch.m_alignments.u64  = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "u128" )
                        {
                            rv.m_arch.m_alignments.u128 = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "f32" )
                        {
                            rv.m_arch.m_alignments.f32 = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "f64" )
                        {
                            rv.m_arch.m_alignments.f64 = key_val.value.as_int();
                        }
                        else if( key_val.path[2] == "ptr" )
                        {
                            rv.m_arch.m_alignments.ptr = key_val.value.as_int();
                        }
                        else
                        {
                            ::std::cerr << "WARNING: Unknown field arch.alignments." << key_val.path[1] << " in " << filename << ::std::endl;
                        }
                    }
                    else
                    {
                        ::std::cerr << "WARNING: Unknown field arch." << key_val.path[1] << " in " << filename << ::std::endl;
                    }
                }
                else
                {
                    ::std::cerr << "WARNING: Unknown configuration item " << key_val.path[0] << " in " << filename << ::std::endl;
                }
            }
            catch(const TomlValue::TypeError& e)
            {
                ::std::cerr << "ERROR: Invalid type for " << key_val.path << " - " << e << ::std::endl;
                exit(1);
            }
        }

        // TODO: Ensure that everything is set
        if( rv.m_arch.m_name == "" ) {
            ::std::cerr << "ERROR: Architecture not specified in " << filename << ::std::endl;
            exit(1);
        }

        return rv;
    }
    void save_spec_to_file(const ::std::string& filename, const TargetSpec& spec)
    {
        // TODO: Have a round-trip unit test
        ::std::ofstream of(filename);

        struct H
        {
            static const char* tfstr(bool v)
            {
                return v ? "true" : "false";
            }
            static const char* c_variant_name(const CodegenMode m)
            {
                switch(m)
                {
                case CodegenMode::Gnu11: return "gnu";
                case CodegenMode::Msvc: return "msvc";
                }
                return "";
            }
        };

        of
            << "[target]\n"
            << "family = \"" << spec.m_family << "\"\n"
            << "os-name = \"" << spec.m_os_name << "\"\n"
            << "env-name = \"" << spec.m_env_name << "\"\n"
            //<< "arch = \"" << spec.m_arch.m_name << "\"\n"
            << "\n"
            << "[backend.c]\n"
            << "variant = \"" << H::c_variant_name(spec.m_backend_c.m_codegen_mode) << "\"\n"
            << "target = \"" << spec.m_backend_c.m_c_compiler << "\"\n"
            << "compiler-opts = ["; for(const auto& s : spec.m_backend_c.m_compiler_opts) of << "\"" << s << "\","; of << "]\n"
            << "linker-opts = ["; for(const auto& s : spec.m_backend_c.m_linker_opts) of << "\"" << s << "\","; of << "]\n"
            << "\n"
            << "[arch]\n"
            << "name = \"" << spec.m_arch.m_name << "\"\n"
            << "pointer-bits = " << spec.m_arch.m_pointer_bits << "\n"
            << "is-big-endian = " << H::tfstr(spec.m_arch.m_big_endian) << "\n"
            << "has-atomic-u8 = " << H::tfstr(spec.m_arch.m_atomics.u8) << "\n"
            << "has-atomic-u16 = " << H::tfstr(spec.m_arch.m_atomics.u16) << "\n"
            << "has-atomic-u32 = " << H::tfstr(spec.m_arch.m_atomics.u32) << "\n"
            << "has-atomic-u64 = " << H::tfstr(spec.m_arch.m_atomics.u64) << "\n"
            << "has-atomic-ptr = " << H::tfstr(spec.m_arch.m_atomics.ptr) << "\n"
            << "alignments = {"
                << " u16 = "  << static_cast<int>(spec.m_arch.m_alignments.u16 ) << ","
                << " u32 = "  << static_cast<int>(spec.m_arch.m_alignments.u32 ) << ","
                << " u64 = "  << static_cast<int>(spec.m_arch.m_alignments.u64 ) << ","
                << " u128 = " << static_cast<int>(spec.m_arch.m_alignments.u128) << ","
                << " f32 = "  << static_cast<int>(spec.m_arch.m_alignments.f32 ) << ","
                << " f64 = "  << static_cast<int>(spec.m_arch.m_alignments.f64 ) << ","
                << " ptr = "  << static_cast<int>(spec.m_arch.m_alignments.ptr )
                << " }\n"
            << "\n"
            ;
    }
    TargetSpec init_from_spec_name(const ::std::string& target_name)
    {
        // Options for all the fully-GNU environments
        #define BACKEND_C_OPTS_GNU  {"-ffunction-sections", "-pthread"}, {"-Wl,--gc-sections"}
        // If there's a '/' or a '\' in the filename, open it as a path, otherwise assume it's a triple.
        if( target_name.find('/') != ::std::string::npos || target_name.find('\\') != ::std::string::npos )
        {
            return load_spec_from_file(target_name);
        }
        else if(target_name == "i586-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, true, "i586-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_X86
                };
        }
        else if(target_name == "x86_64-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, false, "x86_64-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if(target_name == "arm-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, true, "arm-elf-eabi", BACKEND_C_OPTS_GNU},
                ARCH_ARM32
                };
        }
        else if(target_name == "aarch64-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, false, "aarch64-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_ARM64
                };
        }
        else if(target_name == "m68k-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, true, "m68k-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_M68K
                };
        }
        else if(target_name == "i586-pc-windows-gnu")
        {
            return TargetSpec {
                "windows", "windows", "gnu", {CodegenMode::Gnu11, true, "mingw32", BACKEND_C_OPTS_GNU},
                ARCH_X86
            };
        }
        else if(target_name == "x86_64-pc-windows-gnu")
        {
            return TargetSpec {
                "windows", "windows", "gnu", {CodegenMode::Gnu11, false, "x86_64-w64-mingw32", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if (target_name == "x86-pc-windows-msvc")
        {
            // TODO: Should this include the "kernel32.lib" inclusion?
            return TargetSpec {
                "windows", "windows", "msvc", {CodegenMode::Msvc, true, "x86", {}, {}},
                ARCH_X86
            };
        }
        else if (target_name == "x86_64-pc-windows-msvc")
        {
            return TargetSpec {
                "windows", "windows", "msvc", {CodegenMode::Msvc, true, "amd64", {}, {}},
                ARCH_X86_64
                };
        }
        else if(target_name == "i686-unknown-freebsd")
        {
            return TargetSpec {
                "unix", "freebsd", "gnu", {CodegenMode::Gnu11, true, "i686-unknown-freebsd", BACKEND_C_OPTS_GNU},
                ARCH_X86
                };
        }
        else if(target_name == "x86_64-unknown-freebsd")
        {
            return TargetSpec {
                "unix", "freebsd", "gnu", {CodegenMode::Gnu11, false, "x86_64-unknown-freebsd", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if(target_name == "arm-unknown-freebsd")
        {
            return TargetSpec {
                "unix", "freebsd", "gnu", {CodegenMode::Gnu11, true, "arm-unknown-freebsd", BACKEND_C_OPTS_GNU},
                ARCH_ARM32
                };
        }
        else if(target_name == "aarch64-unknown-freebsd")
        {
            return TargetSpec {
                "unix", "freebsd", "gnu", {CodegenMode::Gnu11, false, "aarch64-unknown-freebsd", BACKEND_C_OPTS_GNU},
                ARCH_ARM64
                };
        }
        else if(target_name == "x86_64-unknown-netbsd")
        {
            return TargetSpec {
                "unix", "netbsd", "gnu", {CodegenMode::Gnu11, false, "x86_64-unknown-netbsd", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if(target_name == "i686-unknown-openbsd")
        {
            return TargetSpec {
                "unix", "openbsd", "gnu", {CodegenMode::Gnu11, true, "i686-unknown-openbsd", BACKEND_C_OPTS_GNU},
                ARCH_X86
                };
        }
        else if(target_name == "x86_64-unknown-openbsd")
        {
            return TargetSpec {
                "unix", "openbsd", "gnu", {CodegenMode::Gnu11, false, "x86_64-unknown-openbsd", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if(target_name == "arm-unknown-openbsd")
        {
            return TargetSpec {
                "unix", "openbsd", "gnu", {CodegenMode::Gnu11, true, "arm-unknown-openbsd", BACKEND_C_OPTS_GNU},
                ARCH_ARM32
                };
        }
        else if(target_name == "aarch64-unknown-openbsd")
        {
            return TargetSpec {
                "unix", "openbsd", "gnu", {CodegenMode::Gnu11, false, "aarch64-unknown-openbsd", BACKEND_C_OPTS_GNU},
                ARCH_ARM64
                };
        }
        else if(target_name == "x86_64-unknown-dragonfly")
        {
            return TargetSpec {
                "unix", "dragonfly", "gnu", {CodegenMode::Gnu11, false, "x86_64-unknown-dragonfly", BACKEND_C_OPTS_GNU},
                ARCH_X86_64
                };
        }
        else if(target_name == "x86_64-apple-macosx")
        {
            // NOTE: OSX uses Mach-O binaries, which don't fully support the defaults used for GNU targets
            return TargetSpec {
                "unix", "macos", "gnu", {CodegenMode::Gnu11, false, "x86_64-apple-darwin", {}, {}},
                ARCH_X86_64
                };
        }
        else if(target_name == "arm-unknown-haiku")
        {
            return TargetSpec {
                "unix", "haiku", "gnu", {CodegenMode::Gnu11, true, "arm-unknown-haiku", {}, {}},
                ARCH_ARM32
                };
        }
        else if(target_name == "x86_64-unknown-haiku")
        {
            return TargetSpec {
                "unix", "haiku", "gnu", {CodegenMode::Gnu11, false, "x86_64-unknown-haiku", {}, {}},
                ARCH_X86_64
                };
        }
        else
        {
            ::std::cerr << "Unknown target name '" << target_name << "'" << ::std::endl;
            abort();
        }
        throw "";
    }
}

const TargetSpec& Target_GetCurSpec()
{
    return g_target;
}
void Target_ExportCurSpec(const ::std::string& filename)
{
    save_spec_to_file(filename, g_target);
}
void Target_SetCfg(const ::std::string& target_name)
{
    g_target = init_from_spec_name(target_name);

    if(g_target.m_family == "unix") {
        Cfg_SetFlag("unix");
    }
    else if( g_target.m_family == "windows") {
        Cfg_SetFlag("windows");
    }
    Cfg_SetValue("target_family", g_target.m_family);

    if( g_target.m_os_name == "linux" )
    {
        Cfg_SetFlag("linux");
        Cfg_SetValue("target_vendor", "gnu");
    }

    if( g_target.m_os_name == "freebsd" )
    {
        Cfg_SetFlag("freebsd");
        Cfg_SetValue("target_vendor", "unknown");
    }

    if( g_target.m_os_name == "netbsd" )
    {
        Cfg_SetFlag("netbsd");
        Cfg_SetValue("target_vendor", "unknown");
    }

    if( g_target.m_os_name == "openbsd" )
    {
        Cfg_SetFlag("openbsd");
        Cfg_SetValue("target_vendor", "unknown");
    }

    if( g_target.m_os_name == "dragonfly" )
    {
        Cfg_SetFlag("dragonfly");
        Cfg_SetValue("target_vendor", "unknown");
    }

    Cfg_SetValue("target_env", g_target.m_env_name);
    Cfg_SetValue("target_os", g_target.m_os_name);
    Cfg_SetValue("target_pointer_width", FMT(g_target.m_arch.m_pointer_bits));
    Cfg_SetValue("target_endian", g_target.m_arch.m_big_endian ? "big" : "little");
    Cfg_SetValue("target_arch", g_target.m_arch.m_name);
    if(g_target.m_arch.m_atomics.u8)    Cfg_SetValue("target_has_atomic", "8");
    if(g_target.m_arch.m_atomics.u16)   Cfg_SetValue("target_has_atomic", "16");
    if(g_target.m_arch.m_atomics.u32)   Cfg_SetValue("target_has_atomic", "32");
    if(g_target.m_arch.m_atomics.u64)   Cfg_SetValue("target_has_atomic", "64");
    if(g_target.m_arch.m_atomics.ptr)   Cfg_SetValue("target_has_atomic", "ptr");
    if(g_target.m_arch.m_atomics.ptr)   Cfg_SetValue("target_has_atomic", "cas");   // TODO: Atomic compare-and-set option
    Cfg_SetValueCb("target_feature", [](const ::std::string& s) {
        return false;
        });
}

bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align)
{
    TRACE_FUNCTION_FR(ty, "size=" << out_size << ", align=" << out_align);
    TU_MATCHA( (ty.m_data), (te),
    (Infer,
        BUG(sp, "sizeof on _ type");
        ),
    (Diverge,
        out_size = 0;
        out_align = 0;
        return true;
        ),
    (Primitive,
        switch(te)
        {
        case ::HIR::CoreType::Bool:
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::I8:
            out_size = 1;
            out_align = 1;  // u8 is always 1 aligned
            return true;
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::I16:
            out_size = 2;
            out_align = g_target.m_arch.m_alignments.u16;
            return true;
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::Char:
            out_size = 4;
            out_align = g_target.m_arch.m_alignments.u32;
            return true;
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::I64:
            out_size = 8;
            out_align = g_target.m_arch.m_alignments.u64;
            return true;
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::I128:
            out_size = 16;
            // TODO: If i128 is emulated, this can be 8 (as it is on x86, where it's actually 4 due to the above comment)
            if( g_target.m_backend_c.m_emulated_i128 )
                out_align = g_target.m_arch.m_alignments.u64;
            else
                out_align = g_target.m_arch.m_alignments.u128;
            return true;
        case ::HIR::CoreType::Usize:
        case ::HIR::CoreType::Isize:
            out_size = g_target.m_arch.m_pointer_bits / 8;
            out_align = g_target.m_arch.m_alignments.ptr;
            return true;
        case ::HIR::CoreType::F32:
            out_size = 4;
            out_align = g_target.m_arch.m_alignments.f32;
            return true;
        case ::HIR::CoreType::F64:
            out_size = 8;
            out_align = g_target.m_arch.m_alignments.f64;
            return true;
        case ::HIR::CoreType::Str:
            DEBUG("sizeof on a `str` - unsized");
            out_size  = SIZE_MAX;
            out_align = 1;
            return true;
        }
        ),
    (Path,
        if( te.binding.is_Opaque() )
            return false;
        if( te.binding.is_ExternType() )
        {
            DEBUG("sizeof on extern type - unsized");
            out_align = 0;
            out_size = SIZE_MAX;
            return true;
        }
        const auto* repr = Target_GetTypeRepr(sp, resolve, ty);
        if( !repr )
        {
            DEBUG("Cannot get type repr for " << ty);
            return false;
        }
        out_size  = repr->size;
        out_align = repr->align;
        return true;
        ),
    (Generic,
        // Unknown - return false
        DEBUG("No repr for Generic - " << ty);
        return false;
        ),
    (TraitObject,
        out_align = 0;
        out_size = SIZE_MAX;
        DEBUG("sizeof on a trait object - unsized");
        return true;
        ),
    (ErasedType,
        BUG(sp, "sizeof on an erased type - shouldn't exist");
        ),
    (Array,
        if( !Target_GetSizeAndAlignOf(sp, resolve, *te.inner, out_size,out_align) )
            return false;
        if( out_size == SIZE_MAX )
            BUG(sp, "Unsized type in array - " << ty);
        if( te.size.as_Known() == 0 || out_size == 0 )
        {
            out_size = 0;
        }
        else
        {
            if( SIZE_MAX / te.size.as_Known() <= out_size )
                BUG(sp, "Integer overflow calculating array size");
            out_size *= te.size.as_Known();
        }
        return true;
        ),
    (Slice,
        if( !Target_GetAlignOf(sp, resolve, *te.inner, out_align) )
            return false;
        out_size = SIZE_MAX;
        DEBUG("sizeof on a slice - unsized");
        return true;
        ),
    (Tuple,
        const auto* repr = Target_GetTypeRepr(sp, resolve, ty);
        if( !repr )
        {
            DEBUG("Cannot get type repr for " << ty);
            return false;
        }
        out_size  = repr->size;
        out_align = repr->align;
        return true;
        ),
    (Borrow,
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        // TODO: Handle different types of Unsized (ones with different pointer sizes)
        switch(resolve.metadata_type(sp, *te.inner))
        {
        case MetadataType::Unknown:
            return false;
        case MetadataType::None:
        case MetadataType::Zero:
            out_size = g_target.m_arch.m_pointer_bits / 8;
            break;
        case MetadataType::Slice:
        case MetadataType::TraitObject:
            out_size = g_target.m_arch.m_pointer_bits / 8 * 2;
            break;
        }
        return true;
        ),
    (Pointer,
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        switch(resolve.metadata_type(sp, *te.inner))
        {
        case MetadataType::Unknown:
            return false;
        case MetadataType::None:
        case MetadataType::Zero:
            out_size = g_target.m_arch.m_pointer_bits / 8;
            break;
        case MetadataType::Slice:
        case MetadataType::TraitObject:
            out_size = g_target.m_arch.m_pointer_bits / 8 * 2;
            break;
        }
        return true;
        ),
    (Function,
        // Pointer size
        out_size = g_target.m_arch.m_pointer_bits / 8;
        out_align = g_target.m_arch.m_pointer_bits / 8;
        return true;
        ),
    (Closure,
        BUG(sp, "Encountered closure type at trans stage - " << ty);
        )
    )
    return false;
}
bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size)
{
    size_t  ignore_align;
    bool rv = Target_GetSizeAndAlignOf(sp, resolve, ty, out_size, ignore_align);
    if( out_size == SIZE_MAX )
        BUG(sp, "Getting size of Unsized type - " << ty);
    return rv;
}
bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align)
{
    size_t  ignore_size;
    bool rv = Target_GetSizeAndAlignOf(sp, resolve, ty, ignore_size, out_align);
    if( rv && ignore_size == SIZE_MAX )
        BUG(sp, "Getting alignment of Unsized type - " << ty);
    return rv;
}


namespace {
    // Returns NULL when the repr can't be determined
    ::std::unique_ptr<TypeRepr> make_type_repr_struct(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        TRACE_FUNCTION_F(ty);
        struct Ent {
            unsigned int field;
            size_t  size;
            size_t  align;
        };
        ::std::vector<TypeRepr::Field>  fields;
        ::std::vector<Ent>  ents;
        bool packed = false;
        bool allow_sort = false;
        if( ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Struct() )
        {
            const auto& te = ty.m_data.as_Path();
            const auto& str = *te.binding.as_Struct();
            auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
            auto monomorph = [&](const auto& tpl) {
                auto rv = monomorphise_type_with(sp, tpl, monomorph_cb);
                resolve.expand_associated_types(sp, rv);
                return rv;
                };
            TU_MATCHA( (str.m_data), (se),
            (Unit,
                ),
            (Tuple,
                unsigned int idx = 0;
                for(const auto& e : se)
                {
                    auto ty = monomorph(e.ent);
                    size_t  size, align;
                    if( !Target_GetSizeAndAlignOf(sp, resolve, ty, size,align) )
                    {
                        DEBUG("Can't get size/align of " << ty);
                        return nullptr;
                    }
                    ents.push_back(Ent { idx++, size, align });
                    fields.push_back(TypeRepr::Field { 0, mv$(ty) });
                }
                ),
            (Named,
                unsigned int idx = 0;
                for(const auto& e : se)
                {
                    auto ty = monomorph(e.second.ent);
                    size_t  size, align;
                    if( !Target_GetSizeAndAlignOf(sp, resolve, ty, size,align) )
                    {
                        DEBUG("Can't get size/align of " << ty);
                        return nullptr;
                    }
                    ents.push_back(Ent { idx++, size, align });
                    fields.push_back(TypeRepr::Field { 0, mv$(ty) });
                }
                )
            )
            switch(str.m_repr)
            {
            case ::HIR::Struct::Repr::Packed:
                // packed, not sorted
                packed = true;
                // NOTE: codegen_c checks m_repr for packing too
                break;
            case ::HIR::Struct::Repr::C:
            case ::HIR::Struct::Repr::Simd:
                // No sorting, no packing
                break;
            case ::HIR::Struct::Repr::Aligned:
                // TODO: Update the minimum alignment
            case ::HIR::Struct::Repr::Transparent:
            case ::HIR::Struct::Repr::Rust:
                allow_sort = true;
                break;
            }
        }
        else if( const auto* te = ty.m_data.opt_Tuple() )
        {
            DEBUG("Tuple " << ty);
            unsigned int idx = 0;
            for(const auto& t : *te)
            {
                size_t  size, align;
                if( !Target_GetSizeAndAlignOf(sp, resolve, t, size,align) )
                {
                    DEBUG("Can't get size/align of " << t);
                    return nullptr;
                }
                ents.push_back(Ent { idx++, size, align });
                fields.push_back(TypeRepr::Field { 0, t.clone() });
            }
        }
        else
        {
            BUG(sp, "Unexpected type in creating type repr - " << ty);
        }


        if( allow_sort )
        {
            // Sort by alignment then size (largest first)
            // - NOTE: ?Sized fields (which includes unsized fields) MUST be at the end, even after monomorph
            // - This means that this code needs to know if a field was ?Sized
            // TODO: Determine if a field was from a ?Sized generic (so should be fixed as last)
            //auto cmpfn_lt = [](const Ent& a, const Ent& b){ return a.align == b.align ? a.size < b.size : a.align < b.size; };
            //::std::sort(ents.begin(), ents.end(), cmpfn_lt);
        }

        TypeRepr  rv;
        size_t  cur_ofs = 0;
        size_t  max_align = 1;
        for(const auto& e : ents)
        {
            // Increase offset to fit alignment
            if( !packed && e.align > 0 )
            {
                while( cur_ofs % e.align != 0 )
                {
                    cur_ofs ++;
                }
            }
            max_align = ::std::max(max_align, e.align);

            fields[e.field].offset = cur_ofs;
            if( e.size == SIZE_MAX )
            {
                // Ensure that this is the last item
                ASSERT_BUG(sp, &e == &ents.back(), "Unsized item isn't the last item in " << ty);
                cur_ofs = SIZE_MAX;
            }
            else
            {
                cur_ofs += e.size;
            }
        }
        if( !packed && cur_ofs != SIZE_MAX )
        {
            // Size must be a multiple of alignment
            while( cur_ofs % max_align != 0 )
            {
                cur_ofs ++;
            }
        }
        rv.align = packed ? 1 : max_align;
        rv.size = cur_ofs;
        rv.fields = ::std::move(fields);
        DEBUG("size = " << rv.size << ", align = " << rv.align);
        return box$(rv);
    }


    bool get_nonzero_path(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, TypeRepr::FieldPath& out_path)
    {
        switch(ty.m_data.tag())
        {
        TU_ARM(ty.m_data, Path, te) {
            if( te.binding.is_Struct() )
            {
                const TypeRepr* r = Target_GetTypeRepr(sp, resolve, ty);
                if( !r )
                {
                    return false;
                }
                for(size_t i = 0; i < r->fields.size(); i ++)
                {
                    if( get_nonzero_path(sp, resolve, r->fields[i].ty, out_path) )
                    {
                        out_path.sub_fields.push_back(i);
                        return true;
                    }
                }
                // Handle the NonZero lang item (Note: Checks just the simplepath part)
                if( te.path.m_data.as_Generic().m_path == resolve.m_crate.get_lang_item_path(sp, "non_zero") )
                {
                    out_path.sub_fields.push_back(0);
                    out_path.size = r->size;
                    return true;
                }
            }
            } break;
        TU_ARM(ty.m_data, Borrow, _te) { (void)_te;
            //out_path.sub_fields.push_back(0);
            Target_GetSizeOf(sp, resolve, ty, out_path.size);
            return true;
            } break;
        TU_ARM(ty.m_data, Function, _te) (void)_te;
            //out_path.sub_fields.push_back(0);
            Target_GetSizeOf(sp, resolve, ty, out_path.size);
            return true;
        default:
            break;
        }
        return false;
    }
    ::std::unique_ptr<TypeRepr> make_type_repr_enum(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        const auto& te = ty.m_data.as_Path();
        const auto& enm = *te.binding.as_Enum();

        auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph = [&](const auto& tpl) {
            auto rv = monomorphise_type_with(sp, tpl, monomorph_cb);
            resolve.expand_associated_types(sp, rv);
            return rv;
        };

        TypeRepr  rv;
        switch(enm.m_data.tag())
        {
        case ::HIR::Enum::Class::TAGDEAD:   throw "";
        TU_ARM(enm.m_data, Data, e) {
            ::std::vector<::HIR::TypeRef>   mono_types;
            for(const auto& var : e)
            {
                mono_types.push_back( monomorph(var.type) );
            }
            TypeRepr::FieldPath nz_path;
            if( e.size() == 2 && mono_types[0] == ::HIR::TypeRef::new_unit() && get_nonzero_path(sp, resolve, mono_types[1], nz_path) )
            {
                nz_path.index = 1;
                ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());
                DEBUG("nz_path = " << nz_path.sub_fields);
                size_t  max_size = 0;
                size_t  max_align = 0;
                for(auto& t : mono_types)
                {
                    size_t  size, align;
                    if( !Target_GetSizeAndAlignOf(sp, resolve, t, size, align) )
                    {
                        DEBUG("Generic type in enum - " << t);
                        return nullptr;
                    }
                    if( size == SIZE_MAX ) {
                        BUG(sp, "Unsized type in enum - " << t);
                    }
                    max_size  = ::std::max(max_size , size);
                    max_align = ::std::max(max_align, align);
                    rv.fields.push_back(TypeRepr::Field { 0, mv$(t) });
                }

                rv.size = max_size;
                rv.align = max_align;
                rv.variants = TypeRepr::VariantMode::make_NonZero({ nz_path, 0 });
            }
            else
            {
                size_t  max_size = 0;
                size_t  max_align = 0;
                for(auto& t : mono_types)
                {
                    size_t  size, align;
                    if( !Target_GetSizeAndAlignOf(sp, resolve, t, size, align) )
                    {
                        DEBUG("Generic type in enum - " << t);
                        return nullptr;
                    }
                    if( size == SIZE_MAX ) {
                        BUG(sp, "Unsized type in enum - " << t);
                    }
                    max_size  = ::std::max(max_size , size);
                    max_align = ::std::max(max_align, align);
                    rv.fields.push_back(TypeRepr::Field { 0, mv$(t) });
                }
                DEBUG("max_size = " << max_size << ", max_align = " << max_align);
                // HACK: This is required for the C backend, because the union that contains the enum variants is
                // padded out to align.
                if(max_size > 0)
                {
                    while(max_size % max_align)
                        max_size ++;
                }
                size_t tag_size = 0;
                // TODO: repr(C) enums - they have different rules
                if( mono_types.size() == 0 ) {
                    // Unreachable
                }
                else if( mono_types.size() == 1 ) {
                    // No need for a tag
                }
                else if( mono_types.size() <= 255 ) {
                    rv.fields.push_back(TypeRepr::Field { max_size, ::HIR::CoreType::U8 });
                    tag_size = 1;
                    DEBUG("u8 data tag");
                }
                else {
                    ASSERT_BUG(sp, mono_types.size() <= 0xFFFF, "");
                    while(max_size % 2) max_size ++;
                    rv.fields.push_back(TypeRepr::Field { max_size, ::HIR::CoreType::U16 });
                    tag_size = 2;
                    DEBUG("u16 data tag");
                }
                max_align = ::std::max(max_align, tag_size);
                ::std::vector<uint64_t> vals;
                for(size_t i = 0; i < e.size(); i++)
                {
                    vals.push_back(i);
                }
                if( vals.size() > 1 )
                {
                    rv.variants = TypeRepr::VariantMode::make_Values({ { mono_types.size(), tag_size, {} }, ::std::move(vals) });
                }
                else
                {
                    // Leave the enum with NoVariants
                }
                if( max_align > 0 )
                {
                    // Size must be a multiple of alignment
                    rv.size = (max_size + tag_size);
                    while(rv.size % max_align)
                        rv.size ++;
                    rv.align = max_align;
                }
                else
                {
                    ASSERT_BUG(sp, max_size == 0, "Zero alignment, but non-zero size");
                }
            }
            } break;
        TU_ARM(enm.m_data, Value, e) {
            switch(e.repr)
            {
            case ::HIR::Enum::Repr::C:
                // No auto-sizing, just i32?
                rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U32 });
                break;
            case ::HIR::Enum::Repr::Rust: {
                int pow8 = 0;
                for( const auto& v : e.variants )
                {
                    auto v2 = static_cast<int64_t>(v.val);
                    if( -0x80 <= v2 && v2 < 0x80 )
                    {
                        pow8 = ::std::max(pow8, 1);
                    }
                    else if( -0x8000 <= v2 && v2 < 0x8000 )
                    {
                        pow8 = ::std::max(pow8, 2);
                    }
                    else if( -0x80000000ll <= v2 && v2 < 0x80000000ll )
                    {
                        pow8 = ::std::max(pow8, 3);
                    }
                    else
                    {
                        pow8 = 4;
                    }
                }
                switch(pow8)
                {
                case 0: break;
                case 1:
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::I8 });
                    break;
                case 2:
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::I16 });
                    break;
                case 3:
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::I32 });
                    break;
                case 4:
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::I64 });
                    break;
                default:
                    break;
                }
                } break;
            case ::HIR::Enum::Repr::U8:
                rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U8 });
                break;
            case ::HIR::Enum::Repr::U16:
                rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U16 });
                break;
            case ::HIR::Enum::Repr::U32:
                rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U32 });
                break;
            case ::HIR::Enum::Repr::U64:
                rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U64 });
                break;
            case ::HIR::Enum::Repr::Usize:
                if( g_target.m_arch.m_pointer_bits == 16 )
                {
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U16 });
                }
                else if( g_target.m_arch.m_pointer_bits == 32 )
                {
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U32 });
                }
                else if( g_target.m_arch.m_pointer_bits == 64 )
                {
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U64 });
                }
                break;
            }
            if( rv.fields.size() > 0 )
            {
                // Can't return false or unsized
                Target_GetSizeAndAlignOf(sp, resolve, rv.fields.back().ty, rv.size, rv.align);

                ::std::vector<uint64_t> vals;
                for(const auto& v : e.variants)
                {
                    vals.push_back(v.val);
                }
                rv.variants = TypeRepr::VariantMode::make_Values({ { 0, static_cast<uint8_t>(rv.size), {} }, ::std::move(vals) });
            }
            } break;
        }
        DEBUG("rv.variants = " << rv.variants.tag_str());
        return box$(rv);
    }
    ::std::unique_ptr<TypeRepr> make_type_repr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        TRACE_FUNCTION_F(ty);
        if( TU_TEST1(ty.m_data, Path, .binding.is_Struct()) || ty.m_data.is_Tuple() )
        {
            return make_type_repr_struct(sp, resolve, ty);
        }
        else if( TU_TEST1(ty.m_data, Path, .binding.is_Union()) )
        {
            const auto& te = ty.m_data.as_Path();
            const auto& unn = *te.binding.as_Union();

            auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
            auto monomorph = [&](const auto& tpl) {
                auto rv = monomorphise_type_with(sp, tpl, monomorph_cb);
                resolve.expand_associated_types(sp, rv);
                return rv;
            };

            TypeRepr  rv;
            for(const auto& var : unn.m_variants)
            {
                rv.fields.push_back({ 0, monomorph(var.second.ent) });
                size_t size, align;
                if( !Target_GetSizeAndAlignOf(sp, resolve, rv.fields.back().ty, size, align) )
                {
                    // Generic? - Not good.
                    DEBUG("Generic type encounterd after monomorphise in union - " << rv.fields.back().ty);
                    return nullptr;
                }
                if( size == SIZE_MAX ) {
                    BUG(sp, "Unsized type in union");
                }
                rv.size  = ::std::max(rv.size , size );
                rv.align = ::std::max(rv.align, align);
            }
            return box$(rv);
        }
        else if( TU_TEST1(ty.m_data, Path, .binding.is_Enum()) )
        {
            return make_type_repr_enum(sp, resolve, ty);
        }
        else if( TU_TEST1(ty.m_data, Path, .binding.is_ExternType()) )
        {
            // TODO: Do extern types need anything?
            return nullptr;
        }
        else if( ty.m_data.is_Primitive() )
        {
            return nullptr;
        }
        else if( ty.m_data.is_Borrow() || ty.m_data.is_Pointer() )
        {
            return nullptr;
        }
        else
        {
            TODO(sp, "Type repr for " << ty);
            return nullptr;
        }
    }
}
const TypeRepr* Target_GetTypeRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
{
    // TODO: Thread safety
    // Map of generic types to type representations.
    static ::std::map<::HIR::TypeRef, ::std::unique_ptr<TypeRepr>>  s_cache;

    auto it = s_cache.find(ty);
    if( it != s_cache.end() )
    {
        return it->second.get();
    }

    auto ires = s_cache.insert(::std::make_pair( ty.clone(), make_type_repr(sp, resolve, ty) ));
    return ires.first->second.get();
}
const ::HIR::TypeRef& Target_GetInnerType(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr& repr, size_t idx, const ::std::vector<size_t>& sub_fields, size_t ofs)
{
    const auto& ty = repr.fields.at(idx).ty;
    if( sub_fields.size() == ofs )
    {
        return ty;
    }
    const auto* inner_repr = Target_GetTypeRepr(sp, resolve, ty);
    ASSERT_BUG(sp, inner_repr, "No inner repr for " << ty);
    return Target_GetInnerType(sp, resolve, *inner_repr, sub_fields[ofs], sub_fields, ofs+1);
}
