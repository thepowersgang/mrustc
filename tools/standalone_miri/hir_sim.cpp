/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * value.cpp
 * - Copy of the various HIR types from the compiler
 */
#include <iostream>
#include <algorithm>

#include "hir_sim.hpp"
#include "module_tree.hpp"
#include "debug.hpp"

//::HIR::Path::Path(::HIR::SimplePath sp)
//{
//}

size_t HIR::TypeRef::get_size(size_t ofs) const
{
    if( const auto* w = this->get_wrapper(ofs) )
    {
        switch(w->type)
        {
        case TypeWrapper::Ty::Array:
            return this->get_size(ofs+1) * w->size;
        case TypeWrapper::Ty::Borrow:
        case TypeWrapper::Ty::Pointer:
            if( const auto* next_w = this->get_wrapper(ofs+1) )
            {
                if( next_w->type == TypeWrapper::Ty::Slice )
                {
                    return POINTER_SIZE*2;
                }
                else
                {
                    return POINTER_SIZE;
                }
            }
            else
            {
                // Need to look up the metadata type for the actual type
                if( this->inner_type == RawType::Composite )
                {
                    if( this->composite_type().dst_meta == RawType::Unreachable )
                    {
                        return POINTER_SIZE;
                    }
                    // Special case: extern types (which appear when a type is only ever used by pointer)
                    if( this->composite_type().dst_meta == RawType::Unit )
                    {
                        return POINTER_SIZE;
                    }

                    // TODO: Ideally, this inner type wouldn't be unsized itself... but checking that would be interesting.
                    return POINTER_SIZE + this->composite_type().dst_meta.get_size();
                }
                else if( this->inner_type == RawType::Str )
                    return POINTER_SIZE*2;
                else if( this->inner_type == RawType::TraitObject )
                    return POINTER_SIZE*2;
                else
                {
                    return POINTER_SIZE;
                }
            }
        case TypeWrapper::Ty::Slice:
            LOG_BUG("Getting size of a slice - " << *this);
        }
        throw "";
    }
    else
    {
        switch(this->inner_type)
        {
        case RawType::Unit:
            return 0;
        case RawType::Composite:
            // NOTE: Don't care if the type has metadata
            LOG_ASSERT(this->composite_type().populated, "Getting size of non-defined type - " << *this);
            return this->composite_type().size;
        case RawType::Unreachable:
            LOG_BUG("Attempting to get size of an unreachable type, " << *this);
        case RawType::TraitObject:
        case RawType::Str:
            LOG_BUG("Attempting to get size of an unsized type, " << *this);
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

        case RawType::Bool:
            return 1;
        case RawType::Char:
            return 4;

        case RawType::F32:
            return 4;
        case RawType::F64:
            return 8;

        case RawType::Function: // This should probably be invalid?
        case RawType::USize: case RawType::ISize:
            return POINTER_SIZE;
        }
        throw "";
    }
}
size_t HIR::TypeRef::get_align(size_t ofs) const
{
    if( const auto* w = this->get_wrapper(ofs) )
    {
        LOG_TODO("get_align " << *this);
    }
    else
    {
        switch(this->inner_type)
        {
        case RawType::Unit:
            return 1;
        case RawType::Composite:
            // NOTE: Don't care if the type has metadata
            LOG_ASSERT(this->composite_type().populated, "Getting alignment of non-defined type - " << *this);
            return this->composite_type().alignment;
        case RawType::TraitObject:
        case RawType::Str:
            return 1;
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

        case RawType::Bool:
            return 1;
        case RawType::Char:
            return 4;

        case RawType::F32:
            return 4;
        case RawType::F64:
            return 8;

        case RawType::Function: // This should probably be invalid?
        case RawType::USize:
        case RawType::ISize:
            return POINTER_SIZE;
        case RawType::Unreachable:
            LOG_BUG("Getting alignment of unreachable type");
        }
        throw "";
    }
}
bool HIR::TypeRef::has_slice_meta(size_t& out_inner_size) const
{
    if( const auto* w = this->get_wrapper() )
    {
        out_inner_size = this->get_size(1);
        return (w->type == TypeWrapper::Ty::Slice);
    }
    else
    {
        if(this->inner_type == RawType::Composite)
        {
            // TODO: This type could be wrapping a slice, needs to return the inner type size.
            // - Also need to know which field is the unsized one
            return false;
        }
        else
        {
            out_inner_size = 1;
            return (this->inner_type == RawType::Str);
        }
    }
}

