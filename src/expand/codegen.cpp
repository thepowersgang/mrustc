/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/codegen.cpp
* - Attributes that influence codegen and layouts
*/
#include <synext.hpp>
#include <ast/generics.hpp>
#include <ast/ast.hpp>
#include <parse/ttstream.hpp>
#include <expand/cfg.hpp>

namespace {
    class Common_Function:
        public ExpandDecorator
    {
    public:
        virtual void handle(const AST::Attribute& mi, AST::Function& fcn) const = 0;

        AttrStage   stage() const override { return AttrStage::Pre; }

        void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_None() ) {
            }
            else if( i.is_Function() ) {
                this->handle(mi, i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
        void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_None() ) {
            }
            else if( i.is_Function() ) {
                this->handle(mi, i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
        void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_None() ) {
            }
            else if( i.is_Function() ) {
                this->handle(mi, i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
    };
}

class CHandler_Inline:
    public Common_Function
{
public:
    void handle(const AST::Attribute& mi, AST::Function& fcn) const override {
        TTStream    lex(mi.span(), ParseState(), mi.data());
        //ASSERT_BUG(mi.span(), fcn.m_markings.inline_type == AST::Function::Markings::Inline::Auto, "Duplicate #[inline] attributes");
        if( lex.getTokenIf(TOK_PAREN_OPEN) )
        {
            auto attr = lex.getTokenCheck(TOK_IDENT).ident().name;
            if( attr == "never" ) {
                fcn.m_markings.inline_type = AST::Function::Markings::Inline::Never;
            }
            else if( attr == "always" ) {
                fcn.m_markings.inline_type = AST::Function::Markings::Inline::Always;
            }
            else {
                ERROR(lex.point_span(), E0000, "Unknown inline type #[inline(" << attr << ")]");
            }
            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
        }
        else
        {
            fcn.m_markings.inline_type = AST::Function::Markings::Inline::Normal;
        }
    }
};
STATIC_DECORATOR("inline", CHandler_Inline);
class CHandler_Cold:
    public Common_Function
{
public:
    void handle(const AST::Attribute& mi, AST::Function& fcn) const override {
        TTStream    lex(mi.span(), ParseState(), mi.data());
        lex.getTokenCheck(TOK_EOF);
        ASSERT_BUG(mi.span(), !fcn.m_markings.is_cold, "Duplicate #[cold] attributes");
        fcn.m_markings.is_cold = true;
    }
};
STATIC_DECORATOR("cold", CHandler_Cold);

class CHandler_rustc_legacy_const_generics:
    public Common_Function
{
    void handle(const AST::Attribute& mi, AST::Function& fcn) const override {
        TTStream    lex(mi.span(), ParseState(), mi.data());
        lex.getTokenCheck(TOK_PAREN_OPEN);

        auto& list = fcn.m_markings.rustc_legacy_const_generics;
        do {
            auto idx = lex.getTokenCheck(TOK_INTEGER).intval();
            ASSERT_BUG(lex.point_span(), std::find(list.begin(), list.end(), idx) == list.end(), "#[rustc_legacy_const_generics(" << idx << ")] duplicate index");
            list.push_back(idx);
        } while( lex.getTokenIf(TOK_COMMA) );

        lex.getTokenCheck(TOK_PAREN_CLOSE);
        lex.getTokenCheck(TOK_EOF);
    }
};
STATIC_DECORATOR("rustc_legacy_const_generics", CHandler_rustc_legacy_const_generics);

class CHandler_Repr:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if(i.is_None())
        {
        }
        // --- struct ---
        else if( auto* s = i.opt_Struct() )
        {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);
            do {
                auto repr_type = lex.getTokenCheck(TOK_IDENT).ident().name;
                if( repr_type == "C" ) {
                    switch( s->m_markings.repr )
                    {
                    case AST::Struct::Markings::Repr::Rust:
                        s->m_markings.repr = AST::Struct::Markings::Repr::C;
                        break;
                    default:
                        // TODO: Error
                        break;
                    }
                }
                else if( repr_type == "packed" ) {
                    switch( s->m_markings.repr )
                    {
                    case AST::Struct::Markings::Repr::C:
                    case AST::Struct::Markings::Repr::Rust:
                        break;
                    default:
                        // TODO: Error
                        break;
                    }
                    if( s->m_markings.max_field_align != 0 ) {
                        // TODO: Error
                    }
                    if( lex.getTokenIf(TOK_PAREN_OPEN) )
                    {
                        auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);
                        auto* val = dynamic_cast<AST::ExprNode_Integer*>(&*n);
                        ASSERT_BUG(n->span(), val, "#[repr(packed(...))] - alignment must be an integer");
                        auto v = val->m_value;
                        ASSERT_BUG(lex.point_span(), v > 0, "#[repr(packed(" << v << "))] - alignment must be non-zero");
                        ASSERT_BUG(lex.point_span(), (v & (v-1)) == 0, "#[repr(packed(" << v << "))] - alignment must be a power of two");
                        ASSERT_BUG(lex.point_span(), s->m_markings.align_value == 0, "#[repr(packed(" << v << "))] - conflicts with previous alignment");
                        // TODO: I believe this should change the internal aligment too?
                        s->m_markings.max_field_align = v;
                        lex.getTokenCheck(TOK_PAREN_CLOSE);
                    }
                    else
                    {
                        s->m_markings.max_field_align = 1;
                    }
                }
                else if( repr_type == "simd" ) {
                    s->m_markings.repr = AST::Struct::Markings::Repr::Simd;
                }
                else if( repr_type == "transparent" ) {
                    s->m_markings.repr = AST::Struct::Markings::Repr::Transparent;
                }
                else if( repr_type == "align" ) {
                    lex.getTokenCheck(TOK_PAREN_OPEN);
                    auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);
                    auto* val = dynamic_cast<AST::ExprNode_Integer*>(&*n);
                    ASSERT_BUG(n->span(), val, "#[repr(align(...))] - alignment must be an integer");
                    auto v = val->m_value;
                    ASSERT_BUG(lex.point_span(), v > 0, "#[repr(align(" << v << "))] - alignment must be non-zero");
                    ASSERT_BUG(lex.point_span(), (v & (v-1)) == 0, "#[repr(align(" << v << "))] - alignment must be a power of two");
                    ASSERT_BUG(lex.point_span(), s->m_markings.align_value == 0, "#[repr(align(" << v << "))] - conflicts with previous alignment");
                    s->m_markings.align_value = v;
                    lex.getTokenCheck(TOK_PAREN_CLOSE);
                }
                else if( repr_type == "no_niche" ) {
                    // TODO: rust-lang/rust#68303 happens with UnsafeCell and niche optionisations
                    // - Would mrustc also have this?
                }
                else {
                    TODO(sp, "Handle struct repr '" << repr_type << "'");
                }
            } while(lex.getTokenIf(TOK_COMMA));
            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
        }
        // --- enum ---
        else if( auto* e = i.opt_Enum() )
        {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);

            // Loop, so `repr(C, u8)` is valid
            while( lex.lookahead(0) != TOK_PAREN_CLOSE )
            {
                auto set_repr = [&](::AST::Enum::Markings::Repr r) {
                    ASSERT_BUG(lex.point_span(), e->m_markings.repr == ::AST::Enum::Markings::Repr::Rust, "Multiple enum reprs set");
                    e->m_markings.repr = r;
                    };
                auto repr_str = lex.getTokenCheck(TOK_IDENT).ident().name;
                if( repr_str == "C" ) {
                    // Repeated is OK
                    e->m_markings.is_repr_c = true;
                }
                else if( repr_str == "u8"   ) { set_repr( ::AST::Enum::Markings::Repr::U8 ); }
                else if( repr_str == "u16"  ) { set_repr( ::AST::Enum::Markings::Repr::U16); }
                else if( repr_str == "u32"  ) { set_repr( ::AST::Enum::Markings::Repr::U32); }
                else if( repr_str == "u64"  ) { set_repr( ::AST::Enum::Markings::Repr::U64); }
                else if( repr_str == "usize") { set_repr( ::AST::Enum::Markings::Repr::Usize); }
                else if( repr_str == "i8"   ) { set_repr( ::AST::Enum::Markings::Repr::I8 ); }
                else if( repr_str == "i16"  ) { set_repr( ::AST::Enum::Markings::Repr::I16); }
                else if( repr_str == "i32"  ) { set_repr( ::AST::Enum::Markings::Repr::I32); }
                else if( repr_str == "i64"  ) { set_repr( ::AST::Enum::Markings::Repr::I64); }
                else if( repr_str == "isize") { set_repr( ::AST::Enum::Markings::Repr::Isize); }
                else {
                    ERROR(lex.point_span(), E0000, "Unknown enum repr '" << repr_str << "'");
                }
                if( !lex.getTokenIf(TOK_COMMA) )
                    break;
            }

            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
        }
        // --- union ---
        else if( auto* e = i.opt_Union() )
        {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);

            auto repr_str = lex.getTokenCheck(TOK_IDENT).ident().name;
            if(repr_str == "C") {
                e->m_markings.repr = ::AST::Union::Markings::Repr::C;
            }
            else if(repr_str == "transparent") {
                e->m_markings.repr = ::AST::Union::Markings::Repr::Transparent;
            }
            else {
                ERROR(lex.point_span(), E0000, "Unknown union repr '" << repr_str << "'");
            }

            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
        }
        else {
            ERROR(mi.span(), E0000, "Unexpected attribute #[repr] on " << i.tag_str());
        }
    }
};
STATIC_DECORATOR("repr", CHandler_Repr);

