//
//
//
#include "value.hpp"
#include "hir_sim.hpp"
#include "module_tree.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

Value::Value()
{
    this->meta.direct_data.size = 0;
    this->meta.direct_data.mask[0] = 0;
    this->meta.direct_data.mask[1] = 0;
}
Value::Value(::HIR::TypeRef ty)
{
    size_t size = ty.get_size();
#if 1
    if( ty.get_size() <= sizeof(this->meta.direct_data.data) )
    {
        struct H
        {
            static bool has_pointer(const ::HIR::TypeRef& ty)
            {
                if( ty.wrappers.empty() || ::std::all_of(ty.wrappers.begin(), ty.wrappers.end(), [](const auto& x){ return x.type == TypeWrapper::Ty::Array; }) )
                {
                    // Check the inner type
                    if( ty.inner_type != RawType::Composite )
                    {
                        return false;
                    }
                    // Still not sure, check the inner for any pointers.
                    for(const auto& fld : ty.composite_type->fields)
                    {
                        if( H::has_pointer(fld.second) )
                            return true;
                    }
                    return false;
                }
                return true;
            }
        };
        if( ! H::has_pointer(ty) )
        {
            // Will fit in a inline allocation, nice.
            this->meta.direct_data.size = static_cast<uint8_t>(size);
            return ;
        }
    }
#endif

    // Fallback: Make a new allocation
    throw "TODO";
}

void Value::check_bytes_valid(size_t ofs, size_t size) const
{
    if( this->allocation )
    {
        throw "TODO";
    }
    else
    {
        for(size_t i = 0; i < this->meta.direct_data.size; i++)
        {
            if( !(this->meta.direct_data.mask[i/8] & (1 << i%8)) )
            {
                throw "ERROR";
            }
        }
    }
}
void Value::mark_bytes_valid(size_t ofs, size_t size)
{
    if( this->allocation )
    {
        throw "TODO";
    }
    else
    {
        for(size_t i = 0; i < this->meta.direct_data.size; i++)
        {
            this->meta.direct_data.mask[i/8] |= (1 << i%8);
        }
    }
}

Value Value::read_value(size_t ofs, size_t size) const
{
    ::std::cout << "Value::read_value(" << ofs << ", " << size << ")" << ::std::endl;
    check_bytes_valid(ofs, size);
    if( this->allocation )
    {
        throw "TODO";
    }
    else
    {
        // Inline can become inline.
        Value   rv;
        rv.meta.direct_data.size = static_cast<uint8_t>(size);
        rv.write_bytes(0, this->meta.direct_data.data+ofs, size);
        return rv;
    }
}
void Value::read_bytes(size_t ofs, void* dst, size_t count) const
{
    check_bytes_valid(ofs, count);
    throw "TODO";
}

void Value::write_bytes(size_t ofs, const void* src, size_t count)
{
    if( this->allocation )
    {
        throw "TODO";
    }
    else
    {
        if(ofs >= this->meta.direct_data.size )
            throw "ERROR";
        if(count > this->meta.direct_data.size )
            throw "ERROR";
        if(ofs+count > this->meta.direct_data.size )
            throw "ERROR";
        ::std::memcpy(this->meta.direct_data.data+ofs, src, count);
        mark_bytes_valid(ofs, count);
    }
}
void Value::write_value(size_t ofs, Value v)
{
    if( v.allocation )
    {
        throw "TODO";
    }
    else
    {
        v.check_bytes_valid(0, v.meta.direct_data.size);
        write_bytes(ofs, v.meta.direct_data.data, v.meta.direct_data.size);
        mark_bytes_valid(ofs, meta.direct_data.size);
    }
}

size_t Value::as_usize() const
{
    uint64_t    v;
    this->read_bytes(0, &v, 8);
    // TODO: Handle endian
    return v;
}
::std::ostream& operator<<(::std::ostream& os, const Value& v)
{
    auto flags = os.flags();
    os << ::std::hex;
    if( v.allocation )
    {
        throw "TODO";
    }
    else
    {
        for(size_t i = 0; i < v.meta.direct_data.size; i++)
        {
            if( i != 0 )
                os << " ";
            if( v.meta.direct_data.mask[i/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)v.meta.direct_data.data[i];
            }
            else
            {
                os << "--";
            }
        }
    }
    os.setf(flags);
    return os;
}