HIR::TypeRef HIR::TypeRef::get_inner() const
{
    if( this->wrappers.empty() )
    {
        LOG_ERROR("Getting inner of a non-wrapped type - " << *this);
    }
    else
    {
        auto ity = *this;
        ity.wrappers.erase(ity.wrappers.begin());
        return ity;
    }
}
HIR::TypeRef HIR::TypeRef::wrap(TypeWrapper::Ty ty, size_t size)&&
{
    auto rv = ::std::move(*this);
    rv.wrappers.insert(rv.wrappers.begin(), { ty, size });
    return rv;
}
bool HIR::TypeRef::has_pointer() const
{
    // If ALL of the (potentially non) wrappers are Array, look deeper
    // - Don't need to worry about unsized types here
    if( ::std::all_of(this->wrappers.begin(), this->wrappers.end(), [](const auto& x){ return x.type == TypeWrapper::Ty::Array; }) )
    {
        // TODO: Function pointers should be _pointers_
        if( this->inner_type == RawType::Function )
        {
            return true;
        }
        // Check the inner type
        if( this->inner_type == RawType::Composite )
        {
            // Still not sure, check the inner for any pointers.
            for(const auto& fld : this->composite_type().fields)
            {
                if( fld.second.has_pointer() )
                    return true;
            }
        }
        return false;
    }
    return true;
}
const HIR::TypeRef* HIR::TypeRef::get_unsized_type(size_t& running_inner_size) const
{
    if( const auto* w = this->get_wrapper() )
    {
        if( w->type == TypeWrapper::Ty::Slice )
        {
            return this;
        }
        else
        {
            return nullptr;
        }
    }
    else
    {
        switch(this->inner_type)
        {
        case RawType::Composite:
            if(!this->composite_type().variants.empty())
                return nullptr;
            if(this->composite_type().fields.empty())
                return nullptr;
            running_inner_size = this->composite_type().fields.back().first;
            size_t tmp;
            return this->composite_type().fields.back().second.get_unsized_type(tmp);
        case RawType::TraitObject:
        case RawType::Str:
            return this;
        default:
            return nullptr;
        }
    }
}
HIR::TypeRef HIR::TypeRef::get_meta_type() const
{
    if( const auto* w = this->get_wrapper() )
    {
        if( w->type == TypeWrapper::Ty::Slice )
        {
            return TypeRef(RawType::USize);
        }
        else
        {
            return TypeRef(RawType::Unreachable);
        }
    }
    else
    {
        switch(this->inner_type)
        {
        case RawType::Composite:
            if( this->composite_type().dst_meta == RawType::Unreachable )
                return TypeRef(RawType::Unreachable);
            return this->composite_type().dst_meta;
        case RawType::TraitObject:
            LOG_ASSERT(this->ptr.composite_type, "get_meta_type - " << *this);
            return ::HIR::TypeRef(this->ptr.composite_type).wrap( TypeWrapper::Ty::Pointer, static_cast<size_t>(BorrowType::Shared) );
        case RawType::Str:
            return TypeRef(RawType::USize);
        default:
            return TypeRef(RawType::Unreachable);
        }
    }
}

HIR::TypeRef HIR::TypeRef::get_field(size_t idx, size_t& ofs) const
{
    if( const auto* w = this->get_wrapper() )
    {
        if( w->type == TypeWrapper::Ty::Slice )
        {
            // NOTE: Treated as an unchecked index
            auto ity = this->get_inner();
            ofs = ity.get_size() * idx;
            return ity;
        }
        else if( w->type == TypeWrapper::Ty::Array )
        {
            LOG_ASSERT(idx < w->size, "Getting field on array with OOB index - " << idx << " >= " << w->size << " - " << *this);
            auto ity = this->get_inner();
            ofs = ity.get_size() * idx;
            return ity;
        }
        else
        {
            LOG_ERROR("Field on unknown wrapper type - " << *this << " #" << idx);
        }
    }
    else
    {
        if( this->inner_type == RawType::Composite )
        {
            LOG_ASSERT(idx < this->composite_type().fields.size(), "Field " << idx << " out of bounds in type " << *this);
            ofs = this->composite_type().fields.at(idx).first;
            return this->composite_type().fields.at(idx).second;
        }
        else
        {
            LOG_ERROR(*this << " doesn't have fields");
        }
    }
}
size_t HIR::TypeRef::get_field_ofs(size_t base_idx, const ::std::vector<size_t>& other_idx,  TypeRef& ty) const
{
    assert(this->wrappers.size() == 0);
    assert(this->inner_type == RawType::Composite);
    size_t ofs = this->composite_type().fields.at(base_idx).first;
    const auto* ty_p = &this->composite_type().fields.at(base_idx).second;
    for(auto idx : other_idx)
    {
        assert(ty_p->wrappers.size() == 0);
        assert(ty_p->inner_type == RawType::Composite);
        ofs += ty_p->composite_type().fields.at(idx).first;
        ty_p = &ty_p->composite_type().fields.at(idx).second;
    }
    ty = *ty_p;
    return ofs;
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
            os << x.composite_type().my_path;
            //os << "composite_" << x.composite_type;
            break;
        case RawType::Unreachable:
            os << "!";
            break;
        case RawType::Function: {
            assert( x.ptr.function_type );
            const auto& ft = *x.ptr.function_type;
            if( ft.unsafe )
                os << "unsafe ";
            if( ft.abi != "Rust" )
                os << "extern \"" << ft.abi << "\" ";
            os << "fn( ";
            for(const auto& a : ft.args)
                os << a << ", ";
            os << ")";
            if( ft.ret != RawType::Unit )
                os << "-> " << ft.ret;
            } break;
        case RawType::TraitObject:
            os << "dyn ";
            if( x.ptr.composite_type )
                os << x.composite_type().my_path;
            else
                os << "?";
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
        case RawType::F32:  os << "f32";    break;
        case RawType::F64:  os << "f64";    break;
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
}
