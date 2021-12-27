/*
 */
#include "codegen.hpp"


Codegen_C::Codegen_C(const char* outfile):
    m_of(outfile)
{
}
Codegen_C::~Codegen_C()
{
}

void Codegen_C::emit_type_proto(const HIR::TypeRef& ty)
{
    TRACE_FUNCTION_R(ty, ty);
}
void Codegen_C::emit_static_proto(const RcString& name, const Static& s)
{
    TRACE_FUNCTION_R(name, name);
}
void Codegen_C::emit_function_proto(const RcString& name, const Function& s)
{
    TRACE_FUNCTION_R(name, name);
    this->emit_ctype(s.ret_ty);
    m_of << " " << name;
    m_of << "(";
    for(const auto& ty : s.args)
    {
        if(&ty != &s.args.front())
            m_of << ", ";
        this->emit_ctype(ty);
    }
    m_of << ");\n";
}

void Codegen_C::emit_composite(const RcString& name, const DataType& s)
{
    TRACE_FUNCTION_R(name, name);
}

void Codegen_C::emit_static(const RcString& name, const Static& s)
{
    TRACE_FUNCTION_R(name, name);
}

namespace {
    struct Indent {
        unsigned    indent;
        Indent(unsigned indent): indent(indent) {}

        friend std::ostream& operator<<(std::ostream& os, const Indent& x) {
            for(unsigned i = x.indent; i --; )
                os << "\t";
            return os;
        }
    };
}


