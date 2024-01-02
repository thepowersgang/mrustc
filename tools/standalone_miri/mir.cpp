/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir.cpp
 * - MIR (Middle Intermediate Representation) definitions
 */
#include "../../src/include/rc_string.hpp"
#include "../../src/mir/mir.hpp"
#include "hir_sim.hpp"
#include <iostream>
#include <algorithm>    // std::min

#if 0
namespace std {
    template <typename T>
    inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v) {
        if( v.size() > 0 )
        {
            bool is_first = true;
            for( const auto& i : v )
            {
                if(!is_first)
                    os << ", ";
                is_first = false;
                os << i;
            }
        }
        return os;
    }
}
#endif

namespace MIR {
    ::std::ostream& operator<<(::std::ostream& os, const Constant& v) {
        TU_MATCHA( (v), (e),
        (Int,
            os << (e.v < 0 ? "-" : "+");
            os << (e.v < 0 ? -e.v : e.v);
            ),
        (Uint,
            os << e.v;
            ),
        (Float,
            os << e.v;
            ),
        (Bool,
            os << (e.v ? "true" : "false");
            ),
        (Bytes,
            os << "b\"";
            os << ::std::hex;
            for(auto v : e)
            {
                if( v == '\\' || v == '"' )
                    os << "\\" << v;
                else if( ' ' <= v && v < 0x7F )
                    os << v;
                else if( v < 16 )
                    os << "\\x0" << (unsigned int)v;
                else
                    os << "\\x" << ((unsigned int)v & 0xFF);
            }
            os << "\"";
            os << ::std::dec;
            ),
        (StaticString,
            os << "\"" << FmtEscaped(e) << "\"";
            ),
        (Const,
            os << *e.p;
            ),
        (Generic,
            os << e;
            ),
        (ItemAddr,
            os << "addr " << *e;
            )
        )
        return os;
    }
    void LValue::RefCommon::fmt(::std::ostream& os) const
    {
        TU_MATCHA( (m_lv->m_root), (e),
        (Return,
            os << "retval";
            ),
        (Argument,
            os << "a" << e;
            ),
        (Local,
            os << "_" << e;
            ),
        (Static,
            os << "(" << e << ")";
            )
        )
        for(size_t i = 0; i < m_wrapper_count; i ++)
        {
            const LValue::Wrapper& w = m_lv->m_wrappers.at(i);
            TU_MATCHA( (w), (e),
            (Field,
                os << "." << e;
                ),
            (Deref,
                os << "*";
                ),
            (Index,
                os << "[_" << e << "]";
                ),
            (Downcast,
                os << "#" << e;
                )
            )
        }
    }

    ::std::ostream& operator<<(::std::ostream& os, const LValue& x)
    {
        LValue::CRef(x).fmt(os);
        return os;
    }

