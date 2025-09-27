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

enum eItemType
{
    ITEM_TRAIT,
    ITEM_STRUCT,
    ITEM_ENUM,
    ITEM_UNION,
    ITEM_FN,
    ITEM_EXTERN_FN,
    ITEM_STATIC,
    ITEM_TYPE_ALIAS,
};

struct Handler {
    typedef void (*cb_t)(const Span& sp, AST::Crate& crate, const std::string&, const AST::AbsolutePath&);
    eItemType type;
    cb_t    cb;
    Handler(eItemType type, cb_t cb): type(type), cb(cb) {}
};
struct StrcmpTy {
    bool operator()(const char* a, const char* b) const { return std::strcmp(a,b) < 0; }
};
static std::map<const char*,Handler,StrcmpTy>   g_handlers;

void handle_save(const Span& sp, AST::Crate& crate, const std::string& name, const AST::AbsolutePath& path)
{
    auto rv = crate.m_lang_items.insert( ::std::make_pair(name, path) );
    if( !rv.second ) {
        const auto& other_path = rv.first->second;
        if( path != other_path ) {
            // HACK: Anon modules get visited twice, so can lead to duplicate annotations
            ERROR(sp, E0000, "Duplicate definition of language item '" << name << "' - " << other_path << " and " << path);
        }
    }
    else {
        DEBUG("Bind '"<<name<<"' to " << path);
    }
}
void handle_lang_item(const Span& sp, AST::Crate& crate, const AST::AbsolutePath& path, const ::std::string& name, eItemType type)
{
    // NOTE: MSVC has a limit to the number of if-else chains
    if(g_handlers.empty())
    {
        struct H {
            static void add(const char* n, Handler h) {
                g_handlers.insert(std::make_pair(n, std::move(h)));
            }
        };
        H::add("phantom_fn", Handler(ITEM_FN, handle_save));
        H::add("send" , Handler(ITEM_TRAIT, handle_save));
        H::add("sync" , Handler(ITEM_TRAIT, handle_save));
        H::add("sized", Handler(ITEM_TRAIT, handle_save));
        H::add("copy" , Handler(ITEM_TRAIT, handle_save));
        if( TARGETVER_LEAST_1_29 )
        {
            H::add("clone", Handler(ITEM_TRAIT, handle_save));
        }
        // ops traits
        H::add("drop", Handler(ITEM_TRAIT, handle_save));
        H::add("add", Handler(ITEM_TRAIT, handle_save));
        H::add("sub", Handler(ITEM_TRAIT, handle_save));
        H::add("mul", Handler(ITEM_TRAIT, handle_save));
        H::add("div", Handler(ITEM_TRAIT, handle_save));
        H::add("rem", Handler(ITEM_TRAIT, handle_save));

        H::add("neg", Handler(ITEM_TRAIT, handle_save));
        H::add("not", Handler(ITEM_TRAIT, handle_save));

        H::add("bitand", Handler(ITEM_TRAIT, handle_save));
        H::add("bitor" , Handler(ITEM_TRAIT, handle_save));
        H::add("bitxor", Handler(ITEM_TRAIT, handle_save));
        H::add("shl", Handler(ITEM_TRAIT, handle_save));
        H::add("shr", Handler(ITEM_TRAIT, handle_save));

        H::add("add_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("sub_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("div_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("rem_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("mul_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("bitand_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("bitor_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("bitxor_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("shl_assign", Handler(ITEM_TRAIT, handle_save));
        H::add("shr_assign", Handler(ITEM_TRAIT, handle_save));

        H::add("index", Handler(ITEM_TRAIT, handle_save));
        H::add("deref", Handler(ITEM_TRAIT, handle_save));
        H::add("index_mut", Handler(ITEM_TRAIT, handle_save));
        H::add("deref_mut", Handler(ITEM_TRAIT, handle_save));
        H::add("fn"     , Handler(ITEM_TRAIT, handle_save));
        H::add("fn_mut" , Handler(ITEM_TRAIT, handle_save));
        H::add("fn_once", Handler(ITEM_TRAIT, handle_save));

        H::add("eq" , Handler(ITEM_TRAIT, handle_save));
        H::add("ord", Handler(ITEM_TRAIT, handle_save));	// In 1.29 this is Ord, before it was PartialOrd
        if( TARGETVER_LEAST_1_29 )
            H::add("partial_ord", Handler(ITEM_TRAIT, handle_save));    // New name for v1.29

        H::add("unsize"        , Handler(ITEM_TRAIT, handle_save));
        H::add("coerce_unsized", Handler(ITEM_TRAIT, handle_save));
        H::add("freeze", Handler(ITEM_TRAIT, handle_save));    // TODO: What version?

        H::add("iterator", Handler(ITEM_TRAIT, handle_save));  /* mrustc just desugars? */
        H::add("debug_trait", Handler(ITEM_TRAIT, handle_save));   /* TODO: Poke derive() with this */

        if( TARGETVER_LEAST_1_29 )
            H::add("termination", Handler(ITEM_TRAIT, handle_save));   // 1.29 - trait used for non-() main

        if(TARGETVER_LEAST_1_54)
        {
            H::add("pointee_trait", Handler(ITEM_TRAIT, handle_save));  // 1.54 - pointer metadata trait
            H::add("dyn_metadata", Handler(ITEM_STRUCT, handle_save));  // 1.54 - `dyn Trait` metadata structure
            H::add("structural_peq", Handler(ITEM_TRAIT, handle_save)); // 1.54 - Structural equality trait (partial)
            H::add("structural_teq", Handler(ITEM_TRAIT, handle_save)); // 1.54 - Structural equality trait (total)
            H::add("discriminant_kind", Handler(ITEM_TRAIT, handle_save));  // 1.54 - trait: used for the `discriminant_kind` intrinsic
        }


        H::add("non_zero", Handler(ITEM_STRUCT, handle_save));
        H::add("phantom_data", Handler(ITEM_STRUCT, handle_save));

        if(TARGETVER_LEAST_1_54)
        {
            H::add("RangeFull", Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range_full", p); }));
            H::add("Range"    , Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range"     , p); }));
            H::add("RangeFrom", Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range_from", p); }));
            H::add("RangeTo"  , Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range_to"  , p); }));
            H::add("RangeInclusive"  , Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range_inclusive"   , p); }));
            H::add("RangeToInclusive", Handler(ITEM_STRUCT, [](const auto& sp, auto& crate, const auto& , const auto& p){ handle_save(sp, crate, "range_to_inclusive", p); }));
        }
        else
        {
            H::add("range_full", Handler(ITEM_STRUCT, handle_save));
            H::add("range"     , Handler(ITEM_STRUCT, handle_save));
            H::add("range_from", Handler(ITEM_STRUCT, handle_save));
            H::add("range_to"  , Handler(ITEM_STRUCT, handle_save));
        }

        if( TARGETVER_LEAST_1_54 )
        {
            H::add("unwind_safe", Handler(ITEM_TRAIT, handle_save));    // 1.54 - UnwindSafe trait
            H::add("ref_unwind_safe", Handler(ITEM_TRAIT, handle_save));    // 1.54 - RefUnwindSafe trait
        }
        if( TARGETVER_LEAST_1_74 )
        {
            H::add("transmute_trait", Handler(ITEM_TRAIT, handle_save));    // 1.74 - `BikeshedIntrinsicFrom` trait
            // - Markers
            H::add("destruct"       , Handler(ITEM_TRAIT, handle_save));    // 1.74 - `Destruct` trait
            H::add("tuple_trait"    , Handler(ITEM_TRAIT, handle_save));    // 1.74 - `Tuple` trait (must be implemented for all tuples)
            H::add("pointer_like"   , Handler(ITEM_TRAIT, handle_save));    // 1.74 - `PointerLike` trait
            H::add("const_param_ty" , Handler(ITEM_TRAIT, handle_save));    // 1.74 - `ConstParamTy` trait
            H::add("fn_ptr_trait"   , Handler(ITEM_TRAIT, handle_save));    // 1.74 - `FnPtr` trait

            // Structs
            H::add("transmute_opts" , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `Assume` struct
            H::add("ptr_unique"     , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::ptr::Unique`
            H::add("CStr"           , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::ffi::CStr` - Why? (miri?)
            H::add("String"         , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::alloc::string::String` - Why? (miri?)

            H::add("from_yeet"      , Handler(ITEM_FN, handle_save));   // 1.74 - `::core::try_trait::from_yeet`
            H::add("panic_nounwind" , Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic`
            H::add("panic_display"  , Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic_display`
            H::add("panic_bounds_check", Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic_bounds_check`
            H::add("panic_misaligned_pointer_dereference", Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic_misaligned_pointer_dereference`
            H::add("panic_cannot_unwind", Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic_cannot_unwind`
            H::add("panic_in_cleanup", Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::panic_in_cleanup `
            H::add("const_panic_fmt", Handler(ITEM_FN, handle_save));   // 1.74 - `::core::panicking::const_panic_fmt`

            // Enums
            H::add("c_void"         , Handler(ITEM_ENUM, handle_save));   // 1.74 - `::core::ffi::c_void` - Why? (miri?)
            H::add("Option"         , Handler(ITEM_ENUM, handle_save));   // 1.74 - `::core::option::Option`

            // - Formatting
            H::add("format_arguments"  , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::fmt::Arguments`
            H::add("format_placeholder", Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::fmt::rt::Placeholder`
            H::add("format_argument"   , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::fmt::rt::Argument`
            H::add("format_unsafe_arg" , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::fmt::rt::UnsafeArg`
            H::add("format_alignment"  , Handler(ITEM_ENUM, handle_save));   // 1.74 - `::core::fmt::rt::Alignment`
            H::add("format_count"      , Handler(ITEM_ENUM, handle_save));   // 1.74 - `::core::fmt::rt::Count`

            // - Futures
            H::add("ResumeTy"  , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::future::ResumeTy`
            H::add("Poll"      , Handler(ITEM_ENUM  , handle_save));   // 1.74 - `::core::task::poll::Poll`
            H::add("Context"   , Handler(ITEM_STRUCT, handle_save));   // 1.74 - `::core::task::wake::Context`
        }
        if( TARGETVER_LEAST_1_90 )
        {
            H::add("contract_build_check_ensures", Handler(ITEM_FN, handle_save));  // 1.90 - `::core::contracts::build_check_ensures`
            H::add("contract_check_requires", Handler(ITEM_FN, handle_save));  // 1.90 - `::core::intrinsics::contract_check_requires`
            H::add("contract_check_ensures" , Handler(ITEM_FN, handle_save));  // 1.90 - `::core::intrinsics::contract_check_ensures`
            H::add("use_cloned", Handler(ITEM_TRAIT, handle_save)); // 1.90 - `::core::clone::use_cloned` - for the `.use` syntax

            H::add("Ordering", Handler(ITEM_ENUM, handle_save));    // comparison ordering

            H::add("meta_sized", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::MetaSized
            H::add("pointee_sized", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::PointeeSized
            H::add("bikeshed_guaranteed_no_drop", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::BikeshedGuaranteedNoDrop
            H::add("unsafe_unpin", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::UnsafeUnpin
            H::add("unsized_const_param_ty", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::UnsizedConstParamTy
            H::add("coerce_pointee_validated", Handler(ITEM_TRAIT, handle_save));    // ::core::marker::CoercePointeeValidated

            H::add("async_fn", Handler(ITEM_TRAIT, handle_save));
            H::add("async_fn_mut", Handler(ITEM_TRAIT, handle_save));
            H::add("async_fn_once", Handler(ITEM_TRAIT, handle_save));

            H::add("async_fn_kind_helper", Handler(ITEM_TRAIT, handle_save));   // ::core::ops::async_function::internal_implementation_detail::AsyncFnKindHelper
            H::add("coroutine_state", Handler(ITEM_ENUM, handle_save)); // ::core::ops::coroutine::CoroutineState
            H::add("coroutine", Handler(ITEM_TRAIT, handle_save)); // ::core::ops::coroutine::Coroutine
            H::add("deref_pure", Handler(ITEM_TRAIT, handle_save)); // ::core::ops::deref::DerefPure
            H::add("legacy_receiver", Handler(ITEM_TRAIT, handle_save)); // ::core::ops::deref::LegacyReceiver

            H::add("type_id", Handler(ITEM_STRUCT, handle_save)); // ::core::any::TypeId

            H::add("async_iterator", Handler(ITEM_TRAIT, handle_save)); // ::core::async_iter::async_iter::AsyncIterator
            H::add("fused_iterator", Handler(ITEM_TRAIT, handle_save)); // ::core::iter::traits::marker::FusedIterator

            // Various panic handlers
            H::add("panic_const_add_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_sub_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_mul_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_div_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_rem_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_neg_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_shr_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_shl_overflow", Handler(ITEM_FN, handle_save));
            H::add("panic_const_div_by_zero", Handler(ITEM_FN, handle_save));
            H::add("panic_const_rem_by_zero", Handler(ITEM_FN, handle_save));
            H::add("panic_const_coroutine_resumed", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_fn_resumed", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_gen_fn_resumed", Handler(ITEM_FN, handle_save));
            H::add("panic_const_gen_fn_none", Handler(ITEM_FN, handle_save));
            H::add("panic_const_coroutine_resumed_panic", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_fn_resumed_panic", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_gen_fn_resumed_panic", Handler(ITEM_FN, handle_save));
            H::add("panic_const_gen_fn_none_panic", Handler(ITEM_FN, handle_save));

            H::add("panic_const_coroutine_resumed_drop", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_fn_resumed_drop", Handler(ITEM_FN, handle_save));
            H::add("panic_const_async_gen_fn_resumed_drop", Handler(ITEM_FN, handle_save));
            H::add("panic_const_gen_fn_none_drop", Handler(ITEM_FN, handle_save));

            H::add("panic_null_pointer_dereference", Handler(ITEM_FN, handle_save));
            H::add("panic_invalid_enum_construction", Handler(ITEM_FN, handle_save));

            H::add("unsafe_pinned", Handler(ITEM_STRUCT, handle_save)); // ::core::pin::unsafe_pinned::UnsafePinned

            H::add("RangeCopy", Handler(ITEM_STRUCT, handle_save)); // ::core::range::Range
            H::add("RangeInclusiveCopy", Handler(ITEM_STRUCT, handle_save)); // ::core::range::RangeInclusive
            H::add("RangeFromCopy", Handler(ITEM_STRUCT, handle_save)); // ::core::range::RangeFrom

            H::add("async_drop", Handler(ITEM_TRAIT, handle_save)); // ::core::future::async_drop::AsyncDrop
            H::add("async_drop_in_place", Handler(ITEM_FN, handle_save)); // ::core::future::async_drop::async_drop_in_place
        }
    }
    const char* real_name = nullptr;    // For when lang items have their name changed
    auto it = g_handlers.find(name.c_str());
    if( it != g_handlers.end() )
    {
        if(type != it->second.type) {
            ERROR(sp, E0000, "Language item '" << name << "' " << path << " - on incorrect item type " << type << " != " << it->second.type);
        }
        it->second.cb(sp, crate, name, path);
        return ;
    }

    // Structs
    else if( name == "unsafe_cell" ) { }
    else if( TARGETVER_LEAST_1_29 && name == "alloc_layout") { }
    else if( TARGETVER_LEAST_1_29 && name == "panic_info" ) {}    // Struct
    else if( TARGETVER_LEAST_1_54 && name == "panic_location" ) {}    // Struct
    else if( TARGETVER_LEAST_1_29 && name == "manually_drop" ) {}    // Struct

    else if( TARGETVER_LEAST_1_39 && name == "arc" ) {}    // Struct
    else if( TARGETVER_LEAST_1_39 && name == "rc" ) {}    // Struct

    else if( /*TARGETVER_1_39 &&*/ name == "maybe_uninit" ) {}    // Union

    // Futures
    else if( /*TARGETVER_1_39 &&*/ name == "unpin" ) {}    // Trait
    else if( /*TARGETVER_1_39 &&*/ name == "pin" ) {}    // Struct
    else if( /*TARGETVER_1_39 &&*/ name == "future_trait" ) {}    // Trait
    else if( TARGETVER_LEAST_1_54 && name == "from_generator" ) {}    // Function
    else if( TARGETVER_LEAST_1_54 && name == "get_context" ) {}    // Function

    // Variable argument lists
    else if( /*TARGETVER_1_39 &&*/ name == "va_list" ) {}    // Struct

    // Arbitary receivers
    else if( /*TARGETVER_1_39 &&*/ name == "receiver" ) {}    // Trait
    else if( /*TARGETVER_1_39 &&*/ name == "dispatch_from_dyn" ) {}    // Trait

    // Generators
    else if( TARGETVER_LEAST_1_29 && name == "generator" ) {}   // - Trait
    else if( TARGETVER_LEAST_1_29 && name == "generator_state" ) {}   // - State enum

    // Try
    else if( TARGETVER_LEAST_1_54 && name == "Try" ) { real_name = "try"; }

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
    else if( TARGETVER_LEAST_1_39 && name == "begin_panic" ) {}    // Function
    else if( TARGETVER_LEAST_1_54 && name == "panic_str") {}
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

    if( type == ITEM_EXTERN_FN )
    {
        // TODO: This should force a specific link name instead
        return ;
    }

    auto rv = crate.m_lang_items.insert( ::std::make_pair(real_name == nullptr ? name : real_name, path) );
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
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        auto v = attr.parse_equals_string(crate, mod);
        TU_MATCH_HDRA( (i), {)
        default:
            TODO(sp, "Unknown item type " << i.tag_str() << " with #["<<attr<<"] attached at " << path);
            break;
        TU_ARMA(None, e) {
            // NOTE: Can happen when #[cfg] removed this
            }
        TU_ARMA(Function, e) {
            if( e.code().is_valid() ) {
                handle_lang_item(sp, crate, path, v, ITEM_FN);
            }
            else {
                handle_lang_item(sp, crate, path, v, ITEM_EXTERN_FN);
            }
            }
        TU_ARMA(Type, e) {
            handle_lang_item(sp, crate, path, v, ITEM_TYPE_ALIAS);
            }
        TU_ARMA(Static, e) {
            handle_lang_item(sp, crate, path, v, ITEM_STATIC);
            }
        TU_ARMA(Struct, e) {
            handle_lang_item(sp, crate, path, v, ITEM_STRUCT);
            }
        TU_ARMA(Enum, e) {
            handle_lang_item(sp, crate, path, v, ITEM_ENUM);
            }
        TU_ARMA(Union, e) {
            handle_lang_item(sp, crate, path, v, ITEM_UNION);
            }
        TU_ARMA(Trait, e) {
            handle_lang_item(sp, crate, path, v, ITEM_TRAIT);
            }
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Trait ATYs (a sub-item of others)
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::EnumVariant& ev) const override {
        // TODO: Enum variants (sub-item of other lang items)
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item&i) const override {
        // TODO: lang items on associated items (e.g. functions - `RangeFull::new`)
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const override {
        ::std::string name = mi.parse_equals_string(crate, mod);

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
        else if( TARGETVER_LEAST_1_54 && name == "const_slice_ptr" ) {}
        else if( TARGETVER_LEAST_1_54 && name == "mut_slice_ptr" ) {}
        else if( TARGETVER_LEAST_1_54 && name == "array" ) {}
        else if( /*TARGETVER_1_39 &&*/ name == "bool" ) {}
        // rustc_unicode
        else if( name == "char" ) {}
        // collections
        else if( name == "str" ) {}
        else if( name == "slice" ) {}
        else if( TARGETVER_LEAST_1_29 && name == "slice_u8" ) {}  // libcore now, `impl [u8]`
        else if( TARGETVER_LEAST_1_29 && name == "slice_alloc" ) {}   // liballoc's impls on [T]
        else if( TARGETVER_LEAST_1_29 && name == "slice_u8_alloc" ) {}   // liballoc's impls on [u8]
        else if( TARGETVER_LEAST_1_29 && name == "str_alloc" ) {}   // liballoc's impls on str
        // std - interestingly
        else if( name == "f32" ) {}
        else if( name == "f64" ) {}
        else if( TARGETVER_LEAST_1_29 && name == "f32_runtime" ) {}
        else if( TARGETVER_LEAST_1_29 && name == "f64_runtime" ) {}
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
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if( i.is_None() ) {
            // Ignore.
        }
        else if( /*const auto* e =*/ i.opt_Function() ) {
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-main"), path ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[main] - " << other_path << " and " << path);
            }
        }
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
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if(i.is_None())
        {
        }
        else if(i.is_Function())
        {
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-start"), path ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[start] - " << other_path << " and " << path);
            }
        }
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
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if(i.is_Function())
        {
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-panic_implementation"), path ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[panic_implementation] - " << other_path << " and " << path);
            }
        }
        else {
            ERROR(sp, E0000, "#[panic_implementation] on non-function " << path);
        }
    }
};

class Decorator_PanicHandler:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if(i.is_Function())
        {
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-panic_implementation"), path ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[panic_handler] - " << other_path << " and " << path);
            }
        }
        else {
            ERROR(sp, E0000, "#[panic_handler] on non-function " << path);
        }
    }
};

class Decorator_RustcStdInternalSymbol:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        // Attribute that acts as like `#[no_mangle]` `#[linkage="external"]`
    }
};

class Decorator_AllocErrorHandler:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& attr, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i) const override
    {
        if(i.is_Function())
        {
            auto rv = crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-alloc_error_handler"), path ));
            if( !rv.second )
            {
                const auto& other_path = rv.first->second;
                ERROR(sp, E0000, "Duplicate definition of #[alloc_error_handler] - " << other_path << " and " << path);
            }
        }
    }
};

STATIC_DECORATOR("lang", Decorator_LangItem)
STATIC_DECORATOR("main", Decorator_Main);
STATIC_DECORATOR("start", Decorator_Start);
STATIC_DECORATOR("panic_implementation", Decorator_PanicImplementation);
STATIC_DECORATOR("panic_handler", Decorator_PanicHandler);
STATIC_DECORATOR("rustc_std_internal_symbol", Decorator_RustcStdInternalSymbol);
STATIC_DECORATOR("alloc_error_handler", Decorator_AllocErrorHandler);