void Codegen_C::emit_function(const RcString& name, const ModuleTree& tree, const Function& s)
{
    class BodyEmitter
    {
        Codegen_C& m_parent;
        ::std::ostream& m_of;
        const ModuleTree& m_module_tree;
        const Function& m_fcn;

    public:
        BodyEmitter(Codegen_C& parent, const ModuleTree& module_tree, const Function& fcn)
            : m_parent(parent)
            , m_of(parent.m_of)
            , m_module_tree(module_tree)
            , m_fcn(fcn)
        {
        }

        void emit_ctype(const ::HIR::TypeRef& ty) {
            m_parent.emit_ctype(ty);
        }

        const ::HIR::TypeRef& get_lvalue_type(const ::MIR::LValue::CRef& val) const
        {
            ::HIR::TypeRef ty;
            TU_MATCH_HDRA( (val.lv().m_root), {)
            TU_ARMA(Static, ve) {
                ty = m_module_tree.get_static(ve).ty;
                }
            TU_ARMA(Return, _ve) {
                ty = m_fcn.ret_ty;
                }
            TU_ARMA(Argument, ve) {
                ty = m_fcn.args.at(ve);
                }
            TU_ARMA(Local, ve) {
                ty = m_fcn.m_mir.locals.at(ve);
                }
            }

            LOG_ASSERT(val.wrapper_count() <= val.lv().m_wrappers.size(), "");
            for(size_t i = 0; i < val.wrapper_count(); i ++)
            {
                TU_MATCH_HDRA( (val.lv().m_wrappers[i]), {)
                TU_ARMA(Index, we) { ty = ty.get_inner(); }
                TU_ARMA(Field, we) { size_t ofs; ty = ty.get_field(we, ofs); }
                TU_ARMA(Deref, we) { ty = ty.get_inner(); }
                TU_ARMA(Downcast, we) {
                    size_t ofs;
                    ty = ty.get_field(we, ofs);
                    }
                }
            }
            return ty;
        }

        void emit_lvalue(const ::MIR::LValue::CRef& val)
        {
            TU_MATCH_HDRA( (val), {)
            TU_ARMA(Return, _e) {
                m_of << "rv";
            }
            TU_ARMA(Argument, e) {
                m_of << "arg" << e;
            }
            TU_ARMA(Local, e) {
                if( e == ::MIR::LValue::Storage::MAX_ARG )
                    m_of << "i";
                else
                    m_of << "var" << e;
            }
            TU_ARMA(Static, e) {
                m_of << e;
                m_of << ".val";
            }
            TU_ARMA(Field, field_index) {
                auto inner = val.inner_ref();

                const auto& ty = this->get_lvalue_type(inner);
                if( !ty.wrappers.empty() )
                {
                    switch(ty.wrappers.back().type)
                    {
                    case TypeWrapper::Ty::Slice:
                        if( inner.is_Deref() )
                        {
                            m_of << "(("; emit_ctype(ty.get_inner()); m_of << "*)";
                            emit_lvalue(inner.inner_ref());
                            m_of << ".PTR)";
                        }
                        else
                        {
                            emit_lvalue(inner);
                        }
                        m_of << "[" << field_index << "]";
                        break;
                    case TypeWrapper::Ty::Array:
                        emit_lvalue(inner);
                        m_of << ".DATA[" << field_index << "]";
                        break;
                    case TypeWrapper::Ty::Pointer:
                    case TypeWrapper::Ty::Borrow:
                        LOG_ASSERT(inner.is_Deref(), "");
                        auto dst_type = ty.get_meta_type();
                        if( dst_type != HIR::TypeRef() )
                        {
                            m_of << "(("; emit_ctype(ty); m_of << "*)"; emit_lvalue(inner.inner_ref()); m_of << ".PTR)->_" << field_index;
                        }
                        else
                        {
                            emit_lvalue(inner.inner_ref());
                            m_of << "->_" << field_index;
                        }
                        break;
                    }
                }
                else
                {
                    emit_lvalue(inner);
                    m_of << "._" << field_index;
                }
            }
            TU_ARMA(Deref, _e) {
                auto inner = val.inner_ref();
                const auto& ty = this->get_lvalue_type(inner);
                auto dst_type = ty.get_meta_type();
                // If the type is unsized, then this pointer is a fat pointer, so we need to cast the data pointer.
                if( dst_type != HIR::TypeRef() )
                {
                    m_of << "(*("; emit_ctype(ty); m_of << "*)";
                    emit_lvalue(inner);
                    m_of << ".PTR)";
                }
                else
                {
                    m_of << "(*";
                    emit_lvalue(inner);
                    m_of << ")";
                }
            }
            TU_ARMA(Index, index_local) {
                auto inner = val.inner_ref();
                const auto& ty = this->get_lvalue_type(inner);
                LOG_ASSERT( !ty.wrappers.empty(), "" );
                m_of << "(";
                switch(ty.wrappers.back().type)
                {
                case TypeWrapper::Ty::Slice:
                    if( inner.is_Deref() )
                    {
                        m_of << "("; emit_ctype(ty.get_inner()); m_of << "*)";
                        emit_lvalue(inner.inner_ref());
                        m_of << ".PTR";
                    }
                    else {
                        emit_lvalue(inner);
                    }
                    break;
                case TypeWrapper::Ty::Array:
                    emit_lvalue(inner);
                    m_of << ".DATA";
                    break;
                case TypeWrapper::Ty::Pointer:
                case TypeWrapper::Ty::Borrow:
                    LOG_ASSERT(false, "");
                }
                m_of << ")[";
                emit_lvalue(::MIR::LValue::new_Local(index_local));
                m_of << "]";
            }
            TU_ARMA(Downcast, variant_index) {
                auto inner = val.inner_ref();
                const auto& ty = this->get_lvalue_type(inner);
                emit_lvalue(inner);
                LOG_ASSERT(ty.wrappers.empty(), "");
                const auto& dt = ty.composite_type();
                if(dt.variants.size() == 0)
                {
                    m_of << ".var_" << variant_index;
                }
                else
                {
                    LOG_ASSERT(variant_index < dt.variants.size(), "");
                    m_of << ".DATA";
                    m_of << ".var_" << variant_index;
                }
            }
            }
        }
        void emit_lvalue(const ::MIR::LValue& lv)
        {
            emit_lvalue(::MIR::LValue::CRef(lv));
        }

        void emit_drop(unsigned indent, const ::HIR::TypeRef& ty, const ::MIR::LValue& lv)
        {
            if( ty.wrappers.size() == 0 )
            {
                switch(ty.inner_type)
                {
                case RawType::Composite:
                    if( ty.composite_type().drop_glue != HIR::Path() )
                    {
                        m_of << ty.composite_type().drop_glue << "(&"; emit_lvalue(lv); m_of << ");";
                    }
                    break;
                case RawType::TraitObject:
                    // Call destructor
                    break;

                case RawType::I8:   case RawType::U8:
                case RawType::I16:  case RawType::U16:
                case RawType::I32:  case RawType::U32:
                case RawType::I64:  case RawType::U64:
                case RawType::I128: case RawType::U128:
                case RawType::ISize:case RawType::USize:
                case RawType::F32:  case RawType::F64:
                case RawType::Bool:
                case RawType::Str:
                case RawType::Char:
                    break;
                case RawType::Function:
                case RawType::Unit:
                case RawType::Unreachable:
                    break;
                }
            }
            else
            {
                //switch(ty.wrappers.back().type)
                //{
                //}
            }
        }

        void emit_block_statements(const ::MIR::BasicBlock& b, unsigned indent)
        {
            for(const auto& stmt : b.statements)
            {
                m_of << Indent(indent);
                TU_MATCH_HDRA( (stmt), {)
                TU_ARMA(Asm, se) {
                    LOG_TODO(stmt);
                    }
                TU_ARMA(Asm2, se) {
                    LOG_TODO(stmt);
                    }
                TU_ARMA(Assign, se) {
                    emit_lvalue(se.dst);
                    m_of << " = ";
                    }
                TU_ARMA(SetDropFlag, se) {
                    LOG_TODO(stmt);
                    }
                TU_ARMA(Drop, se) {
                    const auto& ty = this->get_lvalue_type(se.slot);
                    if( se.flag_idx != ~0u ) {
                    }
                    switch(se.kind)
                    {
                    case ::MIR::eDropKind::DEEP:
                        emit_drop(indent, ty, se.slot);
                        break;
                    case ::MIR::eDropKind::SHALLOW:
                        break;
                    }
                    }
                TU_ARMA(ScopeEnd, se) {
                    }
                }
                m_of << "// BB?/" << (&stmt - &b.statements.front()) << " " << stmt << "\n";
            }
        }
    };

    TRACE_FUNCTION_R(name, name);
    BodyEmitter emitter(*this, tree, s);

    this->emit_ctype(s.ret_ty);
    m_of << " " << name;
    m_of << "(";
    for(const auto& ty : s.args)
    {
        size_t idx = &ty - &s.args.front();
        if(&ty != &s.args.front())
            m_of << ", ";
        this->emit_ctype(ty);
        m_of << " a" << idx;
    }
    m_of << ")\n";
    m_of << "{\n";
    for(const auto& b : s.m_mir.blocks)
    {
        m_of << "bb" << (&b - &s.m_mir.blocks.front()) << ":\n";
        emitter.emit_block_statements(b, /*indent=*/1);
    }
    m_of << "}\n";
}

