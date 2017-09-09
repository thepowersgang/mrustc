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

TargetArch ARCH_X86_64 = {
    "x86_64",
    64, false,
    { /*atomic(u8)=*/true, false, true, true,  true }
    };
TargetArch ARCH_X86 = {
    "x86",
    32, false,
    { /*atomic(u8)=*/true, false, true, false,  true }
};
TargetSpec  g_target;

namespace
{
    TargetSpec load_spec_from_file(const ::std::string& filename)
    {
        throw "";
    }
    TargetSpec init_from_spec_name(const ::std::string& target_name)
    {
        if( ::std::ifstream(target_name).is_open() )
        {
            return load_spec_from_file(target_name);
        }
        else if(target_name == "x86_64-linux-gnu")
        {
            return TargetSpec {
                "unix", "linux", "gnu", CodegenMode::Gnu11,
                ARCH_X86_64
                };
        }
        else if(target_name == "x86_64-windows-gnu")
        {
            return TargetSpec {
                "windows", "windows", "gnu", CodegenMode::Gnu11,
                ARCH_X86_64
                };
        }
        else if (target_name == "x86-windows-msvc")
        {
            return TargetSpec {
                "windows", "windows", "msvc", CodegenMode::Msvc,
                ARCH_X86
            };
        }
        //else if (target_name == "x86_64-windows-msvc")
        //{
        //    return TargetSpec {
        //        "windows", "windows", "msvc", CodegenMode::Msvc,
        //        ARCH_X86_64
        //        };
        //}
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
    }
    Cfg_SetValue("target_env", g_target.m_env_name);

    Cfg_SetValue("target_os", g_target.m_os_name);
    Cfg_SetValue("target_pointer_width", FMT(g_target.m_arch.m_pointer_bits));
    Cfg_SetValue("target_endian", g_target.m_arch.m_big_endian ? "big" : "little");
    Cfg_SetValue("target_arch", g_target.m_arch.m_name);
    Cfg_SetValueCb("target_has_atomic", [&](const ::std::string& s) {
        if(s == "8")    return g_target.m_arch.m_atomics.u8;    // Has an atomic byte
        if(s == "ptr")  return g_target.m_arch.m_atomics.ptr;   // Has an atomic pointer-sized value
        return false;
        });
    Cfg_SetValueCb("target_feature", [](const ::std::string& s) {
        return false;
        });
}

bool Target_GetSizeAndAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align)
{
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
            out_align = 1;
            return true;
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::I16:
            out_size = 2;
            out_align = 2;
            return true;
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::Char:
            out_size = 4;
            out_align = 4;
            return true;
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::I64:
            out_size = 8;
            out_align = 8;
            return true;
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::I128:
            out_size = 16;
            // TODO: If i128 is emulated, this can be 8
            out_align = 16;
            return true;
        case ::HIR::CoreType::Usize:
        case ::HIR::CoreType::Isize:
            out_size = g_target.m_arch.m_pointer_bits / 8;
            out_align = g_target.m_arch.m_pointer_bits / 8;
            return true;
        case ::HIR::CoreType::F32:
            out_size = 4;
            out_align = 4;
            return true;
        case ::HIR::CoreType::F64:
            out_size = 8;
            out_align = 8;
            return true;
        case ::HIR::CoreType::Str:
            BUG(sp, "sizeof on a `str` - unsized");
        }
        ),
    (Path,
        // TODO:
        return false;
        ),
    (Generic,
        // Unknown - return false
        return false;
        ),
    (TraitObject,
        BUG(sp, "sizeof on a trait object - unsized");
        ),
    (ErasedType,
        BUG(sp, "sizeof on an erased type - shouldn't exist");
        ),
    (Array,
        // TODO: 
        size_t  size;
        if( !Target_GetSizeAndAlignOf(sp, *te.inner, size,out_align) )
            return false;
        size *= te.size_val;
        ),
    (Slice,
        BUG(sp, "sizeof on a slice - unsized");
        ),
    (Tuple,
        out_size = 0;
        out_align = 0;

        // TODO: Struct reordering
        for(const auto& t : te)
        {
            size_t  size, align;
            if( !Target_GetSizeAndAlignOf(sp, t, size,align) )
                return false;
            if( out_size % align != 0 )
            {
                out_size += align;
                out_size %= align;
            }
            out_size += size;
            out_align = ::std::max(out_align, align);
        }
        ),
    (Borrow,
        // TODO
        ),
    (Pointer,
        // TODO
        ),
    (Function,
        // Pointer size
        out_size = g_target.m_arch.m_pointer_bits / 8;
        out_align = g_target.m_arch.m_pointer_bits / 8;
        return true;
        ),
    (Closure,
        // TODO.
        )
    )
    return false;
}
bool Target_GetSizeOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size)
{
    size_t  ignore_align;
    return Target_GetSizeAndAlignOf(sp, ty, out_size, ignore_align);
}
bool Target_GetAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_align)
{
    size_t  ignore_size;
    return Target_GetSizeAndAlignOf(sp, ty, ignore_size, out_align);
}
