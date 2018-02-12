//
//
//
#include "codegen.hpp"
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include "mangling.hpp"
#include "target.hpp"

#include <iomanip>
#include <fstream>

namespace
{
    template<typename T>
    struct Fmt
    {
        const T&    e;
        Fmt(const T& e):
            e(e)
        {
        }
    };
    template<typename T> Fmt<T> fmt(const T& v) { return Fmt<T>(v); }

    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::MIR::LValue>& x)
    {
        auto fmt_lhs = [](::std::ostream& os, const ::MIR::LValue& lv) {
            if( lv.is_Deref() ) {
                os << "(" << fmt(lv) << ")";
            }
            else {
                os << fmt(lv);
            }
            };
        switch(x.e.tag())
        {
        case ::MIR::LValue::TAGDEAD:    throw "";
        TU_ARM(x.e, Return, _e) (void)_e;
            os << "RETURN";
            break;
        TU_ARM(x.e, Local, e)
            os << "var" << e;
            break;
        TU_ARM(x.e, Argument, e)
            os << "arg" << e.idx;
            break;
        TU_ARM(x.e, Static, e)
            os << e;
            break;
        TU_ARM(x.e, Deref, e)
            os << "*" << fmt(*e.val);
            break;
        TU_ARM(x.e, Field, e) {
            fmt_lhs(os, *e.val);
            // Avoid `0.` existing in the output
            if( e.val->is_Field() || e.val->is_Downcast() )
                os << " ";
            os << "." << e.field_index;
            } break;
        TU_ARM(x.e, Index, e) {
            fmt_lhs(os, *e.val);
            os << "[" << fmt(*e.idx) << "]";
            } break;
        TU_ARM(x.e, Downcast, e) {
            fmt_lhs(os, *e.val);
            os << "@" << e.variant_index;
            } break;
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::MIR::Constant>& x)
    {
        struct H {
            static uint64_t double_to_u64(double v) {
                uint64_t    rv;
                ::std::memcpy(&rv, &v, sizeof(double));
                return rv;
            }
        };
        const auto& e = x.e;
        switch(e.tag())
        {
        case ::MIR::Constant::TAGDEAD:  throw "";
        TU_ARM(e, Int, v) {
            os << (v.v < 0 ? "" : "+") << v.v << " " << v.t;
            } break;
        TU_ARM(e, Uint, v)
            os << v.v << " " << v.t;
            break;
        TU_ARM(e, Float, v) {
            // TODO: Infinity/nan/...
            auto vi = H::double_to_u64(v.v);
            bool sign = (vi & (1ull << 63)) != 0;
            int exp = (vi >> 52) & 0x7FF;
            uint64_t frac = vi & ((1ull << 52) - 1);
            os << (sign ? "-" : "+") << "0x1." << ::std::setw(52/4) << ::std::setfill('0') << ::std::hex << frac << ::std::dec << "p" << (exp - 1023);
            os << " " << v.t;
            } break;
        TU_ARM(e, ItemAddr, v) {
            os << "ADDROF " << v;
            } break;
        default:
            os << e;
            break;
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::MIR::Param>& x)
    {
        switch(x.e.tag())
        {
        case ::MIR::Param::TAGDEAD: throw "";
        TU_ARM(x.e, LValue, e)
            os << fmt(e);
            break;
        TU_ARM(x.e, Constant, e)
            os << fmt(e);
            break;
        }
        return os;
    }

    class CodeGenerator_MonoMir:
        public CodeGenerator
    {
        enum class MetadataType {
            None,
            Slice,
            TraitObject,
        };

        static Span sp;

        const ::HIR::Crate& m_crate;
        ::StaticTraitResolve    m_resolve;


        ::std::string   m_outfile_path;
        ::std::ofstream m_of;
        const ::MIR::TypeResolve* m_mir_res;

    public:
        CodeGenerator_MonoMir(const ::HIR::Crate& crate, const ::std::string& outfile):
            m_crate(crate),
            m_resolve(crate),
            m_outfile_path(outfile + ".mir"),
            m_of(m_outfile_path)
        {
            for( const auto& crate : m_crate.m_ext_crates )
            {
                m_of << "crate \"" << FmtEscaped(crate.second.m_path) << ".o.mir\";\n";
            }
        }

        void finalise(bool is_executable, const TransOptions& opt) override
        {
            if( is_executable )
            {
                m_of << "fn ::main#(i32, *const *const i8): i32 {\n";
                auto c_start_path = m_resolve.m_crate.get_lang_item_path_opt("mrustc-start");
                if( c_start_path == ::HIR::SimplePath() )
                {
                    m_of << "\tlet m: fn();\n";
                    m_of << "\t0: {\n";
                    m_of << "\t\tASSIGN m = ADDROF " << ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "mrustc-main")) << ";\n";
                    m_of << "\t\tCALL RETURN = " << ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "start")) << "(m, arg0, arg1) goto 1 else 1\n";
                }
                else
                {
                    m_of << "\t0: {\n";
                    m_of << "\t\tCALL RETURN = " << ::HIR::GenericPath(c_start_path) << "(arg0, arg1) goto 1 else 1;\n";
                }
                m_of << "\t}\n";
                m_of << "\t1: {\n";
                m_of << "\t\tRETURN\n";
                m_of << "\t}\n";
                m_of << "}\n";
            }

            m_of.flush();
            m_of.close();
        }

        /*
        void emit_box_drop_glue(::HIR::GenericPath p, const ::HIR::Struct& item)
        {
            auto struct_ty = ::HIR::TypeRef( p.clone(), &item );
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            // - Drop Glue
            const auto* ity = m_resolve.is_type_owned_box(struct_ty);

            auto inner_ptr = ::HIR::TypeRef::new_pointer( ::HIR::BorrowType::Unique, ity->clone() );
            auto box_free = ::HIR::GenericPath { m_crate.get_lang_item_path(sp, "box_free"), { ity->clone() } };

            ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> > args;
            args.push_back( ::std::make_pair( ::HIR::Pattern {}, mv$(inner_ptr) ) );

            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, empty_fcn };
            m_mir_res = &mir_res;
            m_of << "fn " << Trans_Mangle(drop_glue_path) << "(" << Trans_Mangle(p) << ") {\n";

            // Obtain inner pointer
            // TODO: This is very specific to the structure of the official liballoc's Box.
            m_of << "\tlet arg1: "; emit_type(args[0].second); m_of << "\n";
            // Call destructor of inner data
            emit_destructor_call( ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Argument({0})) }), *ity, true, 1);
            // Emit a call to box_free for the type
            m_of << "\t" << Trans_Mangle(box_free) << "(arg0);\n";

            m_of << "}\n";
            m_mir_res = nullptr;
        }
        */


        void emit_type(const ::HIR::TypeRef& ty) override
        {
            TRACE_FUNCTION_F(ty);
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "type " << ty;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            if( const auto* te = ty.m_data.opt_Tuple() )
            {
                if( te->size() > 0 )
                {
                    const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                    MIR_ASSERT(*m_mir_res, repr, "No repr for tuple " << ty);

                    m_of << "type " << ty << " {\n";
                    m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
                    for(const auto& e : repr->fields)
                    {
                        m_of << "\t" << e.offset << " = " << e.ty << ";\n";
                    }
                    m_of << "}\n";
                }
#if 0
                auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");
                auto args = ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> >();
                auto ty_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Owned, ty.clone());
                ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), ty_ptr, args, empty_fcn };
                m_mir_res = &mir_res;
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(ty); m_of << "* rv) {";
                auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
                auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
                for(const auto& ity : te)
                {
                    emit_destructor_call(fld_lv, ity, /*unsized_valid=*/false, 1);
                    fld_lv.as_Field().field_index ++;
                }
                m_of << "}\n";
