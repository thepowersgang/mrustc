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
void Codegen_C::emit_function(const RcString& name, const Function& s)
{
    TRACE_FUNCTION_R(name, name);

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
        for(const auto& stmt : b.statements)
        {
        }
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