struct FmtMangledTy {
    const HIR::TypeRef& t;
    unsigned depth;
    FmtMangledTy(const HIR::TypeRef& t, unsigned depth):
        t(t), depth(depth)
    {
    }

    void fmt(std::ostream& os) const
    {
        if(t.wrappers.size() == depth)
        {
            switch(t.inner_type)
            {
            case RawType::Unreachable:
                os << 'C' << 'z';
                break;
            case RawType::Unit:
                os << "T0";
                break;
            case RawType::U8  : os << 'C' << 'a'; break;
            case RawType::I8  : os << 'C' << 'b'; break;
            case RawType::U16 : os << 'C' << 'c'; break;
            case RawType::I16 : os << 'C' << 'd'; break;
            case RawType::U32 : os << 'C' << 'e'; break;
            case RawType::I32 : os << 'C' << 'f'; break;
            case RawType::U64 : os << 'C' << 'g'; break;
            case RawType::I64 : os << 'C' << 'h'; break;
            case RawType::U128: os << 'C' << 'i'; break;
            case RawType::I128: os << 'C' << 'j'; break;
            case RawType::F32 : os << 'C' << 'n'; break;
            case RawType::F64 : os << 'C' << 'o'; break;
            case RawType::USize: os << 'C' << 'u'; break;
            case RawType::ISize: os << 'C' << 'v'; break;
            case RawType::Bool: os << 'C' << 'w'; break;
            case RawType::Char: os << 'C' << 'x'; break;
            case RawType::Str : os << 'C' << 'y'; break;

            case RawType::Composite:
                os << t.ptr.composite_type->my_path;
                break;
            case RawType::TraitObject:
                LOG_TODO(t);
                break;
            case RawType::Function: {
                const auto& e = *t.ptr.function_type;;
                // - Function: 'F' <abi:RcString> <nargs> [args: <TypeRef> ...] <ret:TypeRef>
                os << "F";
                //os << (e.is_unsafe ? "u" : "");    // Optional allowed, next is a number
                if( e.abi != "Rust" )
                {
                    os << "e";
                    os << e.abi.size();
                    os << e.abi;
                }
                os << e.args.size();
                for(const auto& t : e.args)
                    os << FmtMangledTy(t, 0);
                os << FmtMangledTy(e.ret, 0);
                } break;
            }
        }
        else
        {
            const auto& w = t.wrappers.at(depth);
            switch(w.type)
            {
            case TypeWrapper::Ty::Array:
                os << "A" << w.size;
                os << FmtMangledTy(t, depth+1);
                break;
            case TypeWrapper::Ty::Slice:
                os << "S";
                break;
            case TypeWrapper::Ty::Pointer:
                os << "P"; if(0)
            case TypeWrapper::Ty::Borrow:
                os << "B";

                switch(static_cast<::HIR::BorrowType>(w.size))
                {
                case ::HIR::BorrowType::Shared: os << "s"; break;
                case ::HIR::BorrowType::Unique: os << "u"; break;
                case ::HIR::BorrowType::Move:   os << "o"; break;
                }
                break;
            }
            os << FmtMangledTy(t, depth+1);
        }
    }
    friend std::ostream& operator<<(std::ostream& os, const FmtMangledTy& x)
    {
        x.fmt(os);
        return os;
    }
};

