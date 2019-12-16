/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * ffi.cpp
 * - FFI wrappers
 */
#include <cstdint>

/// Argument reference (for checking validity)
struct ArgRef
{
    uint8_t idx;    // if 255, it's not referencing anything

    static ArgRef null() { return ArgRef { 255 }; }
};

/// Representation of a FFI type (defined by the .api file)
/// - These specify various flags used to tag pointers in the MIR
struct FfiType
{
    // Pointer:
    // - Mutability
    // - Nullability
    // - Array size (number of allocated elements)
    //   > Can be either a number, or an argument name
    // - Valid size (number of initialised elements)
    // - Allocation source
    struct Pointer {
        bool    is_mut;
        bool    is_nullable;
        ArgOrCount  alloc_size;
        ArgOrCount  valid_size;
        ArgRef  alloc_source;   // Can be "null"
    };
    ::std::vector<Pointer>  pointers;   // Reverse list (last entry is the outermost pointer)

    // Datatypes:
    // - `void`
    //   - size/alignment
    // - u8,u16,...
    // - function
    //   - Name
    enum class Datatype {
        Void,
        Signed,
        Unsigned,
        Float,
        Function,
    } datatype;
    union Meta {
        struct {
            size_t  size;
            size_t  align;
            ::std::string   tag;
        } void_data;
        unsigned bits;
        struct {
            ArgRef  name_source;
        } function;
    } meta;
};


struct FfiShim
{
    class ValueRef
    {
    public:
        static ValueRef new_global(std::string name);
        static ValueRef new_local(std::string name);
        static ValueRef new_deref(ValueRef target);
    };
    class Expr
    {
        enum {
            LITERAL,
            VALUE,
            CALL,
        } ty;
        union {
        } data;
    public:
        static Expr new_lit(uint64_t v);
        static Expr new_value(ValueRef vr);
        static Expr new_call_int(::std::vector<::std::string> path, ::std::vector<Expr> args);
    };
    struct Stmt;
    struct Block
    {
        ::std::vector<Stmt> statements;
        Expr    val;
    };
    class Stmt
    {
        enum {
            DEFINE, // `let foo = bar;`
            ASSIGN, // `foo ?= bar;`
            IF,
        } ty;
        union {
            struct {
                ::std::string   slot;
                Expr    value;
            } define;
            struct {
                ValueRef    slot;
                Expr    value;
            } assign;
            struct {
                Expr    cond;
                Block   true_arm;
                Block   false_arm;
            } if_block;
        };
    };
};

struct FfiFunction
{
    ::std::vector<FfiType>  arg_types;
    FfiType ret_type;
    ::std::vector<std::string>  arg_names;

    // Either directly defers to a function
    ::std::string   library;
    ::std::string   function;

    // Or, it has code for more advanced checking
    //FfiShimExpr   code;

    bool call(Value& rv, ::std::vector<Value> args) const;
};

bool FfiFunction::call(Value& rv, ::std::vector<Value> args) const
{

}

bool call_ffi(Value& rv, const ::std::string& link_name, const ::std::string& abi, ::std::vector<Value> args)
{
}
