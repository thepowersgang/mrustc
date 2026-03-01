/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir.cpp
 * - MIR (Middle Intermediate Representation) definitions
 */
#include <mir/mir.hpp>
#include <algorithm>    // std::min
#include <hir_typeck/monomorph.hpp>
#include <hir/encoded_literal.hpp>
#include <trans/target.hpp> // Target_GetPointerBits

namespace MIR {
    ::std::ostream& operator<<(::std::ostream& os, const Constant& v) {
        TU_MATCHA( (v), (e),
        (Int,
            os << (e.v < 0 ? "-" : "+");
            os << (e.v < 0 ? -e.v : e.v);
            os << " " << e.t;
            ),
        (Uint,
            os << std::hex << "0x" << e.v << std::dec;
            os << " " << e.t;
            ),
        (Float,
            os << std::hexfloat << e.v << std::defaultfloat;
            os << " " << e.t;
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
            assert(e.p);
            os << *e.p;
            ),
        (Generic,
            os << e;
            ),
        (Function,
            assert(e.p);
            os << "fn " << *e.p;
            ),
        (ItemAddr,
            if(e) {
                os << "&" << *e;
            }
            else {
                os << "#UNSIZE_PLACEHOLDER";    // A `Const` with `nullptr` is a placeholder for MakeDst `Unsize`
            }
            )
        )
        return os;
    }
    ::Ordering Constant::ord(const Constant& b) const
    {
        if( this->tag() != b.tag() )
            return ::ord( static_cast<unsigned int>(this->tag()), static_cast<unsigned int>(b.tag()) );
        TU_MATCHA( (*this,b), (ae,be),
        (Int,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord((unsigned)ae.t, (unsigned)be.t);
            ),
        (Uint,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord((unsigned)ae.t, (unsigned)be.t);
            ),
        (Float,
            if( ae.v != be.v )
                return ::ord(ae.v, be.v);
            return ::ord((unsigned)ae.t, (unsigned)be.t);
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
        (Generic,
            return ::ord(ae.binding, be.binding);
            ),
        (Function,
            return ::ord(*ae.p, *be.p);
            ),
        (ItemAddr,
            ORD(static_cast<bool>(ae), static_cast<bool>(be));
            if(ae) ORD(*ae, *be);
            return OrdEqual;
            )
        )
        throw "";
    }

    void LValue::RefCommon::fmt(::std::ostream& os) const
    {
        os << m_lv->m_root;
        for(size_t i = 0; i < m_wrapper_count; i ++)
        {
            os << m_lv->m_wrappers.at(i);
        }
    }

    ::std::ostream& operator<<(::std::ostream& os, const LValue& x)
    {
        LValue::CRef(x).fmt(os);
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const LValue::Storage& r)
    {
        TU_MATCHA( (r), (e),
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
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const LValue::Wrapper& w)
    {
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
            os << "UniOp(" << e.val << " ";
            switch(e.op)
            {
            case ::MIR::eUniOp::INV:    os << "INV";    break;
            case ::MIR::eUniOp::NEG:    os << "NEG";    break;
            }
            os << ")";
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
            os << "UnionVariant(" << e.path << " #" << e.index << ", " << e.val << ")";
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
            os << "If( " << e.cond << " : " << e.bb_true << ", " << e.bb_false << ")";
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
                    os << "\"" << ve[j] << "\" => bb" << e.targets[j] << ", ";
                ),
            (ByteString,
                for(unsigned int j = 0; j < e.targets.size(); j ++)
                    os << "b\"" << ve[j] << "\" => bb" << e.targets[j] << ", ";
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
    ::std::ostream& operator<<(::std::ostream& os, const Statement& x)
    {
        TU_MATCH_HDRA( (x), {)
        TU_ARMA(Assign, e) {
            os << e.dst << " = " << e.src;
            }
        TU_ARMA(Asm, e) {
            os << "(";
            for(const auto& spec : e.outputs)
                os << "\"" << spec.first << "\" : " << spec.second << ", ";
            os << ") = llvm_asm!(\"" << FmtEscaped(e.tpl) << "\", input=( ";
            for(const auto& spec : e.inputs)
                os << "\"" << spec.first << "\" : " << spec.second << ", ";
            os << "), clobbers=[" << e.clobbers << "], flags=[" << e.flags << "])";
            }
        TU_ARMA(Asm2, e) {
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
            }
        TU_ARMA(SetDropFlag, e) {
            os << "df$" << e.idx << " = ";
            if( e.other == ~0u )
            {
                os << e.new_val;
            }
            else
            {
                os << (e.new_val ? "!" : "") << "df$" << e.other;
            }
            }
        TU_ARMA(SaveDropFlag, e) {
            os << "SaveDropFlag()";
            }
        TU_ARMA(LoadDropFlag, e) {
            os << "LoadDropFlag()";
            }
        TU_ARMA(Drop, e) {
            os << "drop(" << e.slot;
            if(e.kind == ::MIR::eDropKind::SHALLOW)
                os << " SHALLOW";
            if(e.flag_idx != ~0u)
                os << " IF df$" << e.flag_idx;
            os << ")";
            }
        TU_ARMA(ScopeEnd, e) {
            os << "ScopeEnd(";
            for(auto idx : e.slots)
                os << "_$" << idx << ",";
            os << ")";
            }
        }
        return os;
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
        TU_ARMA(SaveDropFlag, ae,be) {
            return ae.idx == be.idx
                && ae.slot == be.slot
                && ae.bit_index == be.bit_index
                ;
            }
        TU_ARMA(LoadDropFlag, ae,be) {
            return ae.idx == be.idx
                && ae.slot == be.slot
                && ae.bit_index == be.bit_index
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
}

::MIR::LValue::Storage MIR::LValue::Storage::clone() const
{
    if( is_Static() ) {
        return new_Static(as_Static().clone());
    }
    else {
        return Storage(this->val);
    }
}
::MIR::Constant MIR::Constant::clone() const
{
    TU_MATCHA( (*this), (e2),
    (Int, return ::MIR::Constant(e2); ),
    (Uint, return ::MIR::Constant(e2); ),
    (Float, return ::MIR::Constant(e2); ),
    (Bool, return ::MIR::Constant(e2); ),
    (Bytes, return ::MIR::Constant(e2); ),
    (StaticString, return ::MIR::Constant(e2); ),
    (Const, return ::MIR::Constant::make_Const({box$(e2.p->clone())}); ),
    (Generic, return ::MIR::Constant(e2); ),
    (Function, return ::MIR::Constant::make_Function({box$(e2.p->clone())}); ),
    (ItemAddr, return ::MIR::Constant(box$(e2->clone())); )
    )
    throw "";
}

::MIR::Param MIR::Param::clone() const
{
    TU_MATCHA( (*this), (e),
    (LValue,
        return e.clone();
        ),
    (Borrow,
        return ::MIR::Param::make_Borrow({ e.type, e.val.clone() });
        ),
    (Constant,
        return e.clone();
        )
    )
    throw "";
}

::MIR::RValue MIR::RValue::clone() const
{
    TU_MATCHA( (*this), (e),
    (Use,
        return ::MIR::RValue(e.clone());
        ),
    (Constant,
        return e.clone();
        ),
    (SizedArray,
        return ::MIR::RValue::make_SizedArray({ e.val.clone(), e.count.clone() });
        ),
    (Borrow,
        return ::MIR::RValue::make_Borrow({ e.type, e.is_raw, e.val.clone() });
        ),
    (Cast,
        return ::MIR::RValue::make_Cast({ e.val.clone(), e.type.clone() });
        ),
    (BinOp,
        return ::MIR::RValue::make_BinOp({ e.val_l.clone(), e.op, e.val_r.clone() });
        ),
    (UniOp,
        return ::MIR::RValue::make_UniOp({ e.val.clone(), e.op });
        ),
    (DstMeta,
        return ::MIR::RValue::make_DstMeta({ e.val.clone() });
        ),
    (DstPtr,
        return ::MIR::RValue::make_DstPtr({ e.val.clone() });
        ),
    // Construct a DST pointer from a thin pointer and metadata
    (MakeDst,
        return ::MIR::RValue::make_MakeDst({ e.ptr_val.clone(), e.meta_val.clone() });
        ),
    (Tuple,
        decltype(e.vals)    ret;
        ret.reserve(e.vals.size());
        for(const auto& v : e.vals)
            ret.push_back( v.clone() );
        return ::MIR::RValue::make_Tuple({ mv$(ret) });
        ),
    // Array literal
    (Array,
        decltype(e.vals)    ret;
        ret.reserve(e.vals.size());
        for(const auto& v : e.vals)
            ret.push_back( v.clone() );
        return ::MIR::RValue::make_Array({ mv$(ret) });
        ),
    // Create a new instance of a union
    (UnionVariant,
        return ::MIR::RValue::make_UnionVariant({ e.path.clone(), e.index, e.val.clone() });
        ),
    // Create a new instance of an enum
    (EnumVariant,
        decltype(e.vals)    ret;
        ret.reserve(e.vals.size());
        for(const auto& v : e.vals)
            ret.push_back( v.clone() );
        return ::MIR::RValue::make_EnumVariant({ e.path.clone(), e.index, mv$(ret) });
        ),
    // Create a new instance of a struct
    (Struct,
        decltype(e.vals)    ret;
        ret.reserve(e.vals.size());
        for(const auto& v : e.vals)
            ret.push_back( v.clone() );
        return ::MIR::RValue::make_Struct({ e.path.clone(), mv$(ret) });
        )
    )
    throw "";
}

::MIR::SwitchValues MIR::SwitchValues::clone() const
{
    TU_MATCHA( (*this), (ve),
    (Unsigned,
        return ve;
        ),
    (Signed,
        return ve;
        ),
    (String,
        return ve;
        ),
    (ByteString,
        return ve;
        )
    )
    throw "";
}

bool MIR::SwitchValues::operator==(const SwitchValues& x) const
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

const HIR::TypeRef& MIR::Cloner::value_generic_type(HIR::GenericRef ce) const
{
    TODO(sp, "`value_generic_type` not implemented, shouldn't be called unless `monomorpiser` has been overridden");
}
const Monomorphiser& MIR::Cloner::monomorphiser() const
{
    static MonomorphiserNop    nop;
    return nop;
}

::HIR::TypeRef MIR::Cloner::monomorph(const ::HIR::TypeRef& ty) const {
    TRACE_FUNCTION_F(ty);
    auto rv = monomorphiser().monomorph_type(sp, ty);
    if(auto* r = resolve()) {
        r->expand_associated_types(sp, rv);
    }
    return rv;
}
::HIR::GenericPath MIR::Cloner::monomorph(const ::HIR::GenericPath& ty) const {
    TRACE_FUNCTION_F(ty);
    auto rv = monomorphiser().monomorph_genericpath(sp, ty, false);
    if(const auto* r = resolve()) {
        for(auto& arg : rv.m_params.m_types)
            r->expand_associated_types(sp, arg);
    }
    return rv;
}
::HIR::Path MIR::Cloner::monomorph(const ::HIR::Path& ty) const {
    TRACE_FUNCTION_F(ty);
    auto rv = monomorphiser().monomorph_path(sp, ty, false);
    if(const auto* r = resolve()) {
        TU_MATCH(::HIR::Path::Data, (rv.m_data), (e2),
        (Generic,
            for(auto& arg : e2.m_params.m_types)
                r->expand_associated_types(sp, arg);
            ),
        (UfcsInherent,
            r->expand_associated_types(sp, e2.type);
            for(auto& arg : e2.params.m_types)
                r->expand_associated_types(sp, arg);
            // TODO: impl params too?
            for(auto& arg : e2.impl_params.m_types)
                r->expand_associated_types(sp, arg);
            ),
        (UfcsKnown,
            r->expand_associated_types(sp, e2.type);
            for(auto& arg : e2.trait.m_params.m_types)
                r->expand_associated_types(sp, arg);
            for(auto& arg : e2.params.m_types)
                r->expand_associated_types(sp, arg);
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown");
            )
        )
    }
    return rv;
}
::HIR::PathParams MIR::Cloner::monomorph(const ::HIR::PathParams& ty) const {
    TRACE_FUNCTION_F(ty);
    auto rv = monomorphiser().monomorph_path_params(sp, ty, false);
    if(const auto* r = resolve()) {
        for(auto& arg : rv.m_types)
            r->expand_associated_types(sp, arg);
    }
    return rv;
}

::std::vector<MIR::AsmParam> MIR::Cloner::clone_asm_params(const ::std::vector<MIR::AsmParam>& params) const
{
    ::std::vector<MIR::AsmParam>    rv;
    for(const auto& p : params)
    {
        TU_MATCH_HDRA((p), {)
        TU_ARMA(Const, v)
            rv.push_back( this->clone_constant(v) );
        TU_ARMA(Sym, v)
            rv.push_back( this->monomorph(v) );
        TU_ARMA(Reg, v)
            rv.push_back(::MIR::AsmParam::make_Reg({
                v.dir,
                v.spec.clone(),
                v.input  ? box$(this->clone_param(*v.input)) : std::unique_ptr<MIR::Param>(),
                v.output ? box$(this->clone_lval(*v.output)) : std::unique_ptr<MIR::LValue>()
                }));
        }
    }
    return rv;
}
::MIR::Statement MIR::Cloner::clone_stmt(const ::MIR::Statement& src) const
{
    TU_MATCH_HDRA( (src), { )
    TU_ARMA(Assign, se) {
        return ::MIR::Statement::make_Assign({
            this->clone_lval(se.dst),
            this->clone_rval(se.src)
            });
        }
    TU_ARMA(Asm, se) {
        return ::MIR::Statement::make_Asm({
            se.tpl,
            this->clone_name_lval_vec(se.outputs),
            this->clone_name_lval_vec(se.inputs),
            se.clobbers,
            se.flags
            });
        }
    TU_ARMA(Asm2, se) {
        return ::MIR::Statement::make_Asm2({
            se.options,
            se.lines,
            this->clone_asm_params(se.params)
            });
        }
    TU_ARMA(SetDropFlag, se) {
        return ::MIR::Statement::make_SetDropFlag({
            map_drop_flag(se.idx),
            se.new_val,
            se.other == ~0u ? ~0u : map_drop_flag(se.other)
            });
        }
    TU_ARMA(SaveDropFlag, se) {
        TODO(Span(), "clone_bb SaveDropFlag");
        }
    TU_ARMA(LoadDropFlag, se) {
        TODO(Span(), "clone_bb LoadDropFlag");
        }
    TU_ARMA(Drop, se) {
        return ::MIR::Statement::make_Drop({
            se.kind,
            this->clone_lval(se.slot),
            se.flag_idx == ~0u ? ~0u : map_drop_flag(se.flag_idx)
            });
        }
    TU_ARMA(ScopeEnd, se) {
        ::MIR::Statement::Data_ScopeEnd new_se;
        new_se.slots.reserve(se.slots.size());
        for(auto idx : se.slots)
            new_se.slots.push_back(map_local(idx));
        return ::MIR::Statement( mv$(new_se) );
        }
    }
    throw "";
}
::MIR::Terminator MIR::Cloner::clone_term(const ::MIR::Terminator& src) const
{
    TU_MATCH_HDRA( (src), { )
    TU_ARMA(Incomplete, se) {
        return ::MIR::Terminator::make_Incomplete({});
        }
    TU_ARMA(Return, se) {
        return ::MIR::Terminator::make_Return({});
        }
    TU_ARMA(Diverge, se) {
        return ::MIR::Terminator::make_Diverge({});
        }
    TU_ARMA(Panic, se) {
        return ::MIR::Terminator::make_Panic({});
        }
    TU_ARMA(Goto, se) {
        return ::MIR::Terminator::make_Goto(map_bb_idx(se));
        }
    TU_ARMA(If, se) {
        return ::MIR::Terminator::make_If({
            this->clone_lval(se.cond),
            map_bb_idx(se.bb_true  ),
            map_bb_idx(se.bb_false)
            });
        }
    TU_ARMA(Switch, se) {
        ::std::vector<::MIR::BasicBlockId>  arms;
        arms.reserve(se.targets.size());
        for(const auto& bbi : se.targets)
            arms.push_back( map_bb_idx(bbi) );
        return ::MIR::Terminator::make_Switch({ this->clone_lval(se.val), mv$(arms) });
        }
    TU_ARMA(SwitchValue, se) {
        ::std::vector<::MIR::BasicBlockId>  arms;
        arms.reserve(se.targets.size());
        for(const auto& bbi : se.targets)
            arms.push_back( map_bb_idx(bbi) );
        return ::MIR::Terminator::make_SwitchValue({ this->clone_lval(se.val), map_bb_idx(se.def_target), mv$(arms), se.values.clone() });
        }
    TU_ARMA(Call, se) {
        ::MIR::CallTarget   tgt;
        TU_MATCHA( (se.fcn), (ste),
        (Value,
            tgt = ::MIR::CallTarget::make_Value( this->clone_lval(ste) );
            ),
        (Path,
            tgt = ::MIR::CallTarget::make_Path( this->monomorph(ste) );
            ),
        (Intrinsic,
            tgt = ::MIR::CallTarget::make_Intrinsic({ ste.name, this->monomorph(ste.params) });
            )
        )
        return ::MIR::Terminator::make_Call({
            map_bb_idx(se.ret_block),
            map_bb_idx(se.panic_block),
            this->clone_lval(se.ret_val),
            mv$(tgt),
            this->clone_param_vec(se.args)
            });
        }
    }
    throw "";
}
::std::vector< ::std::pair<::std::string,::MIR::LValue> > MIR::Cloner::clone_name_lval_vec(const ::std::vector< ::std::pair<::std::string,::MIR::LValue> >& src) const
{
    ::std::vector< ::std::pair<::std::string,::MIR::LValue> >  rv;
    rv.reserve(src.size());
    for(const auto& e : src)
        rv.push_back(::std::make_pair(e.first, this->clone_lval(e.second)));
    return rv;
}
::std::vector<::MIR::LValue> MIR::Cloner::clone_lval_vec(const ::std::vector<::MIR::LValue>& src) const
{
    ::std::vector<::MIR::LValue>    rv;
    rv.reserve(src.size());
    for(const auto& lv : src)
        rv.push_back( this->clone_lval(lv) );
    return rv;
}
::std::vector<::MIR::Param> MIR::Cloner::clone_param_vec(const ::std::vector<::MIR::Param>& src) const
{
    ::std::vector<::MIR::Param>    rv;
    rv.reserve(src.size());
    for(const auto& lv : src)
        rv.push_back( this->clone_param(lv) );
    return rv;
}

::MIR::LValue MIR::Cloner::clone_lval(const ::MIR::LValue& src) const
{
    auto wrappers = src.m_wrappers;
    for(auto& w : wrappers)
    {
        if( w.is_Index() ) {
            w = ::MIR::LValue::Wrapper::new_Index( map_local(w.as_Index()) );
        }
    }
    TU_MATCH_HDRA( (src.m_root), {)
    TU_ARMA(Return, se) {
        return ::MIR::LValue( ::MIR::LValue::Storage::new_Return(), mv$(wrappers) );
        }
    TU_ARMA(Argument, se) {
        return ::MIR::LValue( ::MIR::LValue::Storage::new_Argument(se), mv$(wrappers) );
        }
    TU_ARMA(Local, se) {
        return ::MIR::LValue( ::MIR::LValue::Storage::new_Local(this->map_local(se)), mv$(wrappers) );
        }
    TU_ARMA(Static, se) {
        return ::MIR::LValue( ::MIR::LValue::Storage::new_Static(this->monomorph(se)), mv$(wrappers) );
        }
    }
    throw "";
}
::MIR::Constant MIR::Cloner::clone_constant(const ::MIR::Constant& src) const
{
    TU_MATCH_HDRA( (src), {)
    TU_ARMA(Int  , ce) return ::MIR::Constant(ce);
    TU_ARMA(Uint , ce) return ::MIR::Constant(ce);
    TU_ARMA(Float, ce) return ::MIR::Constant(ce);
    TU_ARMA(Bool , ce) return ::MIR::Constant(ce);
    TU_ARMA(Bytes, ce) return ::MIR::Constant(ce);
    TU_ARMA(StaticString, ce) return ::MIR::Constant(ce);
    TU_ARMA(Const, ce) {
        return ::MIR::Constant::make_Const({ box$(this->monomorph(*ce.p)) });
        }
    TU_ARMA(Generic, ce) {
        auto val = monomorphiser().get_value(sp, ce);
        TU_MATCH_HDRA( (val), {)
        default:
            TODO(sp, "Monomorphise MIR generic constant " << ce << " = " << val);
        TU_ARMA(Generic, ve) {
            return ve;
            }
        TU_ARMA(Evaluated, ve) {
            const auto& ty = this->value_generic_type(ce);
            auto v = EncodedLiteralSlice(*ve);
            ASSERT_BUG(sp, ty.data().is_Primitive(), "Handle non-primitive const generic: " << ty);
            // TODO: This is duplicated in `mir/from_hir_match.cpp` - De-duplicate?
            switch(ty.data().as_Primitive())
            {
            case ::HIR::CoreType::Bool: return ::MIR::Constant::make_Bool({ v.read_uint(1) != 0 });
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:  return ::MIR::Constant::make_Uint({ v.read_uint(ve->bytes.size()), ty.data().as_Primitive() });
            case ::HIR::CoreType::Usize:  return ::MIR::Constant::make_Uint({ v.read_uint(Target_GetPointerBits() / 8), ty.data().as_Primitive() });
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:  return ::MIR::Constant::make_Int({ v.read_sint(ve->bytes.size()), ty.data().as_Primitive() });
            case ::HIR::CoreType::Isize:  return ::MIR::Constant::make_Int({ v.read_sint(Target_GetPointerBits() / 8), ty.data().as_Primitive() });
            case ::HIR::CoreType::F16:
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
            case ::HIR::CoreType::F128: return ::MIR::Constant::make_Float({ v.read_float(ve->bytes.size()), ty.data().as_Primitive() });
            case ::HIR::CoreType::Char: return ::MIR::Constant::make_Uint({ v.read_uint(4), ty.data().as_Primitive() });
            case ::HIR::CoreType::Str:  BUG(sp, "`str` const generic");
            }
            }
        }
        }
    TU_ARMA(Function, ce) {
        return ::MIR::Constant::make_Function({ box$(this->monomorph(*ce.p)) });
        }
    TU_ARMA(ItemAddr, ce) {
        if(!ce)
            return ::MIR::Constant::make_ItemAddr({});
        return ::MIR::Constant::make_ItemAddr(box$(this->monomorph(*ce)));
        }
    }
    throw "";
}
::MIR::Param MIR::Cloner::clone_param(const ::MIR::Param& src) const
{
    TU_MATCHA( (src), (se),
    (LValue,
        return clone_lval(se);
        ),
    (Borrow,
        return ::MIR::Param::make_Borrow({ se.type, this->clone_lval(se.val) });
        ),
    (Constant,
        return clone_constant(se);
        )
    )
    throw "";
}
::MIR::RValue MIR::Cloner::clone_rval(const ::MIR::RValue& src) const
{
    TU_MATCH_HDRA( (src), {)
    TU_ARMA(Use, se) {
        //if( const auto* ae = se.opt_Argument() )
        //    if( const auto* e = this->te.args.at(ae->idx).opt_Constant() )
        //        return e->clone();
        return ::MIR::RValue( this->clone_lval(se) );
        }
    TU_ARMA(Constant, se) {
        return this->clone_constant(se);
        }
    TU_ARMA(SizedArray, se) {
        return ::MIR::RValue::make_SizedArray({ this->clone_param(se.val), monomorphiser().monomorph_arraysize(sp, se.count) });
        }
    TU_ARMA(Borrow, se) {
        return ::MIR::RValue::make_Borrow({ se.type, se.is_raw, this->clone_lval(se.val) });
        }
    TU_ARMA(Cast, se) {
        return ::MIR::RValue::make_Cast({ this->clone_lval(se.val), this->monomorph(se.type) });
        }
    TU_ARMA(BinOp, se) {
        return ::MIR::RValue::make_BinOp({ this->clone_param(se.val_l), se.op, this->clone_param(se.val_r) });
        }
    TU_ARMA(UniOp, se) {
        return ::MIR::RValue::make_UniOp({ this->clone_lval(se.val), se.op });
        }
    TU_ARMA(DstMeta, se) {
        return ::MIR::RValue::make_DstMeta({ this->clone_lval(se.val) });
        }
    TU_ARMA(DstPtr, se) {
        return ::MIR::RValue::make_DstPtr({ this->clone_lval(se.val) });
        }
    TU_ARMA(MakeDst, se) {
        return ::MIR::RValue::make_MakeDst({ this->clone_param(se.ptr_val), this->clone_param(se.meta_val) });
        }
    TU_ARMA(Tuple, se) {
        return ::MIR::RValue::make_Tuple({ this->clone_param_vec(se.vals) });
        }
    TU_ARMA(Array, se) {
        return ::MIR::RValue::make_Array({ this->clone_param_vec(se.vals) });
        }
    TU_ARMA(UnionVariant, se) {
        return ::MIR::RValue::make_UnionVariant({ this->monomorph(se.path), se.index, this->clone_param(se.val) });
        }
    TU_ARMA(EnumVariant, se) {
        return ::MIR::RValue::make_EnumVariant({ this->monomorph(se.path), se.index, this->clone_param_vec(se.vals) });
        }
    TU_ARMA(Struct, se) {
        return ::MIR::RValue::make_Struct({ this->monomorph(se.path), this->clone_param_vec(se.vals) });
        }
    }
    throw "";
}