    Ordering LValue::Storage::ord(const LValue::Storage& x) const
    {
        if( x.is_Static() )
        {
            if( this->is_Static() )
                return this->as_Static().ord( x.as_Static() );
            else
                return OrdLess;
        }
        else
        {
            if( this->is_Static() )
                return OrdGreater;
        }

        return ::ord(this->val, x.val);
    }
    Ordering LValue::ord(const LValue& x) const
    {
        auto rv = m_root.ord(x.m_root);
        if( rv != OrdEqual )
            return rv;
        return ::ord(m_wrappers, x.m_wrappers);
    }
    Ordering LValue::RefCommon::ord(const LValue::RefCommon& x) const
    {
        Ordering rv;
        //TRACE_FUNCTION_FR(FMT_CB(ss, this->fmt(ss); ss << " ? "; x.fmt(ss);), rv);
        rv = m_lv->m_root.ord(x.m_lv->m_root);
        if( rv != OrdEqual )
            return rv;
        for(size_t i = 0; i < ::std::min(m_wrapper_count, x.m_wrapper_count); i ++)
        {
            rv = m_lv->m_wrappers[i].ord(x.m_lv->m_wrappers[i]);
            if( rv != OrdEqual )
                return rv;
        }
        return (rv = ::ord(m_wrapper_count, x.m_wrapper_count));
    }
    ::std::ostream& operator<<(::std::ostream& os, const Param& x)
    {
        TU_MATCHA( (x), (e),
        (LValue,
            os << e;
            ),
        (Borrow,
            os << "Borrow(" << e.type << ", " << e.val << ")";
            ),
        (Constant,
            os << e;
            )
        )
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const RValue& x)
    {
        TU_MATCHA( (x), (e),
        (Use,
            os << "Use(" << e << ")";
            ),
        (Constant,
            os << "Constant(" << e << ")";
            ),
        (SizedArray,
            os << "SizedArray(" << e.val << "; " << e.count << ")";
            ),
        (Borrow,
            os << "Borrow(" << e.type << ", " << e.val << ")";
            ),
        (Cast,
            os << "Cast(" << e.val << " as " << e.type << ")";
            ),
        (BinOp,
            os << "BinOp(" << e.val_l << " ";
            switch(e.op)
            {
            case ::MIR::eBinOp::ADD:    os << "ADD";    break;
            case ::MIR::eBinOp::SUB:    os << "SUB";    break;
            case ::MIR::eBinOp::MUL:    os << "MUL";    break;
            case ::MIR::eBinOp::DIV:    os << "DIV";    break;
            case ::MIR::eBinOp::MOD:    os << "MOD";    break;
            case ::MIR::eBinOp::ADD_OV: os << "ADD_OV"; break;
            case ::MIR::eBinOp::SUB_OV: os << "SUB_OV"; break;
            case ::MIR::eBinOp::MUL_OV: os << "MUL_OV"; break;
            case ::MIR::eBinOp::DIV_OV: os << "DIV_OV"; break;

            case ::MIR::eBinOp::BIT_OR : os << "BIT_OR" ; break;
            case ::MIR::eBinOp::BIT_AND: os << "BIT_AND"; break;
            case ::MIR::eBinOp::BIT_XOR: os << "BIT_XOR"; break;
            case ::MIR::eBinOp::BIT_SHL: os << "BIT_SHL"; break;
            case ::MIR::eBinOp::BIT_SHR: os << "BIT_SHR"; break;

            case ::MIR::eBinOp::EQ: os << "EQ"; break;
            case ::MIR::eBinOp::NE: os << "NE"; break;
            case ::MIR::eBinOp::GT: os << "GT"; break;
            case ::MIR::eBinOp::GE: os << "GE"; break;
            case ::MIR::eBinOp::LT: os << "LT"; break;
            case ::MIR::eBinOp::LE: os << "LE"; break;
            }
            os << " " << e.val_r << ")";
            ),
        (UniOp,
            os << "UniOp(" << e.val << " " << static_cast<int>(e.op) << ")";
            ),
        (DstMeta,
            os << "DstMeta(" << e.val << ")";
            ),
        (DstPtr,
            os << "DstPtr(" << e.val << ")";
            ),
        (MakeDst,
            os << "MakeDst(" << e.ptr_val << ", " << e.meta_val << ")";
            ),
        (Tuple,
            os << "Tuple(" << e.vals << ")";
            ),
        (Array,
            os << "Array(" << e.vals << ")";
            ),
        (UnionVariant,
            os << "Variant(" << e.path << " #" << e.index << ", " << e.val << ")";
            ),
        (EnumVariant,
            os << "Variant(" << e.path << " #" << e.index << ", {" << e.vals << "})";
            ),
        (Struct,
            os << "Struct(" << e.path << ", {" << e.vals << "})";
            )
        )
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
            os << "Panic(" << e.dst << ")";
            ),
        (If,
            os << "If( " << e.cond << " goto " << e.bb_true << " else " << e.bb_false << ")";
            ),
        (Switch,
            os << "Switch( " << e.val << " : ";
            for(unsigned int j = 0; j < e.targets.size(); j ++)
                os << j << " => bb" << e.targets[j] << ", ";
            os << ")";
            ),
        (SwitchValue,
            os << "SwitchValue( " << e.val << " : ";
            TU_MATCHA( (e.values), (ve),
            (Unsigned,
                for(unsigned int j = 0; j < e.targets.size(); j ++)
                    os << ve[j] << " => bb" << e.targets[j] << ", ";
                ),
            (Signed,
                for(unsigned int j = 0; j < e.targets.size(); j ++)
                    os << (ve[j] >= 0 ? "+" : "") << ve[j] << " => bb" << e.targets[j] << ", ";
                ),
            (String,
                for(unsigned int j = 0; j < e.targets.size(); j ++)
                    os << "\"" << FmtEscaped(ve[j]) << "\" => bb" << e.targets[j] << ", ";
                ),
            (ByteString,
                for(unsigned int j = 0; j < e.targets.size(); j ++)
                    os << "[" << ve[j] << "] => bb" << e.targets[j] << ", ";
                )
            )
            os << "else bb" << e.def_target << ")";
            ),
        (Call,
            os << "Call( " << e.ret_val << " = ";
            TU_MATCHA( (e.fcn), (e2),
            (Value,
                os << "(" << e2 << ")";
                ),
            (Path,
                os << e2;
                ),
            (Intrinsic,
                os << "\"" << e2.name << "\"::" << e2.params;
                )
            )
            os << "( ";
            for(const auto& arg : e.args)
                os << arg << ", ";
            os << "), bb" << e.ret_block << ", bb" << e.panic_block << ")";
            )
        )

        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const Statement& x)
    {
        TU_MATCHA( (x), (e),
        (Assign,
            os << e.dst << " = " << e.src;
            ),
        (Asm,
            os << "(";
            for(const auto& spec : e.outputs)
                os << "\"" << spec.first << "\" : " << spec.second << ", ";
            os << ") = asm!(\"\", input=( ";
            for(const auto& spec : e.inputs)
                os << "\"" << spec.first << "\" : " << spec.second << ", ";
            os << "), clobbers=[" << e.clobbers << "], flags=[" << e.flags << "])";
            ),
        (Asm2,
            os << "asm!(";
            for(const auto& l : e.lines) {
                if(&l != &e.lines.front())
                    os << " ";
                l.fmt(os);
            }
            for(const auto& p : e.params)
            {
                os << ", ";
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(Const, v) {
                    os << "const " << v;
                    }
                TU_ARMA(Sym, v) {
                    os << "sym " << v;
                    }
                TU_ARMA(Reg, v) {
                    os << "reg " << v.dir << " " << v.spec << " ";
                    if(v.input)
                        os << *v.input;
                    else
                        os << "_";
                    os << " => ";
                    if(v.output)
                        os << *v.output;
                    else
                        os << "_";
                    }
                }
            }
            if(e.options.any()) {
                os << ", ";
                e.options.fmt(os);
            }
            os << ")";
            ),
        (SetDropFlag,
            os << "df$" << e.idx << " = ";
            if( e.other == ~0u )
            {
                os << e.new_val;
            }
            else
            {
                os << (e.new_val ? "!" : "") << "df$" << e.other;
            }
            ),
        (Drop,
            os << "drop(" << e.slot;
            if(e.kind == ::MIR::eDropKind::SHALLOW)
                os << " SHALLOW";
            if(e.flag_idx != ~0u)
                os << " IF df$" << e.flag_idx;
            os << ")";
            ),
        (ScopeEnd,
            os << "ScopeEnd(";
            for(auto idx : e.slots)
                os << "_$" << idx << ",";
            os << ")";
            )
        )
        return os;
    }

    EnumCachePtr::~EnumCachePtr()
    {
        assert(!this->p);
    }
}

