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


bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align);

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

namespace {
    // Returns NULL when the repr can't be determined
    ::std::unique_ptr<StructRepr> make_struct_repr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        ::std::vector<StructRepr::Ent>  ents;
        bool packed = false;
        bool allow_sort = false;
        if( const auto* te = ty.m_data.opt_Path() )
        {
            const auto& str = *te->binding.as_Struct();
            auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &te->path.m_data.as_Generic().m_params, nullptr);
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
                        return nullptr;
                    ents.push_back(StructRepr::Ent { idx++, size, align, mv$(ty) });
                }
                ),
            (Named,
                unsigned int idx = 0;
                for(const auto& e : se)
                {
                    auto ty = monomorph(e.second.ent);
                    size_t  size, align;
                    if( !Target_GetSizeAndAlignOf(sp, resolve, ty, size,align) )
                        return nullptr;
                    ents.push_back(StructRepr::Ent { idx++, size, align, mv$(ty) });
                }
                )
            )
            switch(str.m_repr)
            {
            case ::HIR::Struct::Repr::Packed:
                packed = true;
                TODO(sp, "make_struct_repr - repr(packed)");    // needs codegen help
                break;
            case ::HIR::Struct::Repr::C:
                // No sorting, no packing
                break;
            case ::HIR::Struct::Repr::Rust:
                allow_sort = true;
                break;
            }
        }
        else if( const auto* te = ty.m_data.opt_Tuple() )
        {
            unsigned int idx = 0;
            for(const auto& t : *te)
            {
                size_t  size, align;
                if( !Target_GetSizeAndAlignOf(sp, resolve, t, size,align) )
                    return nullptr;
                ents.push_back(StructRepr::Ent { idx++, size, align, t.clone() });
            }
        }
        else
        {
            BUG(sp, "Unexpected type in creating struct repr");
        }


        if( allow_sort )
        {
            // TODO: Sort by alignment then size (largest first)
            // - Requires codegen to use this information
        }

        StructRepr  rv;
        size_t  cur_ofs = 0;
        for(auto& e : ents)
        {
            // Increase offset to fit alignment
            if( !packed )
            {
                while( cur_ofs % e.align != 0 )
                {
                    rv.ents.push_back({ ~0u, 1, 1, ::HIR::TypeRef( ::HIR::CoreType::U8 ) });
                    cur_ofs ++;
                }
            }

            rv.ents.push_back(mv$(e));
            cur_ofs += e.size;
        }
        return box$(rv);
    }
}
const StructRepr* Target_GetStructRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
{
    // TODO: Thread safety
    // Map of generic paths to struct representations.
    static ::std::map<::HIR::TypeRef, ::std::unique_ptr<StructRepr>>  s_cache;

    auto it = s_cache.find(ty);
    if( it != s_cache.end() )
    {
        return it->second.get();
    }

    auto ires = s_cache.insert(::std::make_pair( ty.clone(), make_struct_repr(sp, resolve, ty) ));
    return ires.first->second.get();
}

// TODO: Include NonZero and other repr optimisations here

bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align)
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
        TU_MATCHA( (te.binding), (be),
        (Unbound,
            BUG(sp, "Unbound type path " << ty << " encountered");
            ),
        (Opaque,
            return false;
            ),
        (Struct,
            if( const auto* repr = Target_GetStructRepr(sp, resolve, ty) )
            {
                out_size = 0;
                out_align = 1;
                for(const auto& e : repr->ents)
                {
                    out_size += e.size;
                    out_align = ::std::max(out_align, e.align);
                }
                return true;
            }
            else
            {
                return false;
            }
            ),
        (Enum,
            // Search for alternate repr
            // If not found, determine the variant size
            // Get data size and alignment.
            return false;
            ),
        (Union,
            // Max alignment and max data size
            return false;
            )
        )
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
        size_t  size;
        if( !Target_GetSizeAndAlignOf(sp, resolve, *te.inner, size,out_align) )
            return false;
        size *= te.size_val;
        ),
    (Slice,
        BUG(sp, "sizeof on a slice - unsized");
        ),
    (Tuple,
        // Same code as structs :)
        if( const auto* repr = Target_GetStructRepr(sp, resolve, ty) )
        {
            out_size = 0;
            out_align = 1;
            for(const auto& e : repr->ents)
            {
                out_size += e.size;
                out_align = ::std::max(out_align, e.align);
            }
            return true;
        }
        else
        {
            return false;
        }
        ),
    (Borrow,
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        if( resolve.type_is_sized(sp, *te.inner) )
        {
            out_size = g_target.m_arch.m_pointer_bits / 8;
            return true;
        }
        // TODO: Handle different types of Unsized
        ),
    (Pointer,
        // - Alignment is machine native
        out_align = g_target.m_arch.m_pointer_bits / 8;
        // - Size depends on Sized-nes of the parameter
        if( resolve.type_is_sized(sp, *te.inner) )
        {
            out_size = g_target.m_arch.m_pointer_bits / 8;
            return true;
        }
        // TODO: Handle different types of Unsized
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
bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size)
{
    size_t  ignore_align;
    return Target_GetSizeAndAlignOf(sp, resolve, ty, out_size, ignore_align);
}
bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align)
{
    size_t  ignore_size;
    return Target_GetSizeAndAlignOf(sp, resolve, ty, ignore_size, out_align);
}