class CHandler_RustcNonnullOptimizationGuaranteed:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( i.is_Struct() ) {
        }
        else {
        }
    }
};
STATIC_DECORATOR("rustc_nonnull_optimization_guaranteed", CHandler_RustcNonnullOptimizationGuaranteed);

// 1.39
class CHandler_RustcLayoutScalarValidRangeStart:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( auto* s = i.opt_Struct() ) {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);
            auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);
            auto* np = dynamic_cast<AST::ExprNode_Integer*>(n.get());
            ASSERT_BUG(n->span(), np, "#[rustc_layout_scalar_valid_range_start] requires an integer - got " << FMT_CB(ss, n->print(ss)));
            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);

            s->m_markings.scalar_valid_start_set = true;
            s->m_markings.scalar_valid_start = np->m_value;
            DEBUG(path << " #[rustc_layout_scalar_valid_range_start]: " << std::hex << s->m_markings.scalar_valid_start);
        }
        else {
            TODO(sp, "#[rustc_layout_scalar_valid_range_start] on " << i.tag_str());
        }
    }
};
STATIC_DECORATOR("rustc_layout_scalar_valid_range_start", CHandler_RustcLayoutScalarValidRangeStart);
class CHandler_RustcLayoutScalarValidRangeEnd:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( auto* s = i.opt_Struct() ) {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);
            auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);
            auto* np = dynamic_cast<AST::ExprNode_Integer*>(n.get());
            ASSERT_BUG(n->span(), np, "#[rustc_layout_scalar_valid_range_end] requires an integer - got " << FMT_CB(ss, n->print(ss)));
            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
            s->m_markings.scalar_valid_end_set = true;
            s->m_markings.scalar_valid_end = np->m_value;
            DEBUG(path << " #[rustc_layout_scalar_valid_range_end]: " << std::hex << s->m_markings.scalar_valid_end);
        }
        else {
            TODO(sp, "#[rustc_layout_scalar_valid_range_end] on " << i.tag_str());
        }
    }
};
STATIC_DECORATOR("rustc_layout_scalar_valid_range_end", CHandler_RustcLayoutScalarValidRangeEnd);