#endif
            }
            else {
            }

            m_mir_res = nullptr;
        }

        void emit_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "struct " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            bool is_vtable; {
                const auto& lc = p.m_path.m_components.back();
                is_vtable = (lc.size() > 7 && ::std::strcmp(lc.c_str() + lc.size() - 7, "#vtable") == 0);
            };

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            // HACK: For vtables, insert the alignment and size at the start
            // TODO: Add this to the vtable in the HIR instead
            if(is_vtable)
            {
                //m_of << "\tVTABLE_HDR hdr;\n";
            }

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for struct " << ty);
            m_of << "type " << p << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << e.ty << ";\n";
            }
            m_of << "}\n";

            // TODO: Drop glue!
#if 0
            auto struct_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            // - Drop Glue

            ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> > args;
            if( item.m_markings.has_drop_impl ) {
                // If the type is defined outside the current crate, define as static (to avoid conflicts when we define it)
                if( p.m_path.m_crate_name != m_crate.m_crate_name )
                {
                    if( item.m_params.m_types.size() > 0 ) {
                        m_of << "static ";
                    }
                    else {
                        m_of << "extern ";
                    }
                }
                m_of << "tUNIT " << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
            }
            else if( m_resolve.is_type_owned_box(struct_ty) )
            {
                m_box_glue_todo.push_back( ::std::make_pair( mv$(struct_ty.m_data.as_Path().path.m_data.as_Generic()), &item ) );
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
                return ;
            }

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, empty_fcn };
            m_mir_res = &mir_res;
            m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ") {\n";

            // If this type has an impl of Drop, call that impl
            if( item.m_markings.has_drop_impl ) {
                m_of << "\t" << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "(rv);\n";
            }

            auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
            auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
            TU_MATCHA( (item.m_data), (e),
            (Unit,
                ),
            (Tuple,
                for(unsigned int i = 0; i < e.size(); i ++)
                {
                    const auto& fld = e[i];
                    fld_lv.as_Field().field_index = i;

                    emit_destructor_call(fld_lv, monomorph(fld.ent), true, 1);
                }
                ),
            (Named,
                for(unsigned int i = 0; i < e.size(); i ++)
                {
                    const auto& fld = e[i].second;
                    fld_lv.as_Field().field_index = i;

                    emit_destructor_call(fld_lv, monomorph(fld.ent), true, 1);
                }
                )
            )
            m_of << "}\n";
