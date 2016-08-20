/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir.cpp
 * - MIR (Middle Intermediate Representation) definitions
 */
#include <mir/mir.hpp>

namespace MIR {
    ::std::ostream& operator<<(::std::ostream& os, const Constant& v) {
        TU_MATCHA( (v), (e),
        (Int,
            os << (e < 0 ? "-" : "+");
            os << (e < 0 ? -e : e);
            ),
        (Uint,
            os << e;
            ),
        (Float,
            os << e;
            ),
        (Bool,
            os << (e ? "true" : "false");
            ),
        (Bytes,
            os << "[" << e << "]";
            ),
        (StaticString,
            os << "\"" << e << "\"";
            ),
        (ItemAddr,
            os << "&" << e;
            )
        )
        return os;
    }
    
    ::std::ostream& operator<<(::std::ostream& os, const LValue& x)
    {
        return os;
    }
    
    ::std::ostream& operator<<(::std::ostream& os, const Terminator& x)
    {
        TU_MATCHA( (x), (e),
        (Incomplete,
            os << "Invalid";
            ),
        (Return,
            os << "Return";
            ),
        (Diverge,
            os << "Diverge";
            ),
        (Goto,
            os << "Goto(" << e << ")";
            ),
        (Panic,
            os << "Panic(" << e.dst << ";)";
            ),
        (If,
            os << "If( ? : " << e.bb0 << ", " << e.bb1 << ")";
            ),
        (Switch,
            os << "Switch( ? : ";
            for(unsigned int j = 0; j < e.targets.size(); j ++)
                os << j << " => bb" << e.targets[j] << ", ";
            os << ")";
            ),
        (Call,
            os << "Call( ? = ?( ";
            for(const auto& arg : e.args)
                os << arg << ", ";
            os << "), bb" << e.ret_block << ", bb" << e.panic_block << ")";
            )
        )

        return os;
    }
}

::MIR::LValue MIR::LValue::clone() const
{
    TU_MATCHA( (*this), (e),
    (Variable, return LValue(e); ),
    (Temporary, return LValue(e); ),
    (Argument, return LValue(e); ),
    (Static, return LValue(e.clone()); ),
    (Return, return LValue(e); ),
    (Field, return LValue::make_Field({
        box$( e.val->clone() ),
        e.field_index
        }); ),
    (Deref, return LValue::make_Deref({
        box$( e.val->clone() )
        }); ),
    (Index, return LValue::make_Index({
        box$( e.val->clone() ),
        box$( e.idx->clone() )
        }); ),
    (Downcast, return LValue::make_Downcast({
        box$( e.val->clone() ),
        e.variant_index
        }); )
    )
    throw "";
}