class CHandler_LinkName:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        auto link_name = mi.parse_equals_string(crate, mod);
        ASSERT_BUG(sp, link_name != "", "Empty #[link_name] attribute");

        if(i.is_None()) {
        }
        else if( auto* fcn = i.opt_Function() )
        {
            ASSERT_BUG(sp, fcn->m_markings.link_name == "", "Duplicate #[link_name] attributes");
            fcn->m_markings.link_name = link_name;
        }
        else if( auto* st = i.opt_Static() )
        {
            ASSERT_BUG(sp, st->s_class() != ::AST::Static::CONST, "#[link_name] on `const`");
            ASSERT_BUG(sp, st->m_markings.link_name == "", "Duplicate #[link_name] attributes");
            st->m_markings.link_name = link_name;
        }
        else {
        }
    }
};
STATIC_DECORATOR("link_name", CHandler_LinkName);

class CHandler_LinkSection:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        auto link_section = mi.parse_equals_string(crate, mod);
        ASSERT_BUG(sp, link_section != "", "Empty #[link_section] attribute");

        if(i.is_None()) {
        }
        else if( auto* fcn = i.opt_Function() )
        {
            ASSERT_BUG(sp, fcn->m_markings.link_section == "", "Duplicate #[link_section] attributes");
            fcn->m_markings.link_section = link_section;
        }
        else if( auto* st = i.opt_Static() )
        {
            ASSERT_BUG(sp, st->s_class() != ::AST::Static::CONST, "#[link_section] on `const`");
            ASSERT_BUG(sp, st->m_markings.link_section == "", "Duplicate #[link_section] attributes");
            st->m_markings.link_section = link_section;
        }
        else {
        }
    }
};
STATIC_DECORATOR("link_section", CHandler_LinkSection);