#endif
            m_mir_res = nullptr;
        }
        void emit_union(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Union& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "union " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for union " << ty);
            m_of << "type " << p << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << e.ty << ";\n";
            }
            m_of << "}\n";

            // TODO: Drop glue!
#if 0
            // Drop glue (calls destructor if there is one)
            auto item_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(item_ty.clone(), "#drop_glue");
            auto item_ptr_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, item_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(item_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), item_ptr_ty, {}, empty_fcn };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "tUNIT " << Trans_Mangle(drop_impl_path) << "(union u_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(union u_" << Trans_Mangle(p) << "* rv) {\n";
            if( item.m_markings.has_drop_impl )
            {
                m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
            }
            m_of << "}\n";
#endif
        }

        void emit_enum(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Enum& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;


            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for enum " << ty);
            m_of << "type " << p << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << e.ty << ";\n";
            }
            switch(repr->variants.tag())
            {
            case TypeRepr::VariantMode::TAGDEAD:    throw "";
            TU_ARM(repr->variants, None, _e) (void)_e;
                break;
            TU_ARM(repr->variants, Values, e)
                for(auto v : e.values)
                {
                    m_of << "\t[" << e.field.index << ", " << e.field.sub_fields << "] = \"";
                    for(size_t i = 0; i < e.field.size; i ++)
                    {
                        int val = (v >> (i*8)) & 0xFF;
                        //if( val == 0 )
                        //    m_of << "\\0";
                        //else
                        if(val < 16)
                            m_of << ::std::hex << "\\x0" << val << ::std::dec;
                        else
                            m_of << ::std::hex << "\\x" << val << ::std::dec;
                    }
                    m_of << "\";\n";
                }
                break;
            TU_ARM(repr->variants, NonZero, e) {
                m_of << "\t[" << e.field.index << ", " << e.field.sub_fields << "] = \"";
                for(size_t i = 0; i < e.field.size; i ++)
                {
                    m_of << "\\0";
                }
                m_of << "\";\n";
                } break;
            }
            m_of << "}\n";

#if 0
            // ---
            // - Drop Glue
            // ---
            auto struct_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, {}, empty_fcn };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "tUNIT " << Trans_Mangle(drop_impl_path) << "(struct e_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(struct e_" << Trans_Mangle(p) << "* rv) {\n";

            // If this type has an impl of Drop, call that impl
            if( item.m_markings.has_drop_impl )
            {
                m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
            }
            auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });

            if( nonzero_path.size() > 0 )
            {
                // TODO: Fat pointers?
                m_of << "\tif( (*rv)._1"; emit_nonzero_path(nonzero_path); m_of << " ) {\n";
                emit_destructor_call( ::MIR::LValue::make_Field({ box$(self), 1 }), monomorph(item.m_data.as_Data()[1].type), false, 2 );
                m_of << "\t}\n";
            }
            else if( const auto* e = item.m_data.opt_Data() )
            {
                auto var_lv =::MIR::LValue::make_Downcast({ box$(self), 0 });

                m_of << "\tswitch(rv->TAG) {\n";
                for(unsigned int var_idx = 0; var_idx < e->size(); var_idx ++)
                {
                    var_lv.as_Downcast().variant_index = var_idx;
                    m_of << "\tcase " << var_idx << ":\n";
                    emit_destructor_call(var_lv, monomorph( (*e)[var_idx].type ), false, 2);
                    m_of << "\tbreak;\n";
                }
                m_of << "\t}\n";
            }
            else
            {
                // Value enum
                // Glue does nothing (except call the destructor, if there is one)
            }
            m_of << "}\n";

            if( nonzero_path.size() )
            {
                m_enum_repr_cache.insert( ::std::make_pair( p.clone(), mv$(nonzero_path) ) );
            }
