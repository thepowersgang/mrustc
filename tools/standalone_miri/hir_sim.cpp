//
//
//
#include <iostream>

#include "hir_sim.hpp"
#include "module_tree.hpp"

//::HIR::Path::Path(::HIR::SimplePath sp)
//{
//}

size_t HIR::TypeRef::get_size(size_t ofs) const
{
    const size_t POINTER_SIZE = 8;
    if( this->wrappers.size() <= ofs )
    {
        switch(this->inner_type)
        {
        case RawType::Unit:
            return 0;
        case RawType::Composite:
            return this->composite_type->size;
        case RawType::Unreachable:
        case RawType::Str:
            throw "Invalid";
        case RawType::U8:   case RawType::I8:
            return 1;
        case RawType::U16:  case RawType::I16:
            return 2;
        case RawType::U32:  case RawType::I32:
            return 4;
        case RawType::U64:  case RawType::I64:
            return 8;
        case RawType::U128: case RawType::I128:
            return 16;

        case RawType::Function: // This should probably be invalid?
        case RawType::USize: case RawType::ISize:
            return POINTER_SIZE;
        }
        throw "";
    }
    
    switch(this->wrappers[ofs].type)
    {
    case TypeWrapper::Ty::Array:
        return this->get_size(1) * this->wrappers[ofs].size;
    case TypeWrapper::Ty::Borrow:
    case TypeWrapper::Ty::Pointer:
        if( this->wrappers.size() == ofs+1 )
        {
            // Need to look up the metadata type for the actual type
            if( this->inner_type == RawType::Composite )
                throw "TODO";
            else if( this->inner_type == RawType::Str )
                return POINTER_SIZE*2;
            else if( this->inner_type == RawType::TraitObject )
                return POINTER_SIZE*2;
            else
            {
                return POINTER_SIZE;
            }
        }
        else if( this->wrappers[ofs+1].type == TypeWrapper::Ty::Slice )
        {
            return POINTER_SIZE*2;
        }
        else
        {
            return POINTER_SIZE;
        }
    case TypeWrapper::Ty::Slice:
        throw "Invalid";
    }
    throw "";
}

HIR::TypeRef HIR::TypeRef::get_inner() const
{
    if( this->wrappers.empty() )
    {
        throw "ERROR";
    }
    auto ity = *this;
    ity.wrappers.erase(ity.wrappers.begin());
    return ity;
}

HIR::TypeRef HIR::TypeRef::get_field(size_t idx, size_t& ofs) const
{
    if( this->wrappers.empty() )
    {
        if( this->inner_type == RawType::Composite )
        {
            ofs = this->composite_type->fields.at(idx).first;
            return this->composite_type->fields.at(idx).second;
        }
        else
        {
            ::std::cerr << *this << " doesn't have fields" << ::std::endl;
            throw "ERROR";
        }
    }
    else if( this->wrappers.front().type == TypeWrapper::Ty::Slice )
    {
        // TODO
        throw "TODO";
    }
    else if( this->wrappers.front().type == TypeWrapper::Ty::Array )
    {
        auto ity = this->get_inner();
        ofs = ity.get_size() * idx;
        return ity;
    }
    else
    {
        throw "ERROR";
    }
}

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::BorrowType& x)
    {
        switch(x)
        {
        case ::HIR::BorrowType::Move:   os << "Move";   break;
        case ::HIR::BorrowType::Unique: os << "Unique"; break;
        case ::HIR::BorrowType::Shared: os << "Shared"; break;
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::TypeRef& x)
    {
        for(auto it = x.wrappers.begin(); it != x.wrappers.end(); ++it)
        {
            switch(it->type)
            {
            case TypeWrapper::Ty::Array:
            case TypeWrapper::Ty::Slice:
                os << "[";
                break;
            case TypeWrapper::Ty::Pointer:
                os << "*";
                switch(it->size)
                {
                case 2: os << "move ";  break;
                case 1: os << "mut ";   break;
                case 0: os << "const "; break;
                default:
                    break;
                }
                break;
            case TypeWrapper::Ty::Borrow:
                os << "&";
                switch(it->size)
                {
                case 2: os << "move ";  break;
                case 1: os << "mut ";   break;
                case 0: os << "";       break;
                default:
                    break;
                }
                break;
            }
        }
        switch(x.inner_type)
        {
        case RawType::Unit:
            os << "()";
            break;
        case RawType::Composite:
            os << "composite_" << x.composite_type;
            break;
        case RawType::Unreachable:
            os << "!";
            break;
        case RawType::Bool: os << "bool"; break;
        case RawType::Char: os << "char"; break;
        case RawType::Str:  os << "str";  break;

        case RawType::U8:   os << "u8"; break;
        case RawType::I8:   os << "i8"; break;
        case RawType::U16:  os << "u16";    break;
        case RawType::I16:  os << "i16";    break;
        case RawType::U32:  os << "u32";    break;
        case RawType::I32:  os << "i32";    break;
        case RawType::U64:  os << "u64";    break;
        case RawType::I64:  os << "i64";    break;
        case RawType::U128: os << "u128";   break;
        case RawType::I128: os << "i128";   break;
        case RawType::USize: os << "usize";   break;
        case RawType::ISize: os << "isize";   break;
        }
        for(auto it = x.wrappers.rbegin(); it != x.wrappers.rend(); ++it)
        {
            switch(it->type)
            {
            case TypeWrapper::Ty::Array:
                os << ";" << it->size;
            case TypeWrapper::Ty::Slice:
                os << "]";
                break;
            case TypeWrapper::Ty::Pointer:
            case TypeWrapper::Ty::Borrow:
                break;
            }
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const SimplePath& x)
    {
        os << "::\"" << x.crate_name << "\"";
        for(const auto& e : x.ents)
        {
            os << "::" << e;
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::PathParams& x)
    {
        if( !x.tys.empty() )
        {
            os << "<";
            for(const auto& t : x.tys)
                os << t << ",";
            os << ">";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x)
    {
        os << x.m_simplepath;
        os << x.m_params;
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::Path& x)
    {
        if( x.m_name == "" )
        {
            os << x.m_trait;
        }
        else
        {
            os << "<" << x.m_type;
            if( x.m_trait != ::HIR::GenericPath() )
            {
                os << " as " << x.m_trait;
            }
            os << ">::" << x.m_name << x.m_params;
        }
        return os;
    }
}