class CHandler_Link:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if(i.is_None()) {
        }
        else if( auto* b = i.opt_ExternBlock() )
        {
            TTStream    lex(sp, ParseState(), mi.data());
            lex.getTokenCheck(TOK_PAREN_OPEN);
            std::string lib_name;
            bool emit = true;
            AST::ExternBlock::Link  link;

            while(lex.lookahead(0) != TOK_PAREN_OPEN)
            {
                auto key = lex.getTokenCheck(TOK_IDENT).ident().name;
                if( key == "name" ) {
                    lex.getTokenCheck(TOK_EQUAL);
                    auto v = lex.getTokenCheck(TOK_STRING).str();
                    if(v == "")
                        ERROR(sp, E0000, "Empty name on extern block");
                    link.lib_name = v;
                }
                else if( key == "kind" ) {
                    lex.getTokenCheck(TOK_EQUAL);
                    auto v = lex.getTokenCheck(TOK_STRING).str();
                    if(v == "")
                        ERROR(sp, E0000, "Empty `kind` on extern block #[link]");
                    // TODO: save and use the kind
                }
                else if( key == "cfg" ) {
                    emit &= check_cfg_stream(lex);
                }
                else {
                    TODO(sp, "Unknown attribute `#[link(" << key << ")]`");
                }
                if( !lex.getTokenIf(TOK_COMMA) )
                    break ;
            }
            if(link.lib_name == "")
                ERROR(sp, E0000, "No name in `#[link]`");
            if( emit )
            {
                b->m_libraries.push_back(std::move(link));
            }
            lex.getTokenCheck(TOK_PAREN_CLOSE);
            lex.getTokenCheck(TOK_EOF);
        }
        else {
        }
    }
};
STATIC_DECORATOR("link", CHandler_Link);

class CHandler_TargetFeature:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Only valid on functions?
    }
};
STATIC_DECORATOR("target_feature", CHandler_TargetFeature);
