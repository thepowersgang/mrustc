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
#include <hir_conv/main_bindings.hpp>   // ConvertHIR_ConstantEvaluate_Enum
#include <climits>  // UINT_MAX
#include <toml.h>   // tools/common

const TargetArch ARCH_X86_64 = {
    "x86_64",
    64, false,
    TargetArch::Atomics(/*atomic(u8)=*/true, /*atomic(u16)=*/true, /*atomic(u32)=*/true, true,  true),
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 8)
    //TargetArch::Alignments(2, 4, 8, 8, 4, 8, 8) // TODO: Alignment of u128 is 8 with rustc, but gcc uses 16
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
const TargetArch ARCH_POWERPC64 = {
    "powerpc64",
    64, true,
    { /*atomic(u8)=*/true, true, true, true,  true },
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 8)
};
const TargetArch ARCH_POWERPC64LE = {
    "powerpc64",
    64, false,
    { /*atomic(u8)=*/true, true, true, true,  true },
    TargetArch::Alignments(2, 4, 8, 16, 4, 8, 8)
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
        #define BACKEND_C_OPTS_GNU  {"-ffunction-sections", "-pthread"}, {"-Wl,--gc-sections", "-l", "atomic"}
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
                "unix", "linux", "gnu", {CodegenMode::Gnu11, true /*false*/, "x86_64-linux-gnu", BACKEND_C_OPTS_GNU},
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
        else if(target_name == "powerpc64-unknown-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, false, "powerpc64-unknown-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_POWERPC64
                };
        }
        else if(target_name == "powerpc64le-unknown-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", {CodegenMode::Gnu11, false, "powerpc64le-unknown-linux-gnu", BACKEND_C_OPTS_GNU},
                ARCH_POWERPC64LE
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
        else if(target_name == "aarch64-apple-macosx")
        {
            // NOTE: OSX uses Mach-O binaries, which don't fully support the defaults used for GNU targets
            return TargetSpec {
                "unix", "macos", "gnu", {CodegenMode::Gnu11, false, "aarch64-apple-darwin", {}, {}},
                ARCH_ARM64
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

    if( g_target.m_os_name == "macos" )
    {
        Cfg_SetFlag("apple");
        Cfg_SetValue("target_vendor", "apple");
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

    Cfg_SetValue("target_vendor", "");  // NOTE: Doesn't override a pre-set value
    Cfg_SetValue("target_env", g_target.m_env_name);
    Cfg_SetValue("target_os", g_target.m_os_name);
    Cfg_SetValue("target_pointer_width", FMT(g_target.m_arch.m_pointer_bits));
    Cfg_SetValue("target_endian", g_target.m_arch.m_big_endian ? "big" : "little");
    Cfg_SetValue("target_arch", g_target.m_arch.m_name);
    if(g_target.m_arch.m_atomics.u8)    { Cfg_SetValue("target_has_atomic", "8"  ); Cfg_SetValue("target_has_atomic_load_store", "8"  ); Cfg_SetValue("target_has_atomic_equal_alignment", "8"  ); }
    if(g_target.m_arch.m_atomics.u16)   { Cfg_SetValue("target_has_atomic", "16" ); Cfg_SetValue("target_has_atomic_load_store", "16" ); Cfg_SetValue("target_has_atomic_equal_alignment", "16" ); }
    if(g_target.m_arch.m_atomics.u32)   { Cfg_SetValue("target_has_atomic", "32" ); Cfg_SetValue("target_has_atomic_load_store", "32" ); Cfg_SetValue("target_has_atomic_equal_alignment", "32"); }
    if(g_target.m_arch.m_atomics.u64)   { Cfg_SetValue("target_has_atomic", "64" ); Cfg_SetValue("target_has_atomic_load_store", "64" ); Cfg_SetValue("target_has_atomic_equal_alignment", "64"); }
    if(g_target.m_arch.m_atomics.ptr)   { Cfg_SetValue("target_has_atomic", "ptr"); Cfg_SetValue("target_has_atomic_load_store", "ptr"); Cfg_SetValue("target_has_atomic_equal_alignment", "ptr"); }
    // TODO: Atomic compare-and-set option
    if(g_target.m_arch.m_atomics.ptr)   { Cfg_SetValue("target_has_atomic", "cas");  }
    Cfg_SetValueCb("target_feature", [](const ::std::string& s) {
        //if(g_target.m_arch.m_name == "x86_64" && s == "sse2") return true;    // 1.39 ppv-lite86 requires sse2 (x86_64 always has it)
        return false;
        });
}

bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align)
{
    //TRACE_FUNCTION_FR(ty, "size=" << out_size << ", align=" << out_align);
    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Infer, te) {
        BUG(sp, "sizeof on _ type");
        }
    TU_ARMA(Diverge, te) {
        out_size = 0;
        out_align = 0;
        return true;
        }
    TU_ARMA(Primitive, te) {
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
        }
    TU_ARMA(Path, te) {
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
        }
    TU_ARMA(Generic, te) {
        // Unknown - return false
        DEBUG("No repr for Generic - " << ty);
        return false;
        }
    TU_ARMA(TraitObject, te) {
        out_align = 0;
        out_size = SIZE_MAX;
        DEBUG("sizeof on a trait object - unsized");
        return true;
        }
    TU_ARMA(ErasedType, te) {
        BUG(sp, "sizeof on an erased type - shouldn't exist");
        }
    TU_ARMA(Array, te) {
        if( !Target_GetSizeAndAlignOf(sp, resolve, te.inner, out_size,out_align) )
            return false;
        if( out_size == SIZE_MAX )
            BUG(sp, "Unsized type in array - " << ty);
        if( !te.size.is_Known() ) {
            DEBUG("Size unknown - " << ty);
            return false;
        }
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
        }
    TU_ARMA(Slice, te) {
        if( !Target_GetAlignOf(sp, resolve, te.inner, out_align) )
            return false;
        out_size = SIZE_MAX;
        DEBUG("sizeof on a slice - unsized");
        return true;
        }
    TU_ARMA(Tuple, te) {
        const auto* repr = Target_GetTypeRepr(sp, resolve, ty);
        if( !repr )
        {
            DEBUG("Cannot get type repr for " << ty);
            return false;
        }
        out_size  = repr->size;
        out_align = repr->align;
        return true;
        }
    TU_ARMA(Borrow, te) {
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        // TODO: Handle different types of Unsized (ones with different pointer sizes)
        switch(resolve.metadata_type(sp, te.inner))
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
        }
    TU_ARMA(Pointer, te) {
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        switch(resolve.metadata_type(sp, te.inner))
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
        }
    TU_ARMA(Function, te) {
        // Pointer size
        out_size = g_target.m_arch.m_pointer_bits / 8;
        out_align = g_target.m_arch.m_pointer_bits / 8;
        return true;
        }
    TU_ARMA(Closure, te) {
        BUG(sp, "Encountered closure type at trans stage - " << ty);
        }
    TU_ARMA(Generator, te) {
        BUG(sp, "Encountered generator type at trans stage - " << ty);
        }
    }
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
    void set_type_repr(const Span& sp, const ::HIR::TypeRef& ty, ::std::unique_ptr<TypeRepr> repr);

    struct Ent {
        unsigned int field;
        size_t  size;
        size_t  align;
        HIR::TypeRef    ty;
    };
    ::std::ostream& operator<<(std::ostream& os, const Ent& e) {
        os << "Ent { #" << e.field << ": s=" << e.size << " a=" << e.align << " : " << e.ty << " }";
        return os;
    }
    bool struct_enumerate_fields(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, ::std::vector<Ent>& ents)
    {
        const auto& te = ty.data().as_Path();
        const auto& str = *te.binding.as_Struct();
        auto monomorph_cb = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph = [&](const auto& tpl) {
            return resolve.monomorph_expand(sp, tpl, monomorph_cb);
            };
        TU_MATCH_HDRA( (str.m_data), {)
        TU_ARMA(Unit, se) {
            }
        TU_ARMA(Tuple, se) {
            unsigned int idx = 0;
            for(const auto& e : se)
            {
                auto ty = monomorph(e.ent);
                size_t  size, align;
                if( !Target_GetSizeAndAlignOf(sp, resolve, ty, size,align) )
                {
                    DEBUG("Can't get size/align of " << ty);
                    return false;
                }
                DEBUG("#" << idx << ": s=" << size << ",a=" << align << " " << ty);
                ents.push_back(Ent { idx++, size, align, mv$(ty) });
            }
            }
        TU_ARMA(Named, se) {
            unsigned int idx = 0;
            for(const auto& e : se)
            {
                auto ty = monomorph(e.second.ent);
                size_t  size, align;
                if( !Target_GetSizeAndAlignOf(sp, resolve, ty, size,align) )
                {
                    DEBUG("Can't get size/align of " << ty);
                    return false;
                }
                DEBUG("#" << idx << " " << e.first << ": s=" << size << ",a=" << align << " " << ty);
                ents.push_back(Ent { idx++, size, align, mv$(ty) });
            }
            }
        }
        return true;
    }

    enum class StructSorting
    {
        None,
        AllButFinal,
        All,
    };
    // Sort fields with lowest alignment first (and putting smallest fields of equal alignment earlier)
    bool sortfn_struct_fields(const Ent& a, const Ent& b) {
        return a.align != b.align ? a.align < b.align : a.size < b.size;
    }
    /// Generate a struct representation using the provided entries
    /// 
    /// - Handles (optional) sorting and packing
    ::std::unique_ptr<TypeRepr> make_type_repr_struct__inner(const Span&sp, const ::HIR::TypeRef& ty, ::std::vector<Ent>& ents, StructSorting sorting, unsigned forced_alignment, unsigned max_alignment)
    {
        if(ents.size() > 0)
        {
            // Sort in increasing alignment then size
            switch(sorting)
            {
            case StructSorting::None:
                break;
            case StructSorting::AllButFinal:
                ::std::sort(ents.begin(), ents.end() - 1, sortfn_struct_fields);
                break;
            case StructSorting::All:
                ::std::sort(ents.begin(), ents.end(), sortfn_struct_fields);
                break;
            }
        }

        ::std::vector<TypeRepr::Field>  fields(ents.size());

        TypeRepr  rv;
        size_t  cur_ofs = 0;
        size_t  max_align = 1;
        for(auto& e : ents)
        {
            // Increase offset to fit alignment
            auto align = max_alignment > 0 ? std::min<size_t>(e.align, max_alignment) : e.align;
            if( align > 0 )
            {
                while( cur_ofs % align != 0 )
                {
                    cur_ofs ++;
                }
            }
            max_align = ::std::max(max_align, align);

            // Forced padding is indicated by setting the field index to -1
            if( e.field != ~0u )
            {
                ASSERT_BUG(sp, e.field < fields.size(), "Field index out of range");
                ASSERT_BUG(sp, fields[e.field].ty == HIR::TypeRef(), "Dupliate field index");
                fields[e.field].offset = cur_ofs;
                fields[e.field].ty = e.ty.clone();
            }
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
        if(forced_alignment > 0) {
            max_align = std::max(max_align, static_cast<size_t>(forced_alignment));
        }
        // If not packing (and the size isn't infinite/unsized) then round the size up to the alignment
        if( cur_ofs != SIZE_MAX )
        {
            // Size must be a multiple of alignment
            while( cur_ofs % max_align != 0 )
            {
                cur_ofs ++;
            }
        }
        // Aligment is 1 for packed structs, and `max_align` otherwise
        rv.align = max_align;
        rv.size = cur_ofs;
        rv.fields = ::std::move(fields);
        DEBUG(ty << ": size = " << rv.size << ", align = " << rv.align);
        return box$(rv);
    }

    // Returns NULL when the repr can't be determined
    ::std::unique_ptr<TypeRepr> make_type_repr_struct(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        TRACE_FUNCTION_F(ty);
        ::std::vector<Ent>  ents;
        StructSorting   sorting;
        unsigned forced_alignment = 0;
        unsigned max_alignment = 0;
        if( ty.data().is_Path() && ty.data().as_Path().binding.is_Struct() )
        {
            const auto& te = ty.data().as_Path();
            const auto& str = *te.binding.as_Struct();

            if( !struct_enumerate_fields(sp, resolve, ty, ents) )
                return nullptr;

            forced_alignment = str.m_forced_alignment;
            max_alignment = str.m_max_field_alignment;
            sorting = StructSorting::None;  // Defensive default for if repr is invalid
            switch(str.m_repr)
            {
            case ::HIR::Struct::Repr::C:
            case ::HIR::Struct::Repr::Simd:
                // No sorting, no packing
                sorting = StructSorting::None;
                break;
            case ::HIR::Struct::Repr::Transparent:
            case ::HIR::Struct::Repr::Rust:
                if( str.m_struct_markings.dst_type != HIR::StructMarkings::DstType::None )
                {
                    sorting = StructSorting::AllButFinal;
                }
                else
                {
                    sorting = StructSorting::All;
                }
                break;
            }
        }
        else if( const auto* te = ty.data().opt_Tuple() )
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
                ents.push_back(Ent { idx++, size, align, t.clone() });
            }
            sorting = StructSorting::All;
        }
        else
        {
            BUG(sp, "Unexpected type in creating type repr - " << ty);
        }

        return make_type_repr_struct__inner(sp, ty, ents, sorting, forced_alignment, max_alignment);
    }


    bool get_nonzero_path(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, TypeRepr::FieldPath& out_path)
    {
        switch(ty.data().tag())
        {
        TU_ARM(ty.data(), Path, te) {
            if( te.binding.is_Struct() )
            {
                const auto* str = te.binding.as_Struct();
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
                // TODO: 1.39 marks these with #[rustc_nonnull_optimization_guaranteed] instead
                if(str->m_struct_markings.is_nonzero)
                {
                    out_path.sub_fields.push_back(0);
                    out_path.size = r->size;
                    return true;
                }

                if( gTargetVersion <= TargetVersion::Rustc1_29 )
                {
                    // Handle the NonZero lang item (Note: Checks just the simplepath part)
                    if( te.path.m_data.as_Generic().m_path == resolve.m_crate.get_lang_item_path(sp, "non_zero") )
                    {
                        out_path.sub_fields.push_back(0);
                        out_path.size = r->size;
                        return true;
                    }
                }
            }
            } break;
        TU_ARM(ty.data(), Borrow, _te) { (void)_te;
            //out_path.sub_fields.push_back(0);
            Target_GetSizeOf(sp, resolve, ty, out_path.size);
            return true;
            } break;
        TU_ARM(ty.data(), Function, _te) (void)_te;
            //out_path.sub_fields.push_back(0);
            Target_GetSizeOf(sp, resolve, ty, out_path.size);
            return true;
        default:
            break;
        }
        return false;
    }

    size_t get_size_or_zero(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty) {
        size_t size = 0;
        Target_GetSizeOf(sp, resolve, ty, size);
        return size;
    }

    size_t get_offset(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr* r, const TypeRepr::FieldPath& out_path)
    {
        assert(out_path.index < r->fields.size());
        size_t ofs = r->fields[out_path.index].offset;

        r = Target_GetTypeRepr(sp, resolve, r->fields[out_path.index].ty);
        for(const auto& f : out_path.sub_fields)
        {
            assert(f < r->fields.size());
            ofs += r->fields[f].offset;
            r = Target_GetTypeRepr(sp, resolve, r->fields[f].ty);
        }

        return ofs;
    }

    /// <summary>
    /// Locate a suitable niche location in the given path (an enum that has space in its tag)
    /// </summary>
    /// <param name="sp"></param>
    /// <param name="resolve"></param>
    /// <param name="ty"></param>
    /// <param name="out_path">Path to the variant field</param>
    /// <returns>zero for no niche found, or the number of entries already used in the niche</returns>
    unsigned get_variant_niche_path(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t min_offset, size_t max_offset, TypeRepr::FieldPath& out_path)
    {
        TRACE_FUNCTION_F(ty << " min_offset=" << min_offset << " max_offset=" << max_offset);
        switch(ty.data().tag())
        {
        TU_ARM(ty.data(), Tuple, te) {
            const TypeRepr* r = Target_GetTypeRepr(sp, resolve, ty);
            if( !r )
            {
                return 0;
            }

            for(size_t i = 0; i < r->fields.size(); i ++)
            {
                const auto& f = r->fields[i];
                auto size = get_size_or_zero(sp, resolve, f.ty);
                DEBUG(i << ": " << f.offset << " + " << size);
                if( f.offset >= max_offset )
                {
                    continue ;
                }
                else if( f.offset + size > min_offset )
                {
                    if( auto rv = get_variant_niche_path(sp, resolve, f.ty, (f.offset < min_offset ? min_offset - f.offset : 0), max_offset - f.offset, out_path) )
                    {
                        out_path.sub_fields.push_back(i);
                        return rv;
                    }
                }
            }
            }
        TU_ARM(ty.data(), Path, te) {
            if( te.binding.is_Struct() )
            {
                const auto* str = te.binding.as_Struct();
                const TypeRepr* r = Target_GetTypeRepr(sp, resolve, ty);
                if( !r )
                {
                    return 0;
                }

                // Handle bounded
                if(str->m_struct_markings.bounded_max)
                {
                    if( str->m_struct_markings.bounded_max_value >= UINT_MAX )
                    {
                        return 0;
                    }
                    if( min_offset != 0 )
                    {
                        return 0;
                    }
                    DEBUG("Max bounded");
                    assert(r->fields.size() >= 1);
                    assert(r->fields[0].offset == 0);
                    auto size = get_size_or_zero(sp, resolve, r->fields[0].ty);
                    if( size > max_offset )
                        return 0;
                    out_path.sub_fields.push_back(0);
                    out_path.size = size;
                    return str->m_struct_markings.bounded_max_value + 1;
                }

                for(size_t i = 0; i < r->fields.size(); i ++)
                {
                    const auto& f = r->fields[i];
                    auto size = get_size_or_zero(sp, resolve, f.ty);
                    DEBUG(i << ": " << f.offset << " + " << size);
                    if( f.offset >= max_offset )
                    {
                        continue ;
                    }
                    else if( f.offset + size > min_offset )
                    {
                        if( auto rv = get_variant_niche_path(sp, resolve, f.ty, (f.offset < min_offset ? min_offset - f.offset : 0), max_offset - f.offset, out_path) )
                        {
                            out_path.sub_fields.push_back(i);
                            return rv;
                        }
                    }
                }
            }
            else if( te.binding.is_Enum() )
            {
                const TypeRepr* r = Target_GetTypeRepr(sp, resolve, ty);
                if( !r )
                {
                    return 0;
                }

                TU_MATCH_HDRA( (r->variants), { )
                TU_ARMA(None, ve) {
                    // If there is no discriminator, recurse into the only field
                    if( r->fields.empty() ) {
                        return 0;
                    }
                    else {
                        auto rv = get_variant_niche_path(sp, resolve, r->fields[0].ty, min_offset, max_offset, out_path);
                        if(rv) {
                            out_path.sub_fields.push_back(0);
                        }
                        return rv;
                    }
                    }
                TU_ARMA(Linear, ve) {
                    // Check that the offset of this tag field is >= min_offset
                    auto ofs = get_offset(sp, resolve, r, ve.field);
                    DEBUG("Linear - Tag offset: " << ofs);
                    if(min_offset <= ofs && ofs + ve.field.size < max_offset)
                    {
                        out_path.size = ve.field.size;
                        out_path.sub_fields.clear();
                        out_path.sub_fields.insert(out_path.sub_fields.begin(), ve.field.sub_fields.rbegin(), ve.field.sub_fields.rend());
                        out_path.sub_fields.push_back(ve.field.index);
                        return ve.offset + ve.num_variants; // NOTE: The niche variant leaves hole in the values.
                    }
                    }
                TU_ARMA(Values, ve) {
                    auto ofs = get_offset(sp, resolve, r, ve.field);
                    DEBUG("Values - Tag offset: " << ofs);
                    if(min_offset <= ofs && ofs + ve.field.size < max_offset)
                    {
                        auto last_value = *std::max_element(ve.values.begin(), ve.values.end());
                        if(last_value < UINT_MAX)
                        {
                            out_path.size = ve.field.size;
                            out_path.sub_fields.clear();
                            out_path.sub_fields.insert(out_path.sub_fields.begin(), ve.field.sub_fields.rbegin(), ve.field.sub_fields.rend());
                            out_path.sub_fields.push_back(ve.field.index);
                            return last_value + 1;
                        }
                    }
                    return 0;
                    }
                TU_ARMA(NonZero, _ve) {
                    DEBUG("Non-zero enum, can't niche");
                    return 0;
                    }
                }
            }
            } break;
        TU_ARM(ty.data(), Primitive, te) {
            switch(te)
            {
            case ::HIR::CoreType::Char:
                // Only valid if the min offset is zero
                if( min_offset == 0 && max_offset >= 4 )
                {
                    out_path.size = 4;
                    return 0x10FFFF + 1;
                }
                break;
            case ::HIR::CoreType::Bool:
                if( min_offset == 0 && max_offset >= 1 )
                {
                    out_path.size = 1;
                    return 2;
                }
                break;
            default:
                break;
            }
            }
        default:
            break;
        }
        return 0;
    }
    ::std::unique_ptr<TypeRepr> make_type_repr_enum(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        TRACE_FUNCTION_F(ty);
        const auto& te = ty.data().as_Path();
        const auto& enm = *te.binding.as_Enum();

        auto monomorph_cb = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph = [&](const auto& tpl) {
            return resolve.monomorph_expand(sp, tpl, monomorph_cb);
        };

        TypeRepr  rv;
        switch(enm.m_data.tag())
        {
        case ::HIR::Enum::Class::TAGDEAD:   throw "";
        TU_ARM(enm.m_data, Data, e) {

            // repr(C) enums - they have different rules
            // - A data enum with `repr(C)` puts the tag before the data
            if( enm.m_is_c_repr )
            {
                size_t  max_size = 0;
                size_t  max_align = 0;
                for(const auto& var : e)
                {
                    auto t = monomorph(var.type);
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


                rv.fields.push_back(TypeRepr::Field { 0, enm.get_repr_type(enm.m_tag_repr) });
                size_t tag_size, tag_align;
                Target_GetSizeAndAlignOf(sp, resolve, rv.fields.back().ty,  tag_size, tag_align);
                size_t data_ofs = tag_size;

                while(data_ofs % max_align != 0)
                    data_ofs ++;

                for(size_t i = 0; i < e.size(); i ++)
                {
                    rv.fields[i].offset = data_ofs;
                }
                rv.size = data_ofs + max_size;
                rv.align = std::max(tag_align, max_align);
                while(rv.size % rv.align != 0)
                    rv.size ++;
                rv.variants = TypeRepr::VariantMode::make_Linear({ { e.size(), tag_size, {} }, 0, e.size() });
            }
            else if( enm.m_tag_repr == ::HIR::Enum::Repr::Auto && e.size() <= 1 )
            {
                // If there are not multiple variants, then only include the one body
                if(e.size() == 1)
                {
                    auto t = monomorph(e[0].type);
                    const auto* inner_repr = Target_GetTypeRepr(sp, resolve, t);
                    rv.fields.push_back(TypeRepr::Field { 0, mv$(t) });
                    rv.size = inner_repr->size;
                    rv.align = inner_repr->align;
                }
                else
                {
                    rv.size = 0;
                    rv.align = 0;
                }
                // Just leave it as None
                //rv.variants = TypeRepr::VariantMode::make_None({});
            }
            else {
                TRACE_FUNCTION_F("repr(Rust)");
                // repr(Rust) - allows interesting optimisations
                struct Variant {
                    ::HIR::TypeRef  type;
                    ::std::vector<Ent>  ents;
                };
                std::vector< Variant>    variants;
                variants.reserve( e.size() );
                for(const auto& var : e)
                {
                    variants.push_back({ monomorph(var.type), {} });
                    TRACE_FUNCTION_F("");
                    if( var.type == ::HIR::TypeRef::new_unit() ) {
                        continue ;
                    }
                    if( !struct_enumerate_fields(sp, resolve, variants.back().type, variants.back().ents) ) {
                        DEBUG("Generic type in enum - " << variants.back().type);
                        return nullptr;
                    }
                    DEBUG(variants.back().type << ": " << variants.back().ents);
                }

                if( enm.m_tag_repr == ::HIR::Enum::Repr::Auto )
                {
                    // Non-zero optimisation
                    if( rv.variants.is_None() )
                    {
                        if( e.size() == 2 && (e[0].type == ::HIR::TypeRef::new_unit() || e[1].type == ::HIR::TypeRef::new_unit()) )
                        {
                            // Check for a non-zero path in any of those
                            unsigned nz_var = (e[0].type == ::HIR::TypeRef::new_unit() ? 1 : 0);
                            for( size_t i = 0; i < variants[nz_var].ents.size(); i ++ )
                            {
                                TypeRepr::FieldPath nz_path;
                                if( get_nonzero_path(sp, resolve, variants[nz_var].ents[i].ty, nz_path) )
                                {
                                    nz_path.sub_fields.push_back(i);
                                    nz_path.index = nz_var;
                                    ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());
                                    DEBUG("nz_path = " << nz_path.sub_fields);

                                    size_t size0, size1;
                                    size_t align0, align1;
                                    Target_GetSizeAndAlignOf(sp, resolve, variants[0].type, size0, align0);
                                    Target_GetSizeAndAlignOf(sp, resolve, variants[1].type, size1, align1);
                                    rv.size = std::max(size0, size1);
                                    rv.align = std::max(align0, align1);
                                    rv.fields.push_back({ 0, std::move(variants[0].type) });
                                    rv.fields.push_back({ 0, std::move(variants[1].type) });
                                    rv.variants = TypeRepr::VariantMode::make_NonZero({ nz_path, 1 - nz_var });
                                    break;
                                }
                            }
                        }
                    }   // non-zero

                    // Niche optimisation
                    // - Find an inner enum or char, and use high values for the variant
                    if( rv.variants.is_None() )
                    {
                        bool niche_before_data = false;
                        size_t niche_offset = 0;
                        size_t non_niche_offset = 0;
                        // Find the largest variant
                        // - Also get the next-largest size to use as the minimum tag offset
                        unsigned n_match = 0;
                        size_t biggest_var = variants.size();
                        size_t max_var_size = 0;
                        size_t min_offset = 0;
                        size_t max_align = 1;
                        std::vector< std::unique_ptr<TypeRepr> >    reprs;
                        for( size_t i = 0; i < variants.size(); i ++ )
                        {
                            reprs.push_back( make_type_repr_struct__inner(sp, e[i].type, variants[i].ents, StructSorting::All, 0,0) );
                            max_align = std::max(max_align, reprs.back()->align);
                            size_t var_size = reprs.back()->size;
                            // If larger than current max, update current max and reset
                            if( var_size > max_var_size ) {
                                min_offset = max_var_size;  // Downgrade the previous to min_offset
                                max_var_size = var_size;
                                biggest_var = i;
                                n_match = 1;
                            }
                            // If equal to current max, increment count
                            else if( var_size == max_var_size ) {
                                n_match += 1;
                            }
                            // Otherwise (smaller) update the min offset
                            else {
                                min_offset = std::max(min_offset, var_size);
                            }
                        }
                        DEBUG("Niche optimisation: max_var_size=" << max_var_size << " n_match=" << n_match << " biggest_var=" << biggest_var << " min_offset=" << min_offset);

                        if( n_match == 1 )
                        {
                            for(size_t i = 0; i < reprs[biggest_var]->fields.size(); i ++)
                            {
                                const auto& fld = reprs[biggest_var]->fields[i];

                                // 1. Look for a tag at the end
                                // - Prefer the end-of-struct version, as it avoids adding fields to the other variants
                                TypeRepr::FieldPath nz_path;
                                if( auto offset = get_variant_niche_path(sp, resolve, fld.ty, (min_offset > fld.offset ? min_offset - fld.offset : 0), max_var_size, nz_path) )
                                {
                                    size_t max_var = 0;
                                    switch( nz_path.size )
                                    {
                                    case 1: max_var = 0xFF;  break;
                                    case 2: max_var = 0xFFFF;  break;
                                    case 4: max_var = 0xFFFFFFFF;  break;
                                    case 8: max_var = 0xFFFFFFFF;  break;   // Just assume 2^32 here
                                    }

                                    if( offset <= max_var && offset + e.size() <= max_var )
                                    {
                                        nz_path.sub_fields.push_back(i);
                                        nz_path.index = biggest_var;
                                        ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());
                                        DEBUG("Niche optimisation (trailing): offset=" << offset << " path=" << nz_path);

                                        assert(rv.variants.is_None());
                                        rv.variants = TypeRepr::VariantMode::make_Linear({ std::move(nz_path), offset, e.size() });
                                        break ;
                                    }
                                    else
                                    {
                                        if(debug_enabled()) {
                                            nz_path.sub_fields.push_back(i);
                                            nz_path.index = biggest_var;
                                        }
                                        DEBUG("Out of space in this niche: " << (offset+e.size()) << " > " << max_var << " (path=" << nz_path << ")");
                                    }
                                }

                                // Note: rustc doesn't do this.
#if 1
                                // 2. Look for a possible tag at the start?
                                // - Prepending the tag might change the next-largest variant too much?
                                if( fld.offset == 0 )
                                {
                                    TypeRepr::FieldPath nz_path;
                                    if( auto offset = get_variant_niche_path(sp, resolve, fld.ty, 0, max_var_size - min_offset, nz_path) )
                                    {
                                        size_t max_var = 0;
                                        switch( nz_path.size )
                                        {
                                        case 1: max_var = 0xFF;  break;
                                        case 2: max_var = 0xFFFF;  break;
                                        case 4: max_var = 0xFFFFFFFF;  break;
                                        case 8: max_var = 0xFFFFFFFF;  break;   // Just assume 2^32 here
                                        }

                                        if( offset <= max_var && offset + e.size() <= max_var )
                                        {
                                            // TODO: Get the niche offset, store so structure updating can add it...
                                            nz_path.index = i;
                                            ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());
                                            niche_offset = get_offset(sp, resolve, &*reprs[biggest_var], nz_path);
                                            if(niche_offset != 0) {
                                                // - For now, only accept zero offsets
                                                DEBUG("Ignore niche not at the start of the struture");
                                                continue ;
                                            }
                                            ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());

                                            nz_path.sub_fields.push_back(i);
                                            nz_path.index = biggest_var;
                                            ::std::reverse(nz_path.sub_fields.begin(), nz_path.sub_fields.end());
                                            DEBUG("Niche optimisation (leading): linear offset=" << offset << " path=" << nz_path << " @byte " << niche_offset);

                                            niche_before_data = true;
                                            non_niche_offset = nz_path.size;
                                            assert(rv.variants.is_None());
                                            rv.variants = TypeRepr::VariantMode::make_Linear({ std::move(nz_path), offset, e.size() });
                                            break ;
                                        }
                                        else
                                        {
                                            DEBUG("Out of space in this niche: " << (offset+e.size()) << " > " << max_var);
                                        }
                                    }
                                }
#endif
                            }
                        }

                        // Fix overall size
                        size_t max_size = max_var_size;
                        while( max_size % max_align != 0 )
                            max_size ++;

                        if( !rv.variants.is_None() )
                        {
                            const auto& niche_path = rv.variants.as_Linear().field;

                            ::HIR::TypeRef  niche_ty;
                            if( niche_before_data ) {
                                switch( niche_path.size )
                                {
                                case 1: niche_ty = ::HIR::CoreType::U8 ;    break;
                                case 2: niche_ty = ::HIR::CoreType::U16;    break;
                                case 4: niche_ty = ::HIR::CoreType::U32;    break;
                                case 8: niche_ty = ::HIR::CoreType::U64;    break;
                                default:    BUG(sp, "Unknown niche size: " << niche_path);
                                }
                            }
                            // Generate raw struct reprs for all variants
                            // - Add `non_niche_offset` to all variants
                            assert(reprs.size() == variants.size());
                            for(size_t i = 0; i < reprs.size(); i ++)
                            {
                                if( e[i].type != HIR::TypeRef::new_unit() )
                                {
                                    // If the tag is leading, then add to all other variants and update reprs
                                    if( niche_before_data && i != biggest_var )
                                    {
                                        // Add padding (if needed)
                                        if( niche_offset > 0 )
                                        {
                                            variants[i].ents.insert( variants[i].ents.begin(), Ent() );
                                            variants[i].ents[0].align = 1;
                                            variants[i].ents[0].size = niche_offset;
                                            variants[i].ents[0].field = ~0u;
                                            // Leave no type
                                            TODO(sp, "Handle adding padding");
                                        }
                                        // Add the tag
                                        variants[i].ents.insert( variants[i].ents.begin(), Ent() );
                                        variants[i].ents[0].align = niche_path.size;
                                        variants[i].ents[0].size = niche_path.size;
                                        variants[i].ents[0].field = variants[i].ents.size() - 1;
                                        variants[i].ents[0].ty = niche_ty.clone();
                                        // Create the new repr
                                        reprs[i] = make_type_repr_struct__inner(sp, variants[i].type, variants[i].ents, StructSorting::None, 0,0);
                                        // Make sure that the newly calculated repr doesn't change the size/alignment
                                        assert(reprs[i]->size <= max_size);
                                        assert(reprs[i]->align <= max_align);
                                    }
                                    set_type_repr(sp, variants[i].type, std::move(reprs[i]));
                                }
                                else
                                {
                                    // Note: unit type (any empty type) doesn't need the tag added
                                    // NOTE: Unit type should already have a repr, but make sure
                                    Target_GetTypeRepr(sp, resolve, variants[i].type);
                                }
                                rv.fields.push_back(TypeRepr::Field { 0, mv$(variants[i].type) });
                            }

                            rv.size = max_size;
                            rv.align = max_align;

                            // Ensure that the tag offset is still valid
                            auto tag_offset = get_offset(sp, resolve, &rv, niche_path);
                            if( non_niche_offset != 0 ) {
                                ASSERT_BUG(sp, tag_offset < non_niche_offset, "Niche offset invalid: " << tag_offset << " >= " << non_niche_offset);
                            }
                            else {
                                ASSERT_BUG(sp, tag_offset >= min_offset, "Niche offset invalid: " << tag_offset << " < " << min_offset );
                            }
                        }
                    } // Niche optimisation
                } // All optimisations

                // rustc-compatible enum repr
                // ```
                // union {
                //   struct {
                //      TagType tag;
                //      ...data
                //   } var1;
                // }
                // ```
                if( rv.variants.is_None() )
                {
                    ::HIR::TypeRef  tag_ty;
                    // If the tag size is specified, then force that
                    if( enm.m_tag_repr != HIR::Enum::Repr::Auto )
                    {
                        tag_ty = enm.get_repr_type(enm.m_tag_repr);
                    }
                    else
                    {
                        if( e.size() <= 1 ) {
                            // Unreachable
                            BUG(sp, "Reached auto tag type logic with zero/one-sized enum");
                        }
                        else if( e.size() <= 255 ) {
                            tag_ty = ::HIR::CoreType::U8;
                            DEBUG("u8 data tag");
                        }
                        else if( e.size() <= UINT16_MAX ) {
                            tag_ty = ::HIR::CoreType::U16;
                        }
                        else {
                            ASSERT_BUG(sp, e.size() <= UINT32_MAX, "");
                            tag_ty = ::HIR::CoreType::U32;
                        }
                    }

                    size_t tag_size;
                    size_t tag_align;
                    Target_GetSizeAndAlignOf(sp, resolve, tag_ty,  tag_size,tag_align);
                    size_t max_size = tag_size;
                    size_t max_align = tag_align;
                    // Sort all varaint fields (fully)
                    // Add the tag to the start of all variants
                    // Generate a struct repr (with sorting off)
                    for(size_t var_i = 0; var_i < variants.size(); var_i ++)
                    {
                        auto& ents = variants[var_i].ents;
                        auto& var_ty = variants[var_i].type;
                        if( e[var_i].type != HIR::TypeRef::new_unit() )
                        {
                            // - Sort
                            ::std::sort(ents.begin(), ents.end(), sortfn_struct_fields);
                            // - Add tag
                            ents.insert(ents.begin(), Ent());
                            ents[0].align = tag_size;
                            ents[0].size = tag_align;
                            ents[0].field = ents.size() - 1;
                            ents[0].ty = tag_ty.clone();

                            // - Create repr and assign
                            auto repr = make_type_repr_struct__inner(sp, var_ty, ents, StructSorting::None, 0,0);
                            max_size  = std::max(max_size , repr->size );
                            max_align = std::max(max_align, repr->align);
                            set_type_repr(sp, var_ty, std::move(repr));
                        }

                        // - Push the field
                        rv.fields.push_back(TypeRepr::Field { 0, mv$(var_ty) });
                    }
                    rv.fields.push_back(TypeRepr::Field { 0, mv$(tag_ty) });


                    // Size must be a multiple of alignment
                    rv.size = max_size;
                    while(rv.size % max_align != 0)
                        rv.size ++;
                    rv.align = max_align;

                    rv.variants = TypeRepr::VariantMode::make_Linear({ { e.size(), tag_size, {} }, 0, e.size() });
                }
            }
            } break;
        TU_ARM(enm.m_data, Value, e) {
            // TODO: If the values aren't yet populated, force const evaluation
            if(!e.evaluated) {
                ConvertHIR_ConstantEvaluate_Enum(resolve.m_crate, te.path.m_data.as_Generic().m_path, enm);
                assert(e.evaluated);
            }
            switch(enm.m_tag_repr)
            {
            case ::HIR::Enum::Repr::Auto:
                if(enm.m_is_c_repr)
                {
                    // No auto-sizing, just i32?
                    rv.fields.push_back(TypeRepr::Field { 0, ::HIR::CoreType::U32 });
                }
                else
                {
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
            default:
                rv.fields.push_back(TypeRepr::Field { 0, enm.get_repr_type(enm.m_tag_repr) });
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
                DEBUG("vals = " << vals);
                rv.variants = TypeRepr::VariantMode::make_Values({ { 0, static_cast<uint8_t>(rv.size), {} }, ::std::move(vals) });
            }
            } break;
        }

        TU_MATCH_HDRA( (rv.variants), { )
        TU_ARMA(None, e) {
            DEBUG("rv.variants = None");
            }
        TU_ARMA(Linear, e) {
            DEBUG("rv.variants = Linear {"
                << " field=" << e.field
                << " value " << e.offset << "+" << e.num_variants
                << " }");
            }
        TU_ARMA(Values, e) {
            DEBUG("rv.variants = Values {"
                << " field=" << e.field
                << " values " << e.values
                << " }");
            }
        TU_ARMA(NonZero, e) {
            DEBUG("rv.variants = NonZero {"
                << " field=" << e.field
                << " zero_variant=" << e.zero_variant
                << " }");
            }
        }
        return box$(rv);
    }
    ::std::unique_ptr<TypeRepr> make_type_repr_union(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        const auto& te = ty.data().as_Path();
        const auto& unn = *te.binding.as_Union();

        auto monomorph_cb = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph = [&](const auto& tpl) {
            return resolve.monomorph_expand(sp, tpl, monomorph_cb);
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
        // Round the size to be a multiple of align
        if( rv.size % rv.align != 0 )
        {
            rv.size += rv.align - rv.size % rv.align;
        }
        return box$(rv);
    }
    ::std::unique_ptr<TypeRepr> make_type_repr_(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        switch(ty.data().tag())
        {
        case ::HIR::TypeData::TAGDEAD:  abort();
        case ::HIR::TypeData::TAG_Tuple:
            return make_type_repr_struct(sp, resolve, ty);
        case ::HIR::TypeData::TAG_Path:
            switch( ty.data().as_Path().binding.tag() )
            {
            case ::HIR::TypePathBinding::TAGDEAD:  abort();
            case ::HIR::TypePathBinding::TAG_Struct:
                return make_type_repr_struct(sp, resolve, ty);
            case ::HIR::TypePathBinding::TAG_Union:
                return make_type_repr_union(sp, resolve, ty);
            case ::HIR::TypePathBinding::TAG_Enum:
                return make_type_repr_enum(sp, resolve, ty);
            case ::HIR::TypePathBinding::TAG_ExternType:
                // TODO: Do extern types need anything?
                return nullptr;
            case ::HIR::TypePathBinding::TAG_Opaque:
            case ::HIR::TypePathBinding::TAG_Unbound:
                BUG(sp, "Encountered invalid type in make_type_repr - " << ty);
            }
            throw "unreachable";
        // TODO: Why is `make_type_repr` being called on these?
        case ::HIR::TypeData::TAG_Primitive:
        case ::HIR::TypeData::TAG_Borrow:
        case ::HIR::TypeData::TAG_Pointer:
            return nullptr;
        default:
            TODO(sp, "Type repr for " << ty);
        }
    }
    ::std::unique_ptr<TypeRepr> make_type_repr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        ::std::unique_ptr<TypeRepr> rv;
        TRACE_FUNCTION_FR(ty, ty << " " << FMT_CB(ss, if(rv) { ss << "size=" << rv->size << ", align=" << rv->align; } else { ss << "NONE"; }));
        rv = make_type_repr_(sp, resolve, ty);
        return rv;
    }

    // TODO: Thread safety on this cache?
    static ::std::map<::HIR::TypeRef, ::std::unique_ptr<TypeRepr>>  s_cache;

    void set_type_repr(const Span& sp, const ::HIR::TypeRef& ty, ::std::unique_ptr<TypeRepr> repr)
    {
        auto ires = s_cache.insert(::std::make_pair( ty.clone(), mv$(repr) ));
        ASSERT_BUG(sp, ires.second, "set_type_repr called for type that already has a repr: " << ty);
        DEBUG("Set repr for " << ires.first->first);
    }
}
const TypeRepr* Target_GetTypeRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
{
    auto it = s_cache.find(ty);
    if( it != s_cache.end() )
    {
        return it->second.get();
    }

    auto ires = s_cache.insert(::std::make_pair( ty.clone(), make_type_repr(sp, resolve, ty) ));
    if(ires.second)
    {
        DEBUG("Created repr for " << ires.first->first);
    }
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

size_t TypeRepr::get_offset(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr::FieldPath& path) const
{
    const auto* r = this;
    assert(path.index < r->fields.size());
    size_t ofs = r->fields[path.index].offset;

    const auto* ty = &r->fields[path.index].ty;
    for(const auto& f : path.sub_fields)
    {
        r = Target_GetTypeRepr(sp, resolve, *ty);
        assert(r);  // We have an outer repr, so inner must exist
        assert(f < r->fields.size());
        ofs += r->fields[f].offset;
        ty = &r->fields[f].ty;
    }

    return ofs;
}

std::pair<unsigned,bool> TypeRepr::get_enum_variant(const Span& sp, const StaticTraitResolve& resolve, const EncodedLiteralSlice& lit) const
{
    unsigned var_idx = 0;
    bool sub_has_tag = false;
    TU_MATCH_HDRA( (this->variants), {)
    TU_ARMA(None, ve) {
        }
    TU_ARMA(Linear, ve) {
        auto v = lit.slice( this->get_offset(sp, resolve, ve.field), ve.field.size).read_uint(ve.field.size);
        if( v < ve.offset ) {
            var_idx = ve.field.index;
            sub_has_tag = false; // TODO: is this correct?
            DEBUG("VariantMode::Linear - Niche #" << var_idx);
        }
        else {
            var_idx = v - ve.offset;
            sub_has_tag = true;
            DEBUG("VariantMode::Linear - Other #" << var_idx);
        }
        }
    TU_ARMA(Values, ve) {
        auto v = lit.slice( this->get_offset(sp, resolve, ve.field), ve.field.size).read_uint(ve.field.size);
        auto it = std::find(ve.values.begin(), ve.values.end(), v);
        ASSERT_BUG(sp, it != ve.values.end(), "Invalid enum tag: " << v);
        var_idx = it - ve.values.begin();
        DEBUG("VariantMode::Values - #" << var_idx);
        }
    TU_ARMA(NonZero, ve) {
        size_t ofs = this->get_offset(sp, resolve, ve.field);
        bool is_nonzero = false;
        for(size_t i = 0; i < ve.field.size; i ++) {
            if( lit.slice(ofs+i, 1).read_uint(1) != 0 ) {
                is_nonzero = true;
                break;
            }
        }

        var_idx = (is_nonzero ? 1 - ve.zero_variant : ve.zero_variant);
        DEBUG("VariantMode::NonZero - #" << var_idx);
        }
    }
    return std::make_pair(var_idx, sub_has_tag);
}
