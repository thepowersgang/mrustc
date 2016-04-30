/*
 * Convert all paths in AST into absolute form (or to the relevant local item)
 * - NOTE: This is the core of the 'resolve' pass.
 *
 * After complete there should be no:
 * - Relative/super/self paths
 * - MaybeBind patterns
 */
#include <ast/crate.hpp>
#include <ast/ast.hpp>
#include <main_bindings.hpp>

struct GenericSlot
{
    enum class Level
    {
        Impl,
        Function,
    } level;
    unsigned short  index;
};
template<typename Val>
struct Named
{
    ::std::string   name;
    Val value;
};

struct Context
{
    TAGGED_UNION(Ent, Module,
    (Module, struct {
        const ::AST::Module* mod;
        }),
    (VarBlock, struct {
        // "Map" of names to function-level variable slots
        ::std::vector< Named< unsigned int > > variables;
        }),
    (Generic, struct {
        // Map of names to slots
        ::std::vector< Named< GenericSlot > > types;
        ::std::vector< Named< GenericSlot > > constants;
        ::std::vector< Named< GenericSlot > > lifetimes;
        })
    );

    const ::AST::Module&    m_mod;
    ::std::vector<Ent>  m_name_context;
    
    Context(const::AST::Module& mod):
        m_mod(mod)
    {}
};

void Resolve_Absolute_Type(const ::AST::Crate& crate, const Context& context,  TypeRef& type)
{
    TU_MATCH(TypeData, (type.m_data), (e),
    (None,
        // ! type
        ),
    (Any,
        // _ type
        ),
    (Unit,
        ),
    (Macro,
        BUG(Span(), "Resolve_Absolute_Type - Encountered an unexpanded macro");
        ),
    (Primitive,
        ),
    (Function,
        TODO(Span(), "Resolve_Absolute_Type - Function - " << type);
        ),
    (Tuple,
        for(auto& t : e.inner_types)
            Resolve_Absolute_Type(crate, context,  t);
        ),
    (Borrow,
        Resolve_Absolute_Type(crate, context,  *e.inner);
        ),
    (Pointer,
        Resolve_Absolute_Type(crate, context,  *e.inner);
        ),
    (Array,
        Resolve_Absolute_Type(crate, context,  *e.inner);
        ),
    (Generic,
        TODO(Span(), "Resolve_Absolute_Type - Encountered generic");
        ),
    (Path,
        TODO(Span(), "Resolve_Absolute_Type - Path");
        ),
    (TraitObject,
        TODO(Span(), "Resolve_Absolute_Type - TraitObject");
        )
    )
}

void Resolve_Absolute_Expr(const ::AST::Crate& crate, const Context& context,  ::AST::Expr& expr)
{
    if( expr.is_valid() )
    {
        TODO(Span(), "Resolve_Absolute_Expr");
    }
}

void Resolve_Absolute_Mod(const ::AST::Crate& crate, ::AST::Module& mod)
{
    for( auto& i : mod.items() )
    {
        TU_MATCH(AST::Item, (i.data), (e),
        (None,
            ),
        (Module,
            Resolve_Absolute_Mod(crate, e);
            ),
        (Crate,
            // - Nothing
            ),
        (Enum,
            TODO(Span(), "Resolve_Absolute_Mod - Enum");
            ),
        (Trait,
            TODO(Span(), "Resolve_Absolute_Mod - Trait");
            ),
        (Type,
            TODO(Span(), "Resolve_Absolute_Mod - Type");
            ),
        (Struct,
            TODO(Span(), "Resolve_Absolute_Mod - Struct");
            ),
        (Function,
            TODO(Span(), "Resolve_Absolute_Mod - Function");
            ),
        (Static,
            Resolve_Absolute_Type( crate, Context(mod), e.type() );
            Resolve_Absolute_Expr( crate, Context(mod), e.value() );
            )
        )
    }
}

void Resolve_Absolutise(AST::Crate& crate)
{
    Resolve_Absolute_Mod(crate, crate.root_module());
}