#endif
            m_mir_res = nullptr;
        }
        struct Reloc {
            size_t  ofs;
            size_t  len;
            const ::HIR::Path* p;
            ::std::string   bytes;
        };
        void emit_literal_as_bytes(const ::HIR::Literal& lit, const ::HIR::TypeRef& ty, ::std::vector<Reloc>& out_relocations, size_t base_ofs)
        {
            TRACE_FUNCTION_F(lit << ", " << ty);
            auto putb = [&](uint8_t b) {
                if( b == 0 ) {
                    m_of << "\\0";
                }
                else if( b == '\\' ) {
                    m_of << "\\\\";
                }
                else if( b == '"' ) {
                    m_of << "\\\"";
                }
                else if( ' ' <= b && b <= 'z' && b != '\\' ) {
                    m_of << b;
                }
                else if( b < 16 ) {
                    m_of << "\\x0" << ::std::hex << int(b) << ::std::dec;
                }
                else {
                    m_of << "\\x" << ::std::hex << int(b) << ::std::dec;
                }
                };
            auto putu32 = [&](uint32_t v) {
                putb(v & 0xFF);
                putb(v >> 8);
                putb(v >> 16);
                putb(v >> 24);
                };
            auto putsize = [&](uint64_t v) {
                if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 ) {
                    putu32(v      );
                    putu32(v >> 32);
                }
                else if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 ) {
                    putu32(v   );
                }
                else {
                    putu32(v   );
                }
                };
            switch(ty.m_data.tag())
            {
            case ::HIR::TypeRef::Data::TAGDEAD: throw "";
            case ::HIR::TypeRef::Data::TAG_Generic:
            case ::HIR::TypeRef::Data::TAG_ErasedType:
            case ::HIR::TypeRef::Data::TAG_Diverge:
            case ::HIR::TypeRef::Data::TAG_Infer:
            case ::HIR::TypeRef::Data::TAG_TraitObject:
            case ::HIR::TypeRef::Data::TAG_Slice:
            case ::HIR::TypeRef::Data::TAG_Closure:
                BUG(sp, "Unexpected " << ty << " in decoding literal");
            TU_ARM(ty.m_data, Primitive, te) {
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
                    putb(lit.as_Integer() & 0xFF);
                    putb(lit.as_Integer() >> 8);
                    break;
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::Char:
                    ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                    putu32(lit.as_Integer()   );
                    break;
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I64:
                    ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                    putu32(lit.as_Integer()      );
                    putu32(lit.as_Integer() >> 32);
                    break;
                case ::HIR::CoreType::U128:
                case ::HIR::CoreType::I128:
                    ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                    putu32(lit.as_Integer()      );
                    putu32(lit.as_Integer() >> 32);
                    putu32(0);
                    putu32(0);
                    break;
                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Isize:
                    ASSERT_BUG(sp, lit.is_Integer(), ty << " not Literal::Integer - " << lit);
                    putsize(lit.as_Integer());
                    break;
                case ::HIR::CoreType::F32: {
                    ASSERT_BUG(sp, lit.is_Float(), "not Literal::Float - " << lit);
                    uint32_t v;
                    float v2 = lit.as_Float();
                    memcpy(&v, &v2, 4);
                    putu32(v);
                    } break;
                case ::HIR::CoreType::F64: {
                    ASSERT_BUG(sp, lit.is_Float(), "not Literal::Float - " << lit);
                    uint64_t v;
                    memcpy(&v, &lit.as_Float(), 8);
                    putu32(v);
                    } break;
                case ::HIR::CoreType::Str:
                    BUG(sp, "Unexpected " << ty << " in decoding literal");
                }
                } break;
            case ::HIR::TypeRef::Data::TAG_Path:
            case ::HIR::TypeRef::Data::TAG_Tuple: {
                const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                assert(repr);
                size_t cur_ofs = 0;
                if( lit.is_List() )
                {
                    const auto& le = lit.as_List();
                    assert(le.size() == repr->fields.size());
                    for(size_t i = 0; i < repr->fields.size(); i ++)
                    {
                        assert(cur_ofs <= repr->fields[i].offset);
                        while(cur_ofs < repr->fields[i].offset)
                        {
                            putb(0);
                            cur_ofs ++;
                        }
                        emit_literal_as_bytes(le[i], repr->fields[i].ty, out_relocations, base_ofs + cur_ofs);
                        size_t size;
                        assert(Target_GetSizeOf(sp, m_resolve, repr->fields[i].ty, size));
                        cur_ofs += size;
                    }
                    while(cur_ofs < repr->size)
                    {
                        putb(0);
                        cur_ofs ++;
                    }
                }
                else if( lit.is_Variant() )
                {
                    const auto& le = lit.as_Variant();
                    if( *le.val != ::HIR::Literal::make_List({}) )
                    {
                        assert(le.idx < repr->fields.size());
                        while(cur_ofs < repr->fields[le.idx].offset)
                        {
                            putb(0);
                            cur_ofs ++;
                        }

                        emit_literal_as_bytes(*le.val, repr->fields[le.idx].ty, out_relocations, base_ofs + cur_ofs);

                        size_t size;
                        assert(Target_GetSizeOf(sp, m_resolve, repr->fields[le.idx].ty, size));
                        cur_ofs += size;
                    }

                    if(const auto* ve = repr->variants.opt_Values())
                    {
                        ASSERT_BUG(sp, cur_ofs <= repr->fields[ve->field.index].offset, "Bad offset before enum tag");
                        while(cur_ofs < repr->fields[ve->field.index].offset)
                        {
                            putb(0);
                            cur_ofs ++;
                        }
                        auto v = ::HIR::Literal::make_Integer(le.idx);
                        emit_literal_as_bytes(v, repr->fields[ve->field.index].ty, out_relocations, base_ofs + cur_ofs);

                        size_t size;
                        assert(Target_GetSizeOf(sp, m_resolve, repr->fields[ve->field.index].ty, size));
                        cur_ofs += size;
                    }
                    // TODO: Nonzero?
                    while(cur_ofs < repr->size)
                    {
                        putb(0);
                        cur_ofs ++;
                    }
                }
                else
                {
                    TODO(sp, "Composites - " << ty << " w/ " << lit);
                }
                } break;
            case ::HIR::TypeRef::Data::TAG_Borrow:
                if( *ty.m_data.as_Borrow().inner == ::HIR::CoreType::Str )
                {
                    ASSERT_BUG(sp, lit.is_String(), ty << " not Literal::String - " << lit);
                    const auto& s = lit.as_String();
                    putsize(0);
                    putsize(s.size());
                    out_relocations.push_back(Reloc { base_ofs, 8, nullptr, s });
                    break;
                }
                // fall
            case ::HIR::TypeRef::Data::TAG_Pointer: {
                const auto& ity = (ty.m_data.is_Borrow() ? *ty.m_data.as_Borrow().inner : *ty.m_data.as_Pointer().inner);
                size_t ity_size, ity_align;
                Target_GetSizeAndAlignOf(sp, m_resolve, ity, ity_size, ity_align);
                bool is_unsized = (ity_size == SIZE_MAX);
                if( lit.is_BorrowPath() )
                {
                    putsize(0);
                    out_relocations.push_back(Reloc { base_ofs, 8, &lit.as_BorrowPath(), "" });
                    if( is_unsized )
                    {
                        // TODO: Get the size of the pointed-to array
                        // OR: Find out the source item type and the target trait.
                        putsize(0);
                    }
                    break;
                }
                else if( lit.is_Integer() )
                {
                    ASSERT_BUG(sp, lit.as_Integer() == 0, "Pointer from integer not 0");
                    ASSERT_BUG(sp, ty.m_data.is_Pointer(), "Borrow from integer");
                    putsize(0);
                    if( is_unsized )
                    {
                        putsize(0);
                    }
                    break;
                }
                TODO(sp, "Pointers - " << ty << " w/ " << lit);
                } break;
            case ::HIR::TypeRef::Data::TAG_Function:
                ASSERT_BUG(sp, lit.is_BorrowPath(), ty << " not Literal::BorrowPath - " << lit);
                putsize(0);
                out_relocations.push_back(Reloc { base_ofs, 8, &lit.as_BorrowPath(), "" });
                break;
            TU_ARM(ty.m_data, Array, te) {
                // What about byte strings?
                // TODO: Assert size
                ASSERT_BUG(sp, lit.is_List(), "not Literal::List - " << lit);
                for(const auto& v : lit.as_List())
                {
                    emit_literal_as_bytes(v, *te.inner, out_relocations, base_ofs);
                    size_t size;
                    assert(Target_GetSizeOf(sp, m_resolve, *te.inner, size));
                    base_ofs += size;
                }
                } break;
            }
        }
        void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);

            ::std::vector<Reloc>    relocations;

            auto type = params.monomorph(m_resolve, item.m_type);
            m_of << "static " << p << ": " << type << " = \"";
            emit_literal_as_bytes(item.m_value_res, type, relocations, 0);
            m_of << "\"";
            m_of << "{";
            for(const auto& r : relocations)
            {
                m_of << "@" << r.ofs << "+" << r.len << " = ";
                if( r.p )
                    m_of << *r.p;
                else
                    m_of << "\"" << FmtEscaped(r.bytes) << "\"";
                m_of << ",";
            }
            m_of << "}";
            m_of << ";\n";

            m_mir_res = nullptr;
        }
        void emit_function_code(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def, const ::MIR::FunctionPointer& code) override
        {
            TRACE_FUNCTION_F(p);

            ::MIR::TypeResolve::args_t  arg_types;
            for(const auto& ent : item.m_args)
                arg_types.push_back(::std::make_pair( ::HIR::Pattern{}, params.monomorph(m_resolve, ent.second) ));

            ::HIR::TypeRef  ret_type_tmp;
            const auto& ret_type = monomorphise_fcn_return(ret_type_tmp, item, params);

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << p;), ret_type, arg_types, *code };
            m_mir_res = &mir_res;

            m_of << "fn " << p << "(";
            m_of << "): " << ret_type << " {\n";
            for(unsigned int i = 0; i < code->locals.size(); i ++) {
                DEBUG("var" << i << " : " << code->locals[i]);
                m_of << "\tlet var" << i << ": " << code->locals[i] << ";\n";
            }
            for(unsigned int i = 0; i < code->drop_flags.size(); i ++) {
                m_of << "\tlet df" << i << " = " << code->drop_flags[i] << ";\n";
            }

            
            for(unsigned int i = 0; i < code->blocks.size(); i ++)
            {
                TRACE_FUNCTION_F(p << " bb" << i);

                m_of << "\t" << i << ": {\n";

                for(const auto& stmt : code->blocks[i].statements)
                {
                    m_of << "\t\t";
                    mir_res.set_cur_stmt(i, (&stmt - &code->blocks[i].statements.front()));
                    DEBUG(stmt);
                    switch(stmt.tag())
                    {
                    case ::MIR::Statement::TAGDEAD: throw "";
                    TU_ARM(stmt, Assign, se) {
                        m_of << "ASSIGN " << fmt(se.dst) << " = ";
                        switch(se.src.tag())
                        {
                        case ::MIR::RValue::TAGDEAD:    throw "";
                        TU_ARM(se.src, Use, e)
                            m_of << "=" << fmt(e);
                            break;
                        TU_ARM(se.src, Constant, e)
                            m_of << fmt(e);
                            break;
                        TU_ARM(se.src, SizedArray, e)
                            m_of << "[" << fmt(e.val) << "; " << e.count << "]";
                            break;
                        TU_ARM(se.src, Borrow, e) {
                            m_of << "&";
                            switch(e.type)
                            {
                            case ::HIR::BorrowType::Shared: break;
                            case ::HIR::BorrowType::Unique: m_of << "mut "; break;
                            case ::HIR::BorrowType::Owned:  m_of << "move "; break;
                            }
                            m_of << fmt(e.val);
                            } break;
                        TU_ARM(se.src, Cast, e)
                            m_of << "CAST " << fmt(e.val) << " as " << e.type;
                            break;
                        TU_ARM(se.src, BinOp, e) {
                            m_of << "BINOP " << fmt(e.val_l) << " ";
                            switch(e.op)
                            {
                            case ::MIR::eBinOp::ADD:    m_of << "+";    break;
                            case ::MIR::eBinOp::ADD_OV: m_of << "+^";   break;
                            case ::MIR::eBinOp::SUB:    m_of << "-";    break;
                            case ::MIR::eBinOp::SUB_OV: m_of << "-^";   break;
                            case ::MIR::eBinOp::MUL:    m_of << "*";    break;
                            case ::MIR::eBinOp::MUL_OV: m_of << "*^";   break;
                            case ::MIR::eBinOp::DIV:    m_of << "*";    break;
                            case ::MIR::eBinOp::DIV_OV: m_of << "*^";   break;
                            case ::MIR::eBinOp::MOD:    m_of << "%";    break;
                            case ::MIR::eBinOp::BIT_OR: m_of << "|";    break;
                            case ::MIR::eBinOp::BIT_AND:m_of << "&";    break;
                            case ::MIR::eBinOp::BIT_XOR:m_of << "^";    break;
                            case ::MIR::eBinOp::BIT_SHR:m_of << ">>";   break;
                            case ::MIR::eBinOp::BIT_SHL:m_of << "<<";   break;
                            case ::MIR::eBinOp::NE:     m_of << "!=";   break;
                            case ::MIR::eBinOp::EQ:     m_of << "==";   break;
                            case ::MIR::eBinOp::GT:     m_of << ">" ;   break;
                            case ::MIR::eBinOp::GE:     m_of << ">=";   break;
                            case ::MIR::eBinOp::LT:     m_of << "<" ;   break;
                            case ::MIR::eBinOp::LE:     m_of << "<=";   break;
                            }
                            m_of << " " << fmt(e.val_r);
                            } break;
                        TU_ARM(se.src, UniOp, e) {
                            m_of << "UNIOP ";
                            switch(e.op)
                            {
                            case ::MIR::eUniOp::INV:    m_of << "!";    break;
                            case ::MIR::eUniOp::NEG:    m_of << "-";    break;
                            }
                            m_of << " " << fmt(e.val);
                            } break;
                        TU_ARM(se.src, DstMeta, e)
                            m_of << "DSTMETA " << fmt(e.val);
                            break;
                        TU_ARM(se.src, DstPtr, e)
                            m_of << "DSTPTR " << fmt(e.val);
                            break;
                        TU_ARM(se.src, MakeDst, e)
                            m_of << "MAKEDST " << fmt(e.ptr_val) << ", " << fmt(e.ptr_val);
                            break;
                        TU_ARM(se.src, Variant, e)
                            m_of << "VARIANT " << e.path << " " << e.index << " " << fmt(e.val);
                            break;
                        TU_ARM(se.src, Array, e) {
                            m_of << "[ ";
                            for(const auto& v : e.vals)
                            {
                                m_of << fmt(v) << ", ";
                            }
                            m_of << "]";
                            } break;
                        TU_ARM(se.src, Tuple, e) {
                            m_of << "( ";
                            for(const auto& v : e.vals)
                            {
                                m_of << fmt(v) << ", ";
                            }
                            m_of << ")";
                            } break;
                        TU_ARM(se.src, Struct, e) {
                            m_of << "{ ";
                            for(const auto& v : e.vals)
                            {
                                m_of << fmt(v) << ", ";
                            }
                            m_of << "}: " << e.path;
                            } break;
                        }
                        } break;
                    TU_ARM(stmt, SetDropFlag, se) {
                        m_of << "SETFLAG df" << se.idx << " = ";
                        if( se.other == ~0u )
                        {
                            m_of << se.new_val;
                        }
                        else
                        {
                            m_of << (se.new_val ? "" : "!") << "df" << se.other;
                        }
                        } break;
                    TU_ARM(stmt, Asm, se) {
                        m_of << "ASM (";
                        for(const auto& v : se.outputs)
                        {
                            m_of << "\"" << v.first << "\" : " << fmt(v.second) << ", ";
                        }
                        m_of << ") = \"" << se.tpl << "\"(";
                        for(const auto& v : se.inputs)
                        {
                            m_of << "\"" << v.first << "\" : " << fmt(v.second) << ", ";
                        }
                        m_of << ") [";

                        for(const auto& v : se.clobbers)
                        {
                            m_of << "\"" << v << "\", ";
                        }
                        m_of << ":" << se.flags << "]";
                        } break;
                    TU_ARM(stmt, ScopeEnd, se) { (void)se;
                        continue ;
                        } break;
                    TU_ARM(stmt, Drop, se) {
                        m_of << "DROP " << fmt(se.slot);
                        switch(se.kind)
                        {
                        case ::MIR::eDropKind::DEEP:
                            break;
                        case ::MIR::eDropKind::SHALLOW:
                            m_of << " SHALLOW";
                            break;
                        }
                        if(se.flag_idx != ~0u)
                        {
                            m_of << " IF df" << se.flag_idx;
                        }
                        } break;
                    }
                    m_of << ";\n";
                }

                mir_res.set_cur_stmt_term(i);
                const auto& term = code->blocks[i].terminator;
                DEBUG("- " << term);
                m_of << "\t\t";
                switch(term.tag())
                {
                case ::MIR::Terminator::TAGDEAD: throw "";
                TU_ARM(term, Incomplete, _e) (void)_e;
                    m_of << "INCOMPLTE\n";
                    break;
                TU_ARM(term, Return, _e) (void)_e;
                    m_of << "RETURN\n";
                    break;
                TU_ARM(term, Diverge, _e) (void)_e;
                    m_of << "DIVERGE\n";
                    break;
                TU_ARM(term, Goto, e)
                    m_of << "GOTO " << e << "\n";
                    break;
                TU_ARM(term, Panic, e)
                    m_of << "PANIC " << e.dst << "\n";
                    break;
                TU_ARM(term, If, e)
                    m_of << "IF " << fmt(e.cond) << " goto " << e.bb0 << " else " << e.bb1 << "\n";
                    break;
                TU_ARM(term, Switch, e) {
                    m_of << "SWITCH " << fmt(e.val) << " { ";
                    m_of << e.targets;
                    m_of << " }\n";
                    } break;
                TU_ARM(term, SwitchValue, e) {
                    m_of << "SWITCHVALUE " << fmt(e.val) << " { ";
                    switch(e.values.tag())
                    {
                    case ::MIR::SwitchValues::TAGDEAD:  throw "";
                    TU_ARM(e.values, String, ve)
                        for(size_t i = 0; i < ve.size(); i++)
                        {
                            m_of << "\"" << FmtEscaped(ve[i]) << "\" = " << e.targets[i] << ",";
                        }
                        break;
                    TU_ARM(e.values, Unsigned, ve)
                        for(size_t i = 0; i < ve.size(); i++)
                        {
                            m_of << ve[i] << " = " << e.targets[i] << ",";
                        }
                        break;
                    TU_ARM(e.values, Signed, ve)
                        for(size_t i = 0; i < ve.size(); i++)
                        {
                            m_of << (ve[i] < 0 ? "" : "+") << ve[i] << " = " << e.targets[i] << ",";
                        }
                        break;
                    }
                    // TODO: Values.
                    //if( e.values.size() > 0 )
                    //{
                    //    m_of << ", ";
                    //}
                    m_of << "_ = " << e.def_target;
                    m_of << " }\n";
                    } break;
                TU_ARM(term, Call, e) {
                    m_of << "CALL " << fmt(e.ret_val) << " = ";
                    switch(e.fcn.tag())
                    {
                    case ::MIR::CallTarget::TAGDEAD: throw "";
                    TU_ARM(e.fcn, Intrinsic, f) m_of << "\"" << f.name << "\"" << f.params;  break;
                    TU_ARM(e.fcn, Value, f)     m_of << "(" << fmt(f) << ")";  break;
                    TU_ARM(e.fcn, Path, f)      m_of << f;  break;
                    }
                    m_of << "(";
                    for(const auto& a : e.args)
                    {
                        m_of << fmt(a) << ", ";
                    }
                    m_of << ") goto " << e.ret_block << " else " << e.panic_block << "\n";
                    } break;
                }
                m_of << "\t}\n";
            }

            m_of << "}\n";


            m_mir_res = nullptr;
        }


    private:
        const ::HIR::TypeRef& monomorphise_fcn_return(::HIR::TypeRef& tmp, const ::HIR::Function& item, const Trans_Params& params)
        {
            if( visit_ty_with(item.m_return, [&](const auto& x){ return x.m_data.is_ErasedType() || x.m_data.is_Generic(); }) )
            {
                tmp = clone_ty_with(Span(), item.m_return, [&](const auto& tpl, auto& out) {
                    TU_IFLET( ::HIR::TypeRef::Data, tpl.m_data, ErasedType, e,
                        out = params.monomorph(m_resolve, item.m_code.m_erased_types.at(e.m_index));
                        return true;
                    )
                    else if( tpl.m_data.is_Generic() ) {
                        out = params.get_cb()(tpl).clone();
                        return true;
                    }
                    else {
                        return false;
                    }
                });
                m_resolve.expand_associated_types(Span(), tmp);
                return tmp;
            }
            else
            {
                return item.m_return;
            }
        }
    };
    Span CodeGenerator_MonoMir::sp;
}

::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGenerator_MonoMir(const ::HIR::Crate& crate, const ::std::string& outfile)
{
    return ::std::unique_ptr<CodeGenerator>(new CodeGenerator_MonoMir(crate, outfile));
}