void Codegen_C::emit_ctype(const HIR::TypeRef& t, unsigned depth/*=0*/)
{
    if(t.wrappers.size() == depth)
    {
        switch(t.inner_type)
        {
        case RawType::Unreachable:
            m_of << "tBANG";
            break;
        case RawType::Bool:
            m_of << "bool";
            break;
        case RawType::Unit:
            m_of << "ZRT" << "T0";
            break;
        case RawType::U8:   m_of << "uint8_t";  break;
        case RawType::U16:  m_of << "uint16_t"; break;
        case RawType::U32:  m_of << "uint32_t"; break;
        case RawType::U64:  m_of << "uint64_t"; break;
        case RawType::I8:   m_of << "int8_t";   break;
        case RawType::I16:  m_of << "int16_t";  break;
        case RawType::I32:  m_of << "int32_t";  break;
        case RawType::I64:  m_of << "int64_t";  break;

        case RawType::U128:  m_of << "uint128_t"; break;
        case RawType::I128:  m_of << "int128_t";  break;

        case RawType::USize:    m_of << "uintptr_t";    break;
        case RawType::ISize:    m_of << "intptr_t";     break;

        case RawType::F32:  m_of << "float";    break;
        case RawType::F64:  m_of << "double";   break;

        case RawType::Char:
            m_of << "uint32_t";
            break;

        case RawType::Str:
            LOG_ERROR("Hit a str - " << t);
            break;

        case RawType::Composite:
            m_of << t.ptr.composite_type->my_path;
            break;
        case RawType::TraitObject:
            LOG_TODO(t);
            break;
        case RawType::Function:
            // Should be pre-defined, just emit the type name for it
            m_of << "ZRT" << FmtMangledTy(t, depth);
            break;
        }
    }
    else
    {
        const auto& w = t.wrappers.at(depth);
        switch(w.type)
        {
        case TypeWrapper::Ty::Array:
            // Arrays should be pre-defined, just emit the type name for it
            m_of << "ZRT" << FmtMangledTy(t, depth);
            break;
        case TypeWrapper::Ty::Slice:
            // Invalid, must be behind a pointer
            LOG_ERROR("Hit a slice - " << t);
            break;
        case TypeWrapper::Ty::Pointer:
        case TypeWrapper::Ty::Borrow:
            // Check for unsized inner, otherwise recurse with a trailing *
            if( depth+1 == t.wrappers.size() && t.inner_type == RawType::Str )
            {
                m_of << "SLICE_PTR";
            }
            else if( depth+1 < t.wrappers.size() && t.wrappers[depth+1].type == TypeWrapper::Ty::Slice )
            {
                m_of << "SLICE_PTR";
            }
            else if( depth+1 == t.wrappers.size() && t.inner_type == RawType::TraitObject )
            {
                m_of << "TRAITOBJ_PTR";
            }
            else if( depth+1 == t.wrappers.size() && t.inner_type == RawType::Composite && t.ptr.composite_type->dst_meta != HIR::TypeRef() )
            {
                if( t.ptr.composite_type->dst_meta == RawType::USize )
                {
                    m_of << "SLICE_PTR";
                }
                else
                {
                    m_of << "TRAITOBJ_PTR";
                }
            }
            else
            {
                this->emit_ctype(t, depth+1);
                m_of << "*";
            }
            break;
        }
    }
}