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

namespace {
    unsigned num_ch(const std::string& s) {
        uint8_t bits[128/8] = {0};
        for(char c : s) {
            if( 0 <= c && c < 128 ) {
                bits[c / 8] |= (1 << (c % 8));
            }
        }
        unsigned rv = 0;
        for(uint8_t v : bits) {
            for(int i = 0; i < 8; i ++) {
                if(v & (1 << i)) {
                    rv += 1;
                }
            }
        }
        return rv;
    }
    
    const char* tyname(const HIR::CoreType& t) {
        switch(t.raw_type)
        {
        case RawType::I8  : return "i8";
        case RawType::I16 : return "i16";
        case RawType::I32 : return "i32";
        case RawType::I64 : return "i64";
        case RawType::I128: return "i128";
        case RawType::U8  : return "u8";
        case RawType::U16 : return "u16";
        case RawType::U32 : return "u32";
        case RawType::U64 : return "u64";
        case RawType::U128: return "u128";
        
        case RawType::ISize: return "isize";
        case RawType::USize: return "usize";

        case RawType::F32 : return "f32";
        case RawType::F64 : return "f64";
        
        case RawType::Bool: return "bool";
        case RawType::Char: return "char";
        case RawType::Unit: return "?()";
        case RawType::Str : return "?str";

        case RawType::Unreachable: return "!";
        case RawType::Function: return "?fn";
        case RawType::Composite: return "?stuct";
        case RawType::TraitObject: return "?dyn";
        }
        return "?";
    }
}

