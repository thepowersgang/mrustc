/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* hir/literal.hpp
* - HIR (partially) evaluated literal
*/
#pragma once
#include <tagged_union.hpp>
#include "generic_ref.hpp"

namespace HIR {


//enum class LiteralExprOp
//{
//    // Two arguments: left, right
//    Add, Sub, Div, Mul, Mod,
//    // One argument: the value
//    Neg, Not,
//    // Takes a list of 1+ values (first is the function name, rest are arguments)
//    Call,
//    // First argument is the binding index, second is the name
//    ConstGeneric,
//};

/// Literal type used for constant evaluation
/// NOTE: Intentionally minimal, just covers the values (not the types)
TAGGED_UNION_EX(Literal, (), Invalid, (
    (Invalid, struct {}),
    // Defer - The value isn't yet known (needs to be evaluated later)
    (Defer, struct {}),
    // Constant generic
    (Generic, GenericRef),
    //(Expr, struct {
    //    LiteralExprOp   op;
    //    std::vector<Literal> args;
    //    }),
    // List = Array, Tuple, struct literal
    (List, ::std::vector<Literal>), // TODO: Have a variant for repetition lists
    // Variant = Enum variant
    (Variant, struct {
        unsigned int    idx;
        ::std::unique_ptr<Literal> val;
        }),
    // Literal values
    (Integer, uint64_t),
    (Float, double),
    // Borrow of a path (existing item)
    (BorrowPath, ::HIR::Path),
    // Borrow of inline data
    (BorrowData, struct {
        ::std::unique_ptr<Literal> val;
        HIR::TypeRef    ty;
        }),
    // String = &'static str or &[u8; N]
    (String, ::std::string)
    ),
    /*extra_move=*/(),
    /*extra_assign=*/(),
    /*extra=*/(
        static Literal new_invalid() { return Literal::make_Invalid({}); }
        static Literal new_defer() { return Literal::make_Defer({}); }
        static Literal new_generic(HIR::GenericRef g) { return Literal::make_Generic(std::move(g)); }
        static Literal new_list(::std::vector<Literal> l) { return Literal::make_List(std::move(l)); }
        static Literal new_variant(unsigned idx, Literal inner) { return Literal::make_Variant({ idx, box$(inner) }); }
        static Literal new_integer(uint64_t v) { return Literal::make_Integer(v); }
        static Literal new_integer(double v) { return Literal::make_Float(v); }
        static Literal new_borrow_path(::HIR::Path p) { return Literal::make_BorrowPath(mv$(p)); }
        static Literal new_borrow_data(TypeRef ty, Literal inner) { return Literal::make_BorrowData({ box$(inner), mv$(ty) }); }
        static Literal new_string(std::string v) { return Literal::make_String(mv$(v)); }

        Literal clone() const;
        )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Literal& v);
extern bool operator==(const Literal& l, const Literal& r);
static inline bool operator!=(const Literal& l, const Literal& r) { return !(l == r); }

}
