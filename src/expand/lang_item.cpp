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


void handle_lang_item(AST::Crate& crate, const AST::Path& path, const ::std::string& name, AST::eItemType type)
{
    if(name == "phantom_fn") {
        crate.m_lang_item_PhantomFn = AST::Path(path);
        crate.m_lang_item_PhantomFn.nodes().back().args() = { TypeRef("A"), TypeRef("B") };
    }
    else if( name == "send" ) {
        // Don't care, Send is fully library in mrustc
    }
    else if( name == "sync" ) {
        // Don't care, Sync is fully library in mrustc
    }
    else if( name == "sized" ) {
        DEBUG("Bind 'sized' to " << path);
    }
    else if( name == "copy" ) {
        DEBUG("Bind 'copy' to " << path);
    }
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
    else if( name == "ord" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "unsize" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    else if( name == "coerce_unsized" ) { DEBUG("Bind '"<<name<<"' to " << path); }
    
    else if( name == "iterator" ) { /* mrustc just desugars? */ }
    
    else if( name == "debug_trait" ) { /* TODO: Poke derive() with this */ }
    
    // Structs
    else if( name == "non_zero" ) { }
    else if( name == "phantom_data" ) { }
    else if( name == "range_full" ) { }
    else if( name == "range" ) { }
    else if( name == "range_from" ) { }
    else if( name == "range_to" ) { }
    else if( name == "unsafe_cell" ) { }
    
    // Functions
    else if( name == "panic" ) { }
    else if( name == "panic_bounds_check" ) { }
    else if( name == "panic_fmt" ) { }
    else if( name == "str_eq" ) { }
    
    else {
        throw CompileError::Generic(FMT("Unknown lang item '" << name << "'"));
    }

    
}

class Decorator_LangItem:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::EarlyPost; }
    void handle(const AST::MetaItem& attr, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TU_MATCH_DEF(::AST::Item, (i), (e),
        (
            TODO(Span(), "Unknown item type with #[lang] attached at " << path);
            ),
        (Function,
            handle_lang_item(crate, path, attr.string(), AST::ITEM_FN);
            ),
        (Struct,
            handle_lang_item(crate, path, attr.string(), AST::ITEM_STRUCT);
            ),
        (Trait,
            handle_lang_item(crate, path, attr.string(), AST::ITEM_TRAIT);
            )
        )
    }
};

STATIC_DECORATOR("lang", Decorator_LangItem)