namespace MIR {
    ::std::ostream& operator<<(::std::ostream& os, const Constant& v) {
        TU_MATCHA( (v), (e),
        (Int,
            os << (e.v < 0 ? "-" : "+");
            os << (e.v < 0 ? -e.v : e.v);
            os << " " << tyname(e.t);
            ),
        (Uint,
            std::ostringstream  ss_dec, ss_hex;
            ss_dec << e.v;
            ss_hex << std::hex << e.v;
            if( num_ch(ss_dec.str()) < num_ch(ss_hex.str()) ) {
                os << e.v;
            }
            else {
                os << std::hex << "0x" << e.v << std::dec;
            }
            os << " " << tyname(e.t);
            ),
        (Float,
            os << e.v;
            os << " " << tyname(e.t);
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
        (Function,
            os << "addr{fn} " << *e.p;
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

    ::Ordering Constant::ord(const Constant& b) const
    {
        if( this->tag() != b.tag() )
            return ::ord( static_cast<unsigned int>(this->tag()), static_cast<unsigned int>(b.tag()) );
        TU_MATCHA( (*this,b), (ae,be),
        (Int,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord(ae.t.raw_type, be.t.raw_type);
            ),
        (Uint,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord(ae.t.raw_type, be.t.raw_type);
            ),
        (Float,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord(ae.t.raw_type, be.t.raw_type);
            ),
        (Bool,
            return ::ord(ae.v, be.v);
            ),
        (Bytes,
            return ::ord(ae, be);
            ),
        (StaticString,
            return ::ord(ae, be);
            ),
        (Const,
            return ::ord(*ae.p, *be.p);
            ),
        (Function,
            return ::ord(*ae.p, *be.p);
            ),
        (Generic,
            //return ::ord(ae.binding, be.binding);
            return OrdEqual;
            ),
        (ItemAddr,
            ORD(static_cast<bool>(ae), static_cast<bool>(be));
            if(ae) ORD(*ae, *be);
            return OrdEqual;
            )
        )
        throw "";
    }
    bool Param::operator==(const Param& x) const
    {
        if( this->tag() != x.tag() )
            return false;
        TU_MATCHA( (*this, x), (ea, eb),
        (LValue,
            return ea == eb;
            ),
        (Borrow,
            return ea.type == eb.type && ea.val == eb.val;
            ),
        (Constant,
            return ea == eb;
            )
        )
        throw "";
    }

    bool operator==(const RValue& a, const RValue& b)
    {
        if( a.tag() != b.tag() )
            return false;
        TU_MATCHA( (a, b), (are, bre),
        (Use,
            return are == bre;
            ),
        (Constant,
            return are == bre;
            ),
        (SizedArray,
            if( are.val != bre.val )
                return false;
            if( are.count != bre.count )
                return false;
            return true;
            ),
        (Borrow,
            if( are.type != bre.type )
                return false;
            if( are.val != bre.val )
                return false;
            return true;
            ),
        (Cast,
            if( are.type != bre.type )
                return false;
            if( are.val != bre.val )
                return false;
            return true;
            ),
        (BinOp,
            if( are.val_l != bre.val_l )
                return false;
            if( are.op != bre.op )
                return false;
            if( are.val_r != bre.val_r )
                return false;
            return true;
            ),
        (UniOp,
            if( are.op != bre.op )
                return false;
            if( are.val != bre.val )
                return false;
            return true;
            ),
        (DstPtr,
            return are.val == bre.val;
            ),
        (DstMeta,
            return are.val == bre.val;
            ),
        (MakeDst,
            if( are.meta_val != bre.meta_val )
                return false;
            if( are.ptr_val != bre.ptr_val )
                return false;
            return true;
            ),
        (Tuple,
            return are.vals == bre.vals;
            ),
        (Array,
            return are.vals == bre.vals;
            ),
        (UnionVariant,
            if( are.path != bre.path )
                return false;
            if( are.index != bre.index )
                return false;
            return are.val == bre.val;
            ),
        (EnumVariant,
            if( are.path != bre.path )
                return false;
            if( are.index != bre.index )
                return false;
            return are.vals == bre.vals;
            ),
        (Struct,
            if( are.path != bre.path )
                return false;
            return are.vals == bre.vals;
            )
        )
        throw "";
    }
    bool SwitchValues::operator==(const SwitchValues& x) const
    {
        if( this->tag() != x.tag() )
            return false;
        TU_MATCHA( ((*this), x), (ave, bve),
        (Unsigned,
            if( ave != bve )
                return false;
            ),
        (Signed,
            if( ave != bve )
                return false;
            ),
        (String,
            if( ave != bve )
                return false;
            ),
        (ByteString,
            if( ave != bve )
                return false;
            )
        )
        return true;
    }
    bool operator==(const AsmParam& a, const AsmParam& b)
    {
        if(a.tag() != b.tag())
            return false;
        TU_MATCH_HDRA( (a,b), {)
        TU_ARMA(Const, ae, be) {
            return ae == be;
            }
        TU_ARMA(Sym, ae, be) {
            return ae == be;
            }
        TU_ARMA(Reg, ae, be) {
            if(ae.dir != be.dir) return false;
            if(ae.spec != be.spec)  return false;
            if( !!ae.input != !!be.input )  return false;
            if( ae.input && *ae.input != *be.input )    return false;
            if( !!ae.output != !!be.output )  return false;
            if( ae.output && *ae.output != *be.output ) return false;
            }
        }
        return true;
    }
    bool operator==(const Statement& a, const Statement& b) {
        if( a.tag() != b.tag() )
            return false;
        
        TU_MATCH_HDRA( (a,b), {)
        TU_ARMA(Assign, ae,be) {
            return ae.dst == be.dst && ae.src == be.src;
            }
        TU_ARMA(Asm, ae,be) {
            return ae.outputs == be.outputs
                && ae.inputs == be.inputs
                && ae.clobbers == be.clobbers
                && ae.flags == be.flags
                ;
            }
        TU_ARMA(Asm2, ae,be) {
            return ae.lines == be.lines
                && ae.options == be.options
                && ae.params == be.params
                ;
            }
        TU_ARMA(SetDropFlag, ae,be) {
            return ae.idx == be.idx
                && ae.other == be.other
                && ae.new_val == be.new_val
                ;
            }
        TU_ARMA(Drop, ae,be) {
            return ae.slot == be.slot
                && ae.kind == be.kind
                && ae.flag_idx == be.flag_idx
                ;
            }
        TU_ARMA(ScopeEnd, ae,be) {
            return ae.slots == be.slots;
            }
        }
        throw "";
    }
    bool operator==(const Terminator& a, const Terminator& b) {
        if( a.tag() != b.tag() )
            return false;
        TU_MATCHA( (a,b), (ae,be),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            if( ae != be )
                return false;
            ),
        (Panic,
            if( ae.dst != be.dst )
                return false;
            ),
        (If,
            if( ae.cond != be.cond )
                return false;
            if( ae.bb_true != be.bb_true )
                return false;
            if( ae.bb_false != be.bb_false )
                return false;
            ),
        (Switch,
            if( ae.val != be.val )
                return false;
            if( ae.targets != be.targets )
                return false;
            ),
        (SwitchValue,
            if( ae.val != be.val )
                return false;
            if( ae.targets != be.targets )
                return false;
            if( ae.values != be.values)
                return false;
            ),
        (Call,
            if( ae.ret_val != be.ret_val )
                return false;
            TU_MATCHA( (ae.fcn, be.fcn), (afe, bfe),
            (Value,
                if( afe != bfe )
                    return false;
                ),
            (Path,
                if( afe != bfe )
                    return false;
                ),
            (Intrinsic,
                if( afe.name != bfe.name )
                    return false;
                if( afe.params != bfe.params )
                    return false;
                )
            )
            if( ae.args != be.args )
                return false;
            if( ae.ret_block != be.ret_block )
                return false;
            if( ae.panic_block != be.panic_block )
                return false;
            )
        )
        return true;
    }
}

