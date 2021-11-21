/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir.cpp
 * - MIR (Middle Intermediate Representation) definitions
 */
#include <mir/mir.hpp>
#include <algorithm>    // std::min

namespace MIR {
    ::std::ostream& operator<<(::std::ostream& os, const Constant& v) {
        TU_MATCHA( (v), (e),
        (Int,
            os << (e.v < 0 ? "-" : "+");
            os << (e.v < 0 ? -e.v : e.v);
            os << " " << e.t;
            ),
        (Uint,
            os << e.v;
            os << " " << e.t;
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
            assert(e.p);
            os << *e.p;
            ),
        (Generic,
            os << e;
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
            os << "If( " << e.cond << " : " << e.bb0 << ", " << e.bb1 << ")";
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
            if( ae.bb0 != be.bb0 )
                return false;
            if( ae.bb1 != be.bb1 )
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
            os << ") = llvm_asm!(\"" << e.tpl << "\", input=( ";
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
        return ::MIR::RValue::make_Borrow({ e.type, e.val.clone() });
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

