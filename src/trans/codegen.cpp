/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen.cpp
 * - Wrapper for translation
 */
#include "main_bindings.hpp"
#include "trans_list.hpp"
#include <hir/hir.hpp>
#include <mir/mir.hpp>
#include <mir/operations.hpp>
#include <algorithm>
#include "target.hpp"

#include "codegen.hpp"
#include "monomorphise.hpp"

void Trans_Codegen(const ::std::string& outfile, CodegenOutput out_ty, const TransOptions& opt, const ::HIR::Crate& crate, const TransList& list, const ::std::string& hir_file)
{
    static Span sp;

    ::std::unique_ptr<CodeGenerator>    codegen;
    if( opt.mode == "monomir" )
    {
        codegen = Trans_Codegen_GetGenerator_MonoMir(crate, outfile);
    }
    else if( opt.mode == "c" )
    {
        codegen = Trans_Codegen_GetGeneratorC(crate, outfile);
    }
    else
    {
        BUG(sp, "Unknown codegen mode '" << opt.mode << "'");
    }

    // 1. Emit structure/type definitions.
    // - Emit in the order they're needed.
    for(const auto& ty : list.m_types)
    {
        if( ty.second )
        {
            codegen->emit_type_proto(ty.first);
        }
        else
        {
            if( const auto* te = ty.first.data().opt_Path() )
            {
                TU_MATCHA( (te->binding), (tpb),
                (Unbound,  throw ""; ),
                (Opaque,  throw ""; ),
                (ExternType,
                    //codegen->emit_extern_type(sp, te->path.m_data.as_Generic(), *tpb);
                    ),
                (Struct,
                    codegen->emit_struct(sp, te->path.m_data.as_Generic(), *tpb);
                    ),
                (Union,
                    codegen->emit_union(sp, te->path.m_data.as_Generic(), *tpb);
                    ),
                (Enum,
                    codegen->emit_enum(sp, te->path.m_data.as_Generic(), *tpb);
                    )
                )
            }
            codegen->emit_type(ty.first);
        }
    }
    for(const auto& ty : list.m_typeids)
    {
        codegen->emit_type_id(ty);
    }
    // Emit required constructor methods (and other wrappers)
    for(const auto& path : list.m_constructors)
    {
        // Get the item type
        // - Function (must be an intrinsic)
        // - Struct (must be a tuple struct)
        // - Enum variant (must be a tuple variant)
        const ::HIR::Module* mod_ptr = nullptr;
        if(path.m_path.m_components.size() > 1)
        {
            const auto& nse = crate.get_typeitem_by_path(sp, path.m_path, false, true);
            if(const auto* e = nse.opt_Enum())
            {
                auto var_idx = e->find_variant(path.m_path.m_components.back());
                codegen->emit_constructor_enum(sp, path, *e, var_idx);
                continue ;
            }
            mod_ptr = &nse.as_Module();
        }
        else
        {
            mod_ptr = &crate.get_mod_by_path(sp, path.m_path, true);
        }

        // Not an enum, currently must be a struct
        const auto& te = mod_ptr->m_mod_items.at(path.m_path.m_components.back())->ent;
        codegen->emit_constructor_struct(sp, path, te.as_Struct());
    }

    // 2. Emit function prototypes
    for(const auto& ent : list.m_functions)
    {
        DEBUG("FUNCTION " << ent.first);
        assert( ent.second->ptr );
        const auto& fcn = *ent.second->ptr;
        // Extern if there isn't any HIR
        bool is_extern = ! static_cast<bool>(fcn.m_code);
        if( fcn.m_code.m_mir && !ent.second->force_prototype ) {
            codegen->emit_function_proto(ent.first, fcn, ent.second->pp, is_extern);
        }
    }
    // - External functions
    for(const auto& ent : list.m_functions)
    {
        //DEBUG("FUNCTION " << ent.first);
        assert( ent.second->ptr );
        const auto& fcn = *ent.second->ptr;
        if( fcn.m_code.m_mir && !ent.second->force_prototype ) {
        }
        else {
            // TODO: Why would an intrinsic be in the queue?
            // - If it's exported it does.
            if( fcn.m_abi == "rust-intrinsic" ) {
            }
            else {
                codegen->emit_function_ext(ent.first, fcn, ent.second->pp);
            }
        }
    }
    // VTables (may be needed by statics)
    assert(list.m_vtables.empty());
    // 3. Emit statics
    for(const auto& ent : list.m_statics)
    {
        DEBUG("STATIC proto " << ent.first);
        assert(ent.second->ptr);
        const auto& stat = *ent.second->ptr;

        if( ! stat.m_value_res.is_Invalid() && !stat.m_no_emit_value )
        {
            codegen->emit_static_proto(ent.first, stat, ent.second->pp);
        }
        else
        {
            codegen->emit_static_ext(ent.first, stat, ent.second->pp);
        }
    }
    for(const auto& ent : list.m_statics)
    {
        DEBUG("STATIC " << ent.first);
        assert(ent.second->ptr);
        const auto& stat = *ent.second->ptr;

        if( ! stat.m_value_res.is_Invalid() && !stat.m_no_emit_value )
        {
            codegen->emit_static_local(ent.first, stat, ent.second->pp);
        }
    }


    // 4. Emit function code
    for(const auto& ent : list.m_functions)
    {
        if( ent.second->ptr && ent.second->ptr->m_code.m_mir && !ent.second->force_prototype )
        {
            const auto& path = ent.first;
            const auto& fcn = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            TRACE_FUNCTION_F(path);
            DEBUG("FUNCTION CODE " << path);
            // `is_extern` is set if there's no HIR (i.e. this function is from an external crate)
            bool is_extern = ! static_cast<bool>(fcn.m_code);
            // If this is a provided trait method, it needs to be monomorphised too.
            bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef("Self",0xFFFF);}) );
            if( pp.has_types() || is_method )
            {
                ASSERT_BUG(sp, ent.second->monomorphised.code, "Function that required monomorphisation wasn't monomorphised");

                // TODO: Flag that this should be a weak (or weak-er) symbol?
                // - If it's from an external crate, it should be weak, but what about local ones?
                codegen->emit_function_code(path, fcn, ent.second->pp, is_extern,  ent.second->monomorphised.code);
            }
            else {
                codegen->emit_function_code(path, fcn, pp, is_extern,  fcn.m_code.m_mir);
            }
        }
    }

    codegen->finalise(opt, out_ty, hir_file);
}

