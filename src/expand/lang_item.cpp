/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * synexts/lang_item.cpp
 * - Binds language items to #[lang_item] tagged items
 */
#include <synext.hpp>
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../ast/crate.hpp"


void handle_lang_item(const Span& sp, AST::Crate& crate, const AST::Path& path, const ::std::string& name, AST::eItemType type)
{
    if(name == "phantom_fn") {
        // - Just save path
    }
    else if( name == "send" ) {
        // Don't care, Send is fully library in mrustc
        // - Needed for `static`
    }
    else if( name == "sync" ) {
        // Don't care, Sync is fully library in mrustc
        // - Needed for `static`
    }
    else if( name == "sized" ) {
        DEBUG("Bind 'sized' to " << path);
    }
    else if( name == "copy" ) {
        DEBUG("Bind 'copy' to " << path);
    }
    else if( TARGETVER_1_29 && name == "clone" ) {}   // - Trait
    // ops traits
    else if( name == "drop" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "add" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "sub" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "mul" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "div" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "rem" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "neg" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "not" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "bitand" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "bitor"  ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "bitxor" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "shl" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "shr" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "add_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "sub_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "div_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "rem_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "mul_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "bitand_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "bitor_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "bitxor_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "shl_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "shr_assign" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "index" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "deref" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "index_mut" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "deref_mut" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "fn"      ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "fn_mut"  ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "fn_once" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "eq"  ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "ord" ) { DEBUG("Bind '"<<name<<"' to " << path); }	// In 1.29 this is Ord, before it was PartialOrd
    else if( TARGETVER_1_29 && name == "partial_ord" ) { DEBUG("Bind '"<<name<<"' to " << path); }    // New name for v1.29
    else if( name == "unsize" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "coerce_unsized" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "freeze" ) { DEBUG("Bind '"<<name<<"' to " << path); }

    else if( name == "iterator" ) { /* mrustc just desugars? */ }

    else if( name == "debug_trait" ) { /* TODO: Poke derive() with this */ }

    else if( TARGETVER_1_29 && name == "termination" ) { }    // 1.29 - trait used for non-() main

    // Structs
    else if( name == "non_zero" ) { }
    else if( name == "phantom_data" ) { }
    else if( name == "range_full" ) { }
    else if( name == "range" ) { }
    else if( name == "range_from" ) { }
    else if( name == "range_to" ) { }
    else if( name == "unsafe_cell" ) { }
    else if( TARGETVER_1_29 && name == "alloc_layout") { }
    else if( TARGETVER_1_29 && name == "panic_info" ) {}    // Struct
    else if( TARGETVER_1_29 && name == "manually_drop" ) {}    // Struct

    else if( /*TARGETVER_1_39 &&*/ name == "maybe_uninit" ) {}    // Union

    // Futures
    else if( /*TARGETVER_1_39 &&*/ name == "unpin" ) {}    // Trait
    else if( /*TARGETVER_1_39 &&*/ name == "pin" ) {}    // Struct
    else if( /*TARGETVER_1_39 &&*/ name == "future_trait" ) {}    // Trait

    // Variable argument lists
    else if( /*TARGETVER_1_39 &&*/ name == "va_list" ) {}    // Struct

    // Arbitary receivers
    else if( /*TARGETVER_1_39 &&*/ name == "receiver" ) {}    // Trait
    else if( /*TARGETVER_1_39 &&*/ name == "dispatch_from_dyn" ) {}    // Trait

    // Generators
    else if( TARGETVER_1_29 && name == "generator" ) {}   // - Trait
    else if( TARGETVER_1_29 && name == "generator_state" ) {}   // - State enum

    // Statics
    else if( name == "msvc_try_filter" ) { }

    // Extern functions
    else if( name == "panic_impl" ) {
    }
    else if( name == "oom" ) {
    }

    // Functions
    else if( name == "panic" ) { }
    else if( name == "panic_bounds_check" ) { }
    else if( name == "panic_fmt" ) { }
    else if( name == "str_eq" ) { }
    else if( name == "drop_in_place" ) { }
    else if( name == "align_offset" ) { }
    // - builtin `box` support
    else if( name == "exchange_malloc" ) { }
    else if( name == "exchange_free" ) { }
    else if( name == "box_free" ) { }
    else if( name == "owned_box" ) { }
    // - start
    else if( name == "start" ) { }

    else if( name == "eh_personality" ) { }
    // libcompiler_builtins
    // - i128/u128 helpers (not used by mrustc)
    else if( name == "i128_add" ) { }
    else if( name == "i128_addo" ) { }
    else if( name == "u128_add" ) { }
    else if( name == "u128_addo" ) { }
    else if( name == "i128_sub" ) { }
    else if( name == "i128_subo" ) { }
    else if( name == "u128_sub" ) { }
    else if( name == "u128_subo" ) { }
    else if( name == "i128_mul" ) { }
    else if( name == "i128_mulo" ) { }
    else if( name == "u128_mul" ) { }
    else if( name == "u128_mulo" ) { }
    else if( name == "i128_div" ) { }
    else if( name == "i128_rem" ) { }
    else if( name == "u128_div" ) { }
    else if( name == "u128_rem" ) { }
    else if( name == "i128_shl" ) { }
    else if( name == "i128_shlo" ) { }
    else if( name == "u128_shl" ) { }
    else if( name == "u128_shlo" ) { }
    else if( name == "i128_shr" ) { }
    else if( name == "i128_shro" ) { }
    else if( name == "u128_shr" ) { }
    else if( name == "u128_shro" ) { }

    else {
        ERROR(sp, E0000, "Unknown language item '" << name << "'");
    }

    if( type == AST::ITEM_EXTERN_FN )
    {
        // TODO: This should force a specific link name instead
        return ;
    }

    auto rv = crate.m_lang_items.insert( ::std::make_pair( name, ::AST::Path(path) ) );
    if( !rv.second ) {
        const auto& other_path = rv.first->second;
        if( path != other_path ) {
            // HACK: Anon modules get visited twice, so can lead to duplicate annotations
            ERROR(sp, E0000, "Duplicate definition of language item '" << name << "' - " << other_path << " and " << path);
        }
    }
}

class Decorator_LangItem:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        TU_MATCH_DEF(::AST::Item, (i), (e),
        (
            TODO(sp, "Unknown item type " << i.tag_str() << " with #["<<attr<<"] attached at " << path);
            ),
        (None,
            // NOTE: Can happen when #[cfg] removed this
            ),
        (Function,
            if( e.code().is_valid() ) {
                handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_FN);
            }
            else {
                handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_EXTERN_FN);
            }
            ),
        (Static,
            handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_STATIC);
            ),
        (Struct,
            handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_STRUCT);
            ),
        (Enum,
            handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_ENUM);
            ),
        (Union,
            handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_UNION);
            ),
        (Trait,
            handle_lang_item(sp, crate, path, attr.string(), AST::ITEM_TRAIT);
            )
        )
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const override {
        const ::std::string& name = mi.string();

             if( name == "i8" ) {}
        else if( name == "u8" ) {}
        else if( name == "i16" ) {}
        else if( name == "u16" ) {}
        else if( name == "i32" ) {}
        else if( name == "u32" ) {}
        else if( name == "i64" ) {}
        else if( name == "u64" ) {}
        else if( name == "i128" ) {}
        else if( name == "u128" ) {}
        else if( name == "isize" ) {}
        else if( name == "usize" ) {}
        else if( name == "const_ptr" ) {}
        else if( name == "mut_ptr" ) {}
        else if( /*TARGETVER_1_39 &&*/ name == "bool" ) {}
        // rustc_unicode
        else if( name == "char" ) {}
        // collections
        else if( name == "str" ) {}
        else if( name == "slice" ) {}
        else if( TARGETVER_1_29 && name == "slice_u8" ) {}  // libcore now, `impl [u8]`
        else if( TARGETVER_1_29 && name == "slice_alloc" ) {}   // liballoc's impls on [T]
        else if( TARGETVER_1_29 && name == "slice_u8_alloc" ) {}   // liballoc's impls on [u8]
        else if( TARGETVER_1_29 && name == "str_alloc" ) {}   // liballoc's impls on str
        // std - interestingly
        else if( name == "f32" ) {}
        else if( name == "f64" ) {}
        else if( TARGETVER_1_29 && name == "f32_runtime" ) {}
        else if( TARGETVER_1_29 && name == "f64_runtime" ) {}
        else {
            ERROR(sp, E0000, "Unknown lang item '" << name << "' on impl");
        }

        // TODO: Somehow annotate these impls to allow them to provide inherents?
        // - mrustc is lazy and inefficient, so these don't matter :)
    }
};

class Decorator_Main:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        if( i.is_None() ) {
            // Ignore.
        }
        else TU_IFLET(::AST::Item, i, Function, e,
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-main"), ::AST::Path(path) ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[main] - " << other_path << " and " << path);
            }
        )
        else {
            ERROR(sp, E0000, "#[main] on non-function " << path);
        }
    }
};

class Decorator_Start:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        TU_IFLET(::AST::Item, i, Function, e,
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-start"), ::AST::Path(path) ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[start] - " << other_path << " and " << path);
            }
        )
        else {
            ERROR(sp, E0000, "#[start] on non-function " << path);
        }
    }
};

class Decorator_PanicImplementation:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        TU_IFLET(::AST::Item, i, Function, e,
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-panic_implementation"), ::AST::Path(path) ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[panic_implementation] - " << other_path << " and " << path);
            }
        )
        else {
            ERROR(sp, E0000, "#[panic_implementation] on non-function " << path);
        }
    }
};

STATIC_DECORATOR("lang", Decorator_LangItem)
STATIC_DECORATOR("main", Decorator_Main);
STATIC_DECORATOR("start", Decorator_Start);
STATIC_DECORATOR("panic_implementation", Decorator_PanicImplementation);


