/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen_mmir.cpp
 * - Monomorphised MIR Backend
 *
 * Saves the MIR in a loadable form, used by the `standalone_miri` tool
 */
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

    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::HIR::Path>& x)
    {
        return os << Trans_Mangle(x.e);
    }
    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::HIR::GenericPath>& x)
    {
        return os << Trans_Mangle(x.e);
    }
    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::HIR::SimplePath>& x)
    {
        return os << Trans_Mangle(x.e);
    }
    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::HIR::TypeRef>& x)
    {
        TU_MATCH_HDRA( (x.e.data()), {)
        TU_ARMA(Infer, te)  BUG(Span(), "" << x.e);
        TU_ARMA(Diverge, te) {
            os << "!";
            }
        TU_ARMA(Primitive, te) {
            os << te;
            }
        TU_ARMA(Path, te) {
            os << Trans_Mangle(te.path);
            }
        TU_ARMA(Generic, te) {
            BUG(Span(), "" << x.e);
            }
        TU_ARMA(TraitObject, te) {
            auto path = te.m_trait.m_path.clone();
            os << "dyn " << Trans_Mangle(path);
            }
        TU_ARMA(ErasedType, te) {
            BUG(Span(), "" << x.e);
            }
        TU_ARMA(Array, te) {
            os << "[" << fmt(te.inner) << "; " << te.size << "]";
            }
        TU_ARMA(Slice, te) {
            os << "[" << fmt(te.inner) << "]";
            }
        TU_ARMA(Tuple, te) {
            if( te.empty() )
                os << "()";
            else
                os << Trans_Mangle(x.e);
            }
        TU_ARMA(Borrow, te) {
            switch(te.type)
            {
            case ::HIR::BorrowType::Shared: os << "&";  break;
            case ::HIR::BorrowType::Unique: os << "&mut ";  break;
            case ::HIR::BorrowType::Owned:  os << "&move "; break;
            }
            os << fmt(te.inner);
            }
        TU_ARMA(Pointer, te) {
            switch(te.type)
            {
            case ::HIR::BorrowType::Shared: os << "*const ";  break;
            case ::HIR::BorrowType::Unique: os << "*mut ";  break;
            case ::HIR::BorrowType::Owned:  os << "*move "; break;
            }
            os << fmt(te.inner);
            }
        TU_ARMA(Function, e) {
            if( e.is_unsafe ) {
                os << "unsafe ";
            }
            if( e.m_abi != "" ) {
                os << "extern \"" << e.m_abi << "\" ";
            }
            os << "fn(";
            for(const auto& t : e.m_arg_types)
                os << fmt(t) << ", ";
            os << ") -> " << fmt(e.m_rettype);
            } break;
        case ::HIR::TypeData::TAG_Closure:
        case ::HIR::TypeData::TAG_Generator:
            BUG(Span(), "Unexpected type in trans: " << x.e);
            break;
        }
        return os;
    }

    ::std::ostream& operator<<(::std::ostream& os, const Fmt<::MIR::LValue>& x)
    {
        for(const auto& w : ::reverse(x.e.m_wrappers))
        {
            if( w.is_Deref() ) {
                os << "(*";
            }
        }
        TU_MATCHA( (x.e.m_root), (e),
        (Return,
            os << "RETURN";
            ),
        (Local,
            os << "var" << e;
            ),
        (Argument,
            os << "arg" << e;
            ),
        (Static,
            os << fmt(e);
            )
        )
        bool was_num = false;
        for(const auto& w : x.e.m_wrappers)
        {
            bool prev_was_num = was_num; was_num = false;
            switch(w.tag())
            {
            case ::MIR::LValue::Wrapper::TAGDEAD:   throw "";
            TU_ARM(w, Deref, e)
                os << ")";
                break;
            TU_ARM(w, Field, field_index) {
                // Add a space to prevent accidental float literals
                if( prev_was_num )
                    os << " ";
                os << "." << field_index;
                was_num = true;
                } break;
            TU_ARM(w, Index, e) {
                os << "[" << fmt(::MIR::LValue::new_Local(e)) << "]";
                } break;
            TU_ARM(w, Downcast, variant_index) {
                os << "@" << variant_index;
                was_num = true;
                } break;
            }
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
            os << "ADDROF " << fmt(*v);
            } break;
        TU_ARM(e, Const, v) {
            BUG(Span(), "Stray named constant in MIR after cleanup - " << e);
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
        TU_ARM(x.e, Borrow, e) {
            os << "&";
            switch(e.type)
            {
            case ::HIR::BorrowType::Shared: break;
            case ::HIR::BorrowType::Unique: os << "mut "; break;
            case ::HIR::BorrowType::Owned:  os << "move "; break;
            }
            os << fmt(e.val);
            } break;
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
            m_outfile_path(outfile),
            m_of(m_outfile_path + ".mir")
        {
            for( const auto& crate_name : m_crate.m_ext_crates_ordered )
            {
                m_of << "crate \"" << FmtEscaped(m_crate.m_ext_crates.at(crate_name).m_path) << ".mir\";\n";
            }
        }

        void finalise(const TransOptions& opt, CodegenOutput out_ty, const ::std::string& hir_file) override
        {
            if( out_ty == CodegenOutput::Executable )
            {
                m_of << "fn main#(isize, *const *const i8): isize {\n";
                auto c_start_path = m_resolve.m_crate.get_lang_item_path_opt("mrustc-start");
                if( c_start_path == ::HIR::SimplePath() )
                {
                    m_of << "\tlet m: fn();\n";
                    m_of << "\t0: {\n";
                    m_of << "\t\tASSIGN m = ADDROF " << fmt(::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "mrustc-main"))) << ";\n";
                    m_of << "\t\tCALL RETURN = " << fmt(::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "start"))) << "(m, arg0, arg1) goto 1 else 1\n";
                }
                else
                {
                    m_of << "\t0: {\n";
                    m_of << "\t\tCALL RETURN = " << fmt(::HIR::GenericPath(c_start_path)) << "(arg0, arg1) goto 1 else 1;\n";
                }
                m_of << "\t}\n";
                m_of << "\t1: {\n";
                m_of << "\t\tRETURN\n";
                m_of << "\t}\n";
                m_of << "}\n";

                if(TARGETVER_LEAST_1_29)
                {
                    // Bind `panic_impl` lang item to the item tagged with `panic_implementation`
                    const auto& panic_impl_path = m_crate.get_lang_item_path(Span(), "mrustc-panic_implementation");
                    m_of << "fn panic_impl#(usize): u32 = \"panic_impl\":\"Rust\" {\n";
                    m_of << "\t0: {\n";
                    m_of << "\t\tCALL RETURN = " << fmt(panic_impl_path) << "(arg0) goto 1 else 2\n";
                    m_of << "\t}\n";
                    m_of << "\t1: { RETURN }\n";
                    m_of << "\t2: { DIVERGE }\n";
                    m_of << "}\n";

                    // TODO: OOM impl?
                }
            }

            m_of.flush();
            m_of.close();

            // HACK! Create the output file, but keep it empty
            {
                ::std::ofstream of( m_outfile_path );
                if( !of.good() )
                {
                    // TODO: Error?
                }
            }
        }


        void emit_type(const ::HIR::TypeRef& ty) override
        {
            TRACE_FUNCTION_F(ty);
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "type " << ty;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            if( const auto* te = ty.data().opt_Tuple() )
            {
                if( te->size() > 0 )
                {
                    const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                    MIR_ASSERT(*m_mir_res, repr, "No repr for tuple " << ty);

                    bool has_drop_glue =  m_resolve.type_needs_drop_glue(sp, ty);
                    auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");

                    m_of << "type " << fmt(ty) << " {\n";
                    m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
                    if( has_drop_glue )
                    {
                        m_of << "\tDROP " << fmt(drop_glue_path) << ";\n";
                    }
                    for(const auto& e : repr->fields)
                    {
                        m_of << "\t" << e.offset << " = " << fmt(e.ty) << ";\n";
                    }
                    m_of << "}\n";
                }
            }
            else {
            }

            m_mir_res = nullptr;
        }

        // TODO: Move this to a more common location
        MetadataType metadata_type(const ::HIR::TypeRef& ty) const
        {
            if( ty == ::HIR::CoreType::Str || ty.data().is_Slice() ) {
                return MetadataType::Slice;
            }
            else if( ty.data().is_TraitObject() ) {
                return MetadataType::TraitObject;
            }
            else if( ty.data().is_Path() )
            {
                const auto& te = ty.data().as_Path();
                switch( te.binding.tag() )
                {
                TU_ARM(te.binding, Struct, tpb) {
                    switch( tpb->m_struct_markings.dst_type )
                    {
                    case ::HIR::StructMarkings::DstType::None:
                        return MetadataType::None;
                    case ::HIR::StructMarkings::DstType::Possible: {
                        // TODO: How to figure out? Lazy way is to check the monomorpised type of the last field (structs only)
                        const auto& path = ty.data().as_Path().path.m_data.as_Generic();
                        const auto& str = *ty.data().as_Path().binding.as_Struct();
                        auto monomorph = [&](const auto& tpl) {
                            return m_resolve.monomorph_expand(sp, tpl, MonomorphStatePtr(nullptr, &path.m_params, nullptr));
                        };
                        TU_MATCHA( (str.m_data), (se),
                        (Unit,  MIR_BUG(*m_mir_res, "Unit-like struct with DstType::Possible"); ),
                        (Tuple, return metadata_type( monomorph(se.back().ent) ); ),
                        (Named, return metadata_type( monomorph(se.back().second.ent) ); )
                        )
                        //MIR_TODO(*m_mir_res, "Determine DST type when ::Possible - " << ty);
                        return MetadataType::None;
                    }
                    case ::HIR::StructMarkings::DstType::Slice:
                        return MetadataType::Slice;
                    case ::HIR::StructMarkings::DstType::TraitObject:
                        return MetadataType::TraitObject;
                    }
                    throw "";
                    } break;
                TU_ARM(te.binding, Union, tpb)
                    return MetadataType::None;
                TU_ARM(te.binding, Enum, tpb)
                    return MetadataType::None;
                default:
                    MIR_BUG(*m_mir_res, "Unbound/opaque path in trans - " << ty);
                }
                throw "";
            }
            else {
                return MetadataType::None;
            }
        }

        void emit_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "struct " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            auto drop_glue_path = ::HIR::Path(::HIR::TypeRef::new_path(p.clone(), &item), "#drop_glue");

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            struct H {
                static ::HIR::TypeRef get_metadata_type(const Span& sp, const ::StaticTraitResolve& resolve, const TypeRepr& r)
                {
                    ASSERT_BUG(sp, r.fields.size() > 0, "");
                    auto& t = r.fields.back().ty;
                    if( t == ::HIR::CoreType::Str ) {
                        return ::HIR::CoreType::Usize;
                    }
                    else if( t.data().is_Slice() ) {
                        return ::HIR::CoreType::Usize;
                    }
                    else if( t.data().is_TraitObject() ) {
                        const auto& te = t.data().as_TraitObject();
                        //auto vtp = t.m_data.as_TraitObject().m_trait.m_path;

                        const auto& trait = resolve.m_crate.get_trait_by_path(sp, te.m_trait.m_path.m_path);
                        auto vtable_ty = trait.get_vtable_type(sp, resolve.m_crate, te);
                        return ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, std::move(vtable_ty));
                    }
                    else if( t.data().is_Path() ) {
                        auto* repr = Target_GetTypeRepr(sp, resolve, t);
                        ASSERT_BUG(sp, repr, "No repr for " << t);
                        return get_metadata_type(sp, resolve, *repr);
                    }
                    else {
                        BUG(sp, "Unexpected type in get_metadata_type - " << t);
                    }
                }
            };


            // Generate the drop glue (and determine if there is any)
            bool has_drop_glue = m_resolve.type_needs_drop_glue(sp, ty);

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for struct " << ty);
            m_of << "type " << Trans_Mangle(p) << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            if( repr->size == SIZE_MAX )
            {
                m_of << "\tDSTMETA " << H::get_metadata_type(sp, m_resolve, *repr) << ";\n";
            }
            if( has_drop_glue )
            {
                m_of << "\tDROP " << fmt(drop_glue_path) << ";\n";
            }
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << fmt(e.ty) << ";\n";
            }
            m_of << "}\n";

            m_mir_res = nullptr;
        }
        void emit_constructor_enum(const Span& sp, const ::HIR::GenericPath& var_path, const ::HIR::Enum& item, size_t var_idx) override
        {
            TRACE_FUNCTION_F(var_path);

            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &var_path.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };

            auto enum_path = var_path.clone();
            enum_path.m_path.m_components.pop_back();

            // Create constructor function
            const auto& var_ty = item.m_data.as_Data().at(var_idx).type;
            const auto& e = var_ty.data().as_Path().binding.as_Struct()->m_data.as_Tuple();
            m_of << "/* " << var_path << " */\n";
            m_of << "fn " << fmt(var_path) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                m_of << fmt(monomorph(e[i].ent));
            }
            m_of << "): " << fmt(enum_path) << " {\n";
            m_of << "\t0: {\n";
            m_of << "\t\tASSIGN RETURN = ENUM " << fmt(enum_path) << " " << var_idx << " { ";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                m_of << "arg" << i;
            }
            m_of << " };\n";
            m_of << "\t\tRETURN\n";
            m_of << "\t}\n";
            m_of << "}";
        }
        void emit_constructor_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &p.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };
            // Create constructor function
            const auto& e = item.m_data.as_Tuple();
            m_of << "/* " << p << " */\n";
            m_of << "fn " << fmt(p) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                m_of << fmt(monomorph(e[i].ent));
            }
            m_of << "): " << fmt(p) << " {\n";
            m_of << "\t0: {\n";
            m_of << "\t\tASSIGN RETURN = { ";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                m_of << "arg" << i;
            }
            m_of << " }: " << fmt(p) << ";\n";
            m_of << "\t\tRETURN\n";
            m_of << "\t}\n";
            m_of << "}\n";
        }
        void emit_union(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Union& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "union " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            bool has_drop_glue = m_resolve.type_needs_drop_glue(sp, ty);
            auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for union " << ty);
            m_of << "type " << fmt(p) << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            if( has_drop_glue )
            {
                m_of << "\tDROP " << fmt(drop_glue_path) << ";\n";
            }
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << fmt(e.ty) << ";\n";
            }
            m_of << "}\n";
        }

        void emit_enum(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Enum& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;


            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            // Generate the drop glue (and determine if there is any)
            bool has_drop_glue = m_resolve.type_needs_drop_glue(sp, ty);
            auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");

            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for enum " << ty);
            m_of << "type " << fmt(p) << " {\n";
            m_of << "\tSIZE " << repr->size << ", ALIGN " << repr->align << ";\n";
            if( has_drop_glue )
            {
                m_of << "\tDROP " << fmt(drop_glue_path) << ";\n";
            }
            for(const auto& e : repr->fields)
            {
                m_of << "\t" << e.offset << " = " << fmt(e.ty) << ";\n";
            }

            auto emit_value = [&](const TypeRepr::FieldPath& path, uint64_t v) {
                m_of << "\"";
                for(size_t i = 0; i < path.size; i ++)
                {
                    int val = (v >> (i*8)) & 0xFF;
                    if(val < 16)
                        m_of << ::std::hex << "\\x0" << val << ::std::dec;
                    else
                        m_of << ::std::hex << "\\x" << val << ::std::dec;
                }
                m_of << "\"";
                };

            switch(repr->variants.tag())
            {
            case TypeRepr::VariantMode::TAGDEAD:    throw "";
            TU_ARM(repr->variants, None, _e) {
                }
            TU_ARM(repr->variants, Linear, e) {
                m_of << "\t@[" << e.field.index << ", " << e.field.sub_fields << "] = {\n";
                for(size_t i = 0; i < e.num_variants; i ++ )
                {
                    m_of << "\t\t";

                    if( e.is_niche(i) ) {
                        m_of << "*";
                    }
                    else {
                        emit_value(e.field, e.offset + i);
                    }
                    // - Data field number (optional)
                    if( !item.is_value() )
                    {
                        m_of << " =" << i;
                    }
                    m_of << ",\n";
                }
                m_of << "\t\t}\n";
                }
            TU_ARM(repr->variants, Values, e) {
                m_of << "\t@[" << e.field.index << ", " << e.field.sub_fields << "] = {\n";
                for(size_t idx = 0; idx < e.values.size(); idx ++)
                {
                    m_of << "\t\t";
                    // - Tag value
                    emit_value(e.field, e.values[idx]);
                    // - Data field number (optional)
                    if( !item.is_value() )
                    {
                        m_of << " =" << idx;
                    }
                    m_of << ",\n";
                }
                m_of << "\t}\n";
                }
            TU_ARM(repr->variants, NonZero, e) {
                m_of << "\t@[" << e.field.index << ", " << e.field.sub_fields << "] = { ";
                for(size_t i = 0; i < 2; i ++)
                {
                    if( i == 1 ) {
                        m_of << ", ";
                    }

                    if( e.zero_variant == i ) {
                        m_of << "\"";
                        for(size_t i = 0; i < e.field.size; i ++)
                        {
                            m_of << "\\0";
                        }
                        m_of << "\"";
                    }
                    else {
                        m_of << "* =" << i;
                    }
                }
                m_of << " }\n";
                }
            }
            m_of << "}\n";

            m_mir_res = nullptr;
        }

        void emit_str_byte(uint8_t b) {
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
        }
        void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);

            auto type = params.monomorph(m_resolve, item.m_type);
            const auto& encoded = item.m_value_res;

            m_of << "static " << fmt(p) << ": " << fmt(type) << " = \"";
            for(auto b : encoded.bytes)
                emit_str_byte(b);
            m_of << "\"";
            m_of << "{";
            for(const auto& r : encoded.relocations)
            {
                m_of << "@" << r.ofs << "+" << r.len << " = ";
                if( r.p )
                    m_of << fmt(*r.p);
                else
                    m_of << "\"" << FmtEscaped(r.bytes) << "\"";
                m_of << ",";
            }
            m_of << "}";
            m_of << ";\n";

            m_mir_res = nullptr;
        }

        void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern fn " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            // If the function is a C external, emit as such
            if( item.m_linkage.name != "" )
            {
                ::HIR::TypeRef  ret_type_tmp;
                const auto& ret_type = monomorphise_fcn_return(ret_type_tmp, item, params);

                m_of << "/* " << p << " */\n";
                m_of << "fn " << fmt(p) << "(";
                for(unsigned int i = 0; i < item.m_args.size(); i ++)
                {
                    if( i != 0 )    m_of << ", ";
                    m_of << fmt(params.monomorph(m_resolve, item.m_args[i].second));
                }
                m_of << "): " << fmt(ret_type) << " = \"" << item.m_linkage.name << "\":\"" << item.m_abi << "\";\n";
            }

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

            // - Signature
            m_of << "/* " << p << " */\n";
            m_of << "fn " << fmt(p) << "(";
            for(unsigned int i = 0; i < item.m_args.size(); i ++)
            {
                if( i != 0 )    m_of << ", ";
                m_of << fmt(params.monomorph(m_resolve, item.m_args[i].second));
            }
            m_of << "): " << fmt(ret_type);
            if( item.m_linkage.name != "" )
            {
                m_of << " = \"" << item.m_linkage.name << "\":\"" << item.m_abi << "\"";
            }
            m_of << " {\n";
            // - Locals
            for(unsigned int i = 0; i < code->locals.size(); i ++) {
                DEBUG("var" << i << " : " << code->locals[i]);
                m_of << "\tlet var" << i << ": " << fmt(code->locals[i]) << ";\n";
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
                            m_of << "CAST " << fmt(e.val) << " as " << fmt(e.type);
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
                            case ::MIR::eBinOp::DIV:    m_of << "/";    break;
                            case ::MIR::eBinOp::DIV_OV: m_of << "/^";   break;
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
                            m_of << "MAKEDST " << fmt(e.ptr_val) << ", " << fmt(e.meta_val);
                            break;
                        TU_ARM(se.src, UnionVariant, e)
                            m_of << "UNION " << fmt(e.path) << " " << e.index << " " << fmt(e.val);
                            break;
                        TU_ARM(se.src, EnumVariant, e) {
                            m_of << "ENUM " << fmt(e.path) << " " << e.index << " { ";
                            for(const auto& v : e.vals)
                            {
                                m_of << fmt(v) << ", ";
                            }
                            m_of << "}";
                            } break;
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
                            m_of << "}: " << fmt(e.path);
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
                        m_of << ") = \"" << FmtEscaped(se.tpl) << "\"(";
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
                    TU_ARM(stmt, Asm2, se) {
                        m_of << "ASM2 (";
                        for(const auto& l : se.lines)
                            m_of << l;
                        for(const auto& p : se.params)
                        {
                            m_of << ", ";
                            TU_MATCH_HDRA((p), {)
                            TU_ARMA(Const, v)
                                m_of << "const " << fmt(v);
                            TU_ARMA(Sym, v)
                                m_of << "sym " << fmt(v);
                            TU_ARMA(Reg, v) {
                                m_of << "reg(" << v.dir << " " << v.spec << ") ";
                                if(v.input ) m_of << fmt(*v.input ); else m_of << "_";
                                m_of << " => ";
                                if(v.output) m_of << fmt(*v.output); else m_of << "_";
                                }
                            }
                        }
                        m_of << ", ";
                        se.options.fmt(m_of);
                        m_of << ")";
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
                    m_of << "INCOMPLETE\n";
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
                    TU_ARM(e.values, ByteString, ve) {
                        for(size_t j = 0; j < ve.size(); j ++)
                        {
                            m_of << "b\"";
                            for(size_t i = 0; i < ve[j].size(); i ++) {
                                auto b = ve[j][i];
                                switch(b)
                                {
                                case '\\': m_of << "\\\\"; break;
                                case '\"': m_of << "\\\""; break;
                                default:
                                    if( ' ' <= b && b < 0x7f ) {
                                        m_of << char(ve[j][i]);
                                    }
                                    else {
                                        m_of << "\\x";
                                        m_of << "0123456789ABCDEF"[b >> 4];
                                        m_of << "0123456789ABCDEF"[b & 15];
                                    }
                                    break;
                                }
                            }
                            m_of << "\" = " << e.targets[i] << ",";
                        }
                        } break;
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
                    TU_ARM(e.fcn, Intrinsic, f) {
                        m_of << "\"" << f.name << "\"";
                        if( f.params.m_types.size() > 0 ) {
                            m_of << "<";
                            for(const auto& t : f.params.m_types)
                                m_of << fmt(t) << ",";
                            m_of << ">";
                        }
                        } break;
                    TU_ARM(e.fcn, Value, f)     m_of << "(" << fmt(f) << ")";  break;
                    TU_ARM(e.fcn, Path, f)      m_of << fmt(f);  break;
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
            bool has_erased = visit_ty_with(item.m_return, [&](const auto& x) { return x.data().is_ErasedType(); });
            
            if( has_erased || monomorphise_type_needed(item.m_return) )
            {
                // If there's an erased type, make a copy with the erased type expanded
                if( has_erased )
                {
                    tmp = clone_ty_with(sp, item.m_return, [&](const auto& x, auto& out) {
                        if( const auto* te = x.data().opt_ErasedType() ) {
                            out = item.m_code.m_erased_types.at(te->m_index).clone();
                            return true;
                        }
                        else {
                            return false;
                        }
                        });
                    tmp = params.monomorph_type(Span(), tmp).clone();
                }
                else
                {
                    tmp = params.monomorph_type(Span(), item.m_return).clone();
                }
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