namespace {

    size_t Target_GetSizeOf_Required(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty)
    {
        size_t size;
        bool type_has_size = Target_GetSizeOf(sp, resolve, ty, size);
        ASSERT_BUG(sp, type_has_size, "Attempting to get the size of a unsized type");
        return size;
    }

    void encode_literal_as_bytes(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Literal& lit, const ::HIR::TypeRef& ty, EncodedLiteral& out, size_t base_ofs)
    {
        TRACE_FUNCTION_F("@" << ::std::hex << base_ofs << ::std::dec << " " << lit << ", " << ty);
        auto ptr_size = Target_GetCurSpec().m_arch.m_pointer_bits / 8;
        auto putb = [&](uint8_t b) {
            ASSERT_BUG(sp, base_ofs < out.bytes.size(), "encode_literal_as_bytes: Out of range write: " << base_ofs << " >= " << out.bytes.size());
            out.bytes[base_ofs++] = b;
            };
        auto put_val = [&](int bsize, uint64_t v) {
            if(false) {
                // Big endian
                for(int i = bsize; i--; )
                    putb( i < 8 ? (v >> (8*i)) & 0xFF : 0 );
            }
            else {
                // Little endian
                for(int i = 0; i < bsize; i ++)
                    putb( i < 8 ? (v >> (8*i)) & 0xFF : 0 );
            }
            };
        auto put_size = [&](uint64_t v) { put_val(ptr_size, v); };
        auto put_ptr_bytes = [&](std::string s) {
            out.relocations.push_back( Reloc::new_bytes(base_ofs, ptr_size,  s) );
            put_size(EncodedLiteral::PTR_BASE);
            };
        auto put_ptr_path = [&](const HIR::Path* p) {
            out.relocations.push_back( Reloc::new_named(base_ofs, ptr_size, p) );
            put_size(EncodedLiteral::PTR_BASE);
            };
        switch(ty.data().tag())
        {
        case ::HIR::TypeData::TAGDEAD: throw "";
        case ::HIR::TypeData::TAG_Generic:
        case ::HIR::TypeData::TAG_ErasedType:
        case ::HIR::TypeData::TAG_Diverge:
        case ::HIR::TypeData::TAG_Infer:
        case ::HIR::TypeData::TAG_TraitObject:
        case ::HIR::TypeData::TAG_Slice:
        case ::HIR::TypeData::TAG_Closure:
        case ::HIR::TypeData::TAG_Generator:
            BUG(sp, "Unexpected " << ty << " in decoding literal");
        TU_ARM(ty.data(), Primitive, te) {
            switch(te)
            {
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::Bool:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                putb(lit.as_Integer());
                break;
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::I16:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                put_val(2, lit.as_Integer());
                break;
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::Char:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                put_val(4, lit.as_Integer());
                break;
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::I64:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                put_val(8, lit.as_Integer());
                break;
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::I128:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                put_val(16, lit.as_Integer());
                break;
            case ::HIR::CoreType::Usize:
            case ::HIR::CoreType::Isize:
                ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                put_size(lit.as_Integer());
                break;
            case ::HIR::CoreType::F32: {
                ASSERT_BUG(sp, lit.is_Float(), "not Literal::Float - " << lit);
                uint32_t v;
                float v2 = lit.as_Float();
                memcpy(&v, &v2, 4);
                put_val(4, v);
                } break;
            case ::HIR::CoreType::F64: {
                ASSERT_BUG(sp, lit.is_Float(), "not Literal::Float - " << lit);
                uint64_t v;
                memcpy(&v, &lit.as_Float(), 8);
                put_val(8, v);
                } break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Unexpected " << ty << " in decoding literal");
            }
            } break;
        case ::HIR::TypeData::TAG_Path:
        case ::HIR::TypeData::TAG_Tuple: {
            const auto* repr = Target_GetTypeRepr(sp, resolve, ty);
            assert(repr);
            size_t cur_ofs = 0;
            if( lit.is_List() )
            {
                const auto& le = lit.as_List();
                assert(le.size() == repr->fields.size());
                for(size_t i = 0; i < repr->fields.size(); i ++)
                {
                    encode_literal_as_bytes(sp, resolve, le[i], repr->fields[i].ty, out, base_ofs + repr->fields[i].offset);
                }
            }
            else if( lit.is_Variant() )
            {
                const auto& le = lit.as_Variant();
                if( *le.val != ::HIR::Literal::make_List({}) )
                {
                    ASSERT_BUG(sp, le.idx < repr->fields.size(), "");
                    if( le.val->is_List() ) 
                    {
                        size_t inner_base_ofs = base_ofs + repr->fields[le.idx].offset;
                        TRACE_FUNCTION_F("Enum @" << ::std::hex << base_ofs << ::std::dec << " " << *le.val << ", " << repr->fields[le.idx].ty);
                        const auto* inner_repr = Target_GetTypeRepr(sp, resolve, repr->fields[le.idx].ty);
                        const auto& inner_le = le.val->as_List();

                        assert(inner_le.size() <= inner_repr->fields.size());
                        for(size_t i = 0; i < inner_le.size(); i ++)
                        {
                            encode_literal_as_bytes(sp, resolve, inner_le[i], inner_repr->fields[i].ty, out, inner_base_ofs + inner_repr->fields[i].offset);
                        }
                    }
                    else
                    {
                        encode_literal_as_bytes(sp, resolve, *le.val, repr->fields[le.idx].ty, out, base_ofs + repr->fields[le.idx].offset);
                    }
                }

                TU_MATCH_HDRA( (repr->variants), {)
                TU_ARMA(None, ve) {
                    }
                TU_ARMA(NonZero, ve) {
                    // No tag to write, just leave as zeroes
                    }
                TU_ARMA(Linear, ve) {
                    if( ve.is_niche(le.idx) ) {
                        // No tag to write (this is the niche)
                    }
                    else {
                        // Obtain the offset, write into it
                        struct H {
                            static size_t get_offset(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr* r, const TypeRepr::FieldPath& out_path)
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
                        };
                        auto ofs = H::get_offset(sp, resolve, repr, ve.field);
                        base_ofs += ofs;
                        //DEBUG("@
                        put_val(ve.field.size, ve.offset + le.idx);
                    }
                    }
                TU_ARMA(Values, ve) {
                    auto v = ::HIR::Literal::make_Integer( ve.values.at(le.idx) );
                    encode_literal_as_bytes(sp, resolve, v, repr->fields[ve.field.index].ty, out, base_ofs + repr->fields[ve.field.index].offset);
                    }
                }
            }
            else
            {
                TODO(sp, "Composites - " << ty << " w/ " << lit);
            }
            } break;
        case ::HIR::TypeData::TAG_Borrow:
            if( ty.data().as_Borrow().inner == ::HIR::CoreType::Str )
            {
                ASSERT_BUG(sp, lit.is_String(), ty << " not Literal::String - " << lit);
                const auto& s = lit.as_String();
                put_ptr_bytes(s);
                put_size(s.size());
                break;
            }
            // fall
        case ::HIR::TypeData::TAG_Pointer: {
            const auto& ity = (ty.data().is_Borrow() ? ty.data().as_Borrow().inner : ty.data().as_Pointer().inner);
            size_t ity_size, ity_align;
            Target_GetSizeAndAlignOf(sp, resolve, ity, ity_size, ity_align);
            bool is_unsized = (ity_size == SIZE_MAX);

            TU_MATCH_HDRA( (lit), { )
            TU_ARMA(BorrowPath, le) {
                MonomorphState  item_params;
                auto value = resolve.get_value(sp, le, item_params);
                put_ptr_path(&le);
                if( is_unsized )
                {
                    if( ity.data().is_Slice() )
                    {
                        ASSERT_BUG(sp, value.is_Static(), "Literal::BorrowPath returning &/*[T] not of a static - " << le << " is " << value.tag_str());
                        const auto& stat = *value.as_Static();
                        ASSERT_BUG(sp, stat.m_type.data().is_Array(), "Literal::BorrowPath : &[T] of non-array static, " << le << " - " << stat.m_type);
                        auto size = stat.m_type.data().as_Array().size.as_Known();
                        put_size( size );
                    }
                    else if( const auto* to = ity.data().opt_TraitObject() )
                    {
                        const auto& trait_path = to->m_trait.m_path;
                        ASSERT_BUG(sp, value.is_Static(), "BorrowOf returning &TraitObject not of a static - " << le << " is " << value.tag_str());
                        const auto& stat = *value.as_Static();

                        out.paths.push_back( ::std::make_unique<HIR::Path>(stat.m_type.clone(), trait_path.clone(), "vtable#") );
                        put_ptr_path( out.paths.back().get() );
                    }
                    else
                    {
                        BUG(sp, "Unexpected unsized type in Literal::BorrowPath - " << ity);
                    }
                }
                }
            TU_ARMA(Integer, le) {
                // NOTE: Windows uses magic numbers in the upper range of pointers, so this can be non-zero
                // - BUT, if it's unsized, then NUL is the only valid option
                //ASSERT_BUG(sp, !is_unsized || le == 0, "Cannot originate a non-NUL fat pointer from an integer");
                ASSERT_BUG(sp, ty.data().is_Pointer(), "Originating a Borrow from an integer is invalid");
                put_size(le);
                if( is_unsized )
                {
                    put_size(0);
                }
                }
            TU_ARMA(String, s) {
                // TODO: Check type
                put_ptr_bytes(s);
                if( is_unsized )
                {
                    put_size(s.size());
                }
                }
            break; default:
                TODO(sp, "Emit a pointer - " << ty << " from literal " << lit);
            }
            } break;
        case ::HIR::TypeData::TAG_Function:
            ASSERT_BUG(sp, lit.is_BorrowPath(), ty << " not Literal::BorrowPath - " << lit);
            put_ptr_path(&lit.as_BorrowPath());
            break;
        TU_ARM(ty.data(), Array, te) {
            TU_MATCH_HDRA( (lit), { )
            default:
                BUG(sp, "Invalid literal for Array - Literal::" << lit.tag_str() << " " << lit);
            TU_ARMA(List, le) {
                size_t item_size = Target_GetSizeOf_Required(sp, resolve, te.inner);
                // TODO: Assert that list size matches type's item count
                for(const auto& v : le)
                {
                    encode_literal_as_bytes(sp, resolve, v, te.inner, out, base_ofs);
                    base_ofs += item_size;
                }
                }
            TU_ARMA(String, le) {
                ASSERT_BUG(sp, le.size() == te.size.as_Known(), "String size doesn't match array size");
                for(auto c : le)
                    putb(c);
                }
            }
            } break;
        }
    }
}

EncodedLiteral Trans_EncodeLiteralAsBytes(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Literal& lit, const ::HIR::TypeRef& ty)
{
    EncodedLiteral  rv;
    rv.bytes.resize( Target_GetSizeOf_Required(sp, resolve, ty) );
    encode_literal_as_bytes(sp, resolve, lit, ty, rv, 0);
    std::sort(rv.relocations.begin(), rv.relocations.end(), [](const Reloc& a, const Reloc& b){ return a.ofs < b.ofs; });
    return rv;
}
