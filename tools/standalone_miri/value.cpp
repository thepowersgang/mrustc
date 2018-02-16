//
//
//
#include "value.hpp"
#include "hir_sim.hpp"
#include "module_tree.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include "debug.hpp"

AllocationPtr Allocation::new_alloc(size_t size)
{
    Allocation* rv = new Allocation();
    rv->refcount = 1;
    rv->data.resize( (size + 8-1) / 8 );    // QWORDS
    rv->mask.resize( (size + 8-1) / 8 );    // bitmap bytes
    //LOG_DEBUG(rv << " ALLOC");
    return AllocationPtr(rv);
}
AllocationPtr AllocationPtr::new_fcn(::HIR::Path p)
{
    AllocationPtr   rv;
    auto* ptr = new ::HIR::Path(::std::move(p));
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::Function) );
    return rv;
}
AllocationPtr::AllocationPtr(const AllocationPtr& x):
    m_ptr(nullptr)
{
    if( x )
    {
        switch(x.get_ty())
        {
        case Ty::Allocation:
            m_ptr = x.m_ptr;
            assert(alloc().refcount != 0);
            assert(alloc().refcount != SIZE_MAX);
            alloc().refcount += 1;
            //LOG_DEBUG(&alloc() << " REF++ " << alloc().refcount);
            break;
        case Ty::Function: {
            auto ptr_i = reinterpret_cast<uintptr_t>(new ::HIR::Path(x.fcn()));
            assert( (ptr_i & 3) == 0 );
            m_ptr = reinterpret_cast<void*>( ptr_i + static_cast<uintptr_t>(Ty::Function) );
            assert(get_ty() == Ty::Function);
            } break;
        case Ty::Unused1:
            throw "BUG";
        case Ty::Unused2:
            throw "BUG";
        }
    }
    else
    {
        m_ptr = nullptr;
    }
}
AllocationPtr::~AllocationPtr()
{
    if( *this )
    {
        switch(get_ty())
        {
        case Ty::Allocation: {
            auto* ptr = &alloc();
            ptr->refcount -= 1;
            //LOG_DEBUG(&alloc() << " REF-- " << ptr->refcount);
            if(ptr->refcount == 0)
            {
                delete ptr;
            }
            } break;
        case Ty::Function: {
            auto* ptr = const_cast<::HIR::Path*>(&fcn());
            delete ptr;
            } break;
        case Ty::Unused1: {
            } break;
        case Ty::Unused2: {
            } break;
        }
    }
}

::std::ostream& operator<<(::std::ostream& os, const AllocationPtr& x)
{
    if( x )
    {
        switch(x.get_ty())
        {
        case AllocationPtr::Ty::Allocation:
            os << &x.alloc();
            break;
        case AllocationPtr::Ty::Function:
            os << *const_cast<::HIR::Path*>(&x.fcn());
            break;
        case AllocationPtr::Ty::Unused1:
            break;
        case AllocationPtr::Ty::Unused2:
            break;
        }
    }
    else
    {
        os << "null";
    }
    return os;
}

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
    // Support inline data if the data will fit within the inline region (which is the size of the metadata)
    if( ty.get_size() <= sizeof(this->meta.direct_data.data) )
    {
        struct H
        {
            static bool has_pointer(const ::HIR::TypeRef& ty)
            {
                if( ty.wrappers.empty() || ::std::all_of(ty.wrappers.begin(), ty.wrappers.end(), [](const auto& x){ return x.type == TypeWrapper::Ty::Array; }) )
                {
                    // TODO: Function pointers should be _pointers_
                    if( ty.inner_type == RawType::Function )
                    {
                        return true;
                    }
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
            //LOG_TRACE("No pointers in " << ty << ", storing inline");
            this->meta.direct_data.size = static_cast<uint8_t>(size);
            this->meta.direct_data.mask[0] = 0;
            this->meta.direct_data.mask[1] = 0;
            return ;
        }
    }
#endif

    // Fallback: Make a new allocation
    //LOG_TRACE(" Creating allocation for " << ty);
    this->allocation = Allocation::new_alloc(size);
    this->meta.indirect_meta.offset = 0;
    this->meta.indirect_meta.size = size;
}
Value Value::new_fnptr(const ::HIR::Path& fn_path)
{
    Value   rv( ::HIR::TypeRef(::HIR::CoreType { RawType::Function }) );
    assert(rv.allocation);
    rv.allocation.alloc().relocations.push_back(Relocation { 0, AllocationPtr::new_fcn(fn_path) });
    rv.allocation.alloc().data.at(0) = 0;
    rv.allocation.alloc().mask.at(0) = 0xFF;    // TODO: Get pointer size and make that much valid instead of 8 bytes
    return rv;
}

void Value::check_bytes_valid(size_t ofs, size_t size) const
{
    if( this->allocation )
    {
        const auto& alloc = this->allocation.alloc();
        if( ofs >= this->meta.indirect_meta.size || ofs+size > this->meta.indirect_meta.size ) {
            ::std::cerr << "ERROR: OOB read" << ::std::endl;
            throw "ERROR";
        }
        ofs += this->meta.indirect_meta.offset;
        //assert(ofs + size < alloc.size());
        for(size_t i = ofs; i < ofs + size; i++)
        {
            if( !(alloc.mask[i/8] & (1 << i%8)) )
            {
                ::std::cerr << "ERROR: Invalid bytes in value" << ::std::endl;
                throw "ERROR";
            }
        }
    }
    else
    {
        if( size == 0 && this->meta.direct_data.size > 0 ) {
            return ;
        }
        if( ofs >= this->meta.direct_data.size ) {
            ::std::cerr << "ERROR: OOB read" << ::std::endl;
            throw "ERROR";
        }
        if( ofs+size > this->meta.direct_data.size ) {
            ::std::cerr << "ERROR: OOB read" << ::std::endl;
            throw "ERROR";
        }
        for(size_t i = ofs; i < ofs + size; i++)
        {
            if( !(this->meta.direct_data.mask[i/8] & (1 << i%8)) )
            {
                ::std::cerr << "ERROR: Invalid bytes in value" << ::std::endl;
                throw "ERROR";
            }
        }
    }
}
void Value::mark_bytes_valid(size_t ofs, size_t size)
{
    if( this->allocation )
    {
        auto& alloc = this->allocation.alloc();
        // TODO: Assert range.
        ofs += this->meta.indirect_meta.offset;
        assert( ofs+size <= alloc.mask.size() * 8 );
        for(size_t i = ofs; i < ofs + size; i++)
        {
            alloc.mask[i/8] |= (1 << i%8);
        }
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
    Value   rv;
    LOG_DEBUG("(" << ofs << ", " << size << ") - " << *this);
    check_bytes_valid(ofs, size);
    if( this->allocation )
    {
        const auto& alloc = this->allocation.alloc();
        // TODO: Determine if this can become an inline allocation.
        bool has_reloc = false;
        for(const auto& r : alloc.relocations)
        {
            if( this->meta.indirect_meta.offset+ofs <= r.slot_ofs && r.slot_ofs < this->meta.indirect_meta.offset + ofs + size )
            {
                has_reloc = true;
            }
        }
        if( has_reloc || size > sizeof(this->meta.direct_data.data) )
        {
            rv.allocation = Allocation::new_alloc(size);
            rv.meta.indirect_meta.offset = 0;
            rv.meta.indirect_meta.size = size;

            for(const auto& r : alloc.relocations)
            {
                if( this->meta.indirect_meta.offset+ofs <= r.slot_ofs && r.slot_ofs < this->meta.indirect_meta.offset + ofs + size )
                {
                    rv.allocation.alloc().relocations.push_back({ r.slot_ofs - (this->meta.indirect_meta.offset+ofs), r.backing_alloc });
                }
            }
        }
        else
        {
            rv.meta.direct_data.size = static_cast<uint8_t>(size);
        }
        rv.write_bytes(0, this->data_ptr() + ofs, size);
    }
    else
    {
        // Inline can become inline.
        rv.meta.direct_data.size = static_cast<uint8_t>(size);
        rv.write_bytes(0, this->meta.direct_data.data+ofs, size);
    }
    LOG_DEBUG("RETURN " << rv);
    return rv;
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
        if(ofs >= this->meta.indirect_meta.size ) {
            ::std::cerr << "Value::write_bytes - Out of bounds write, " << ofs << " >= " << this->meta.indirect_meta.size << ::std::endl;
            throw "ERROR";
        }
        if(count > this->meta.indirect_meta.size ) {
            ::std::cerr << "Value::write_bytes - Out of bounds write, count " << count << " > size " << this->meta.indirect_meta.size << ::std::endl;
            throw "ERROR";
        }
        if(ofs+count > this->meta.indirect_meta.size ) {
            ::std::cerr << "Value::write_bytes - Out of bounds write, " << ofs << "+" << count << " > size " << this->meta.indirect_meta.size << ::std::endl;
            throw "ERROR";
        }


        // - Remove any relocations already within this region
        auto& this_relocs = this->allocation.alloc().relocations;
        for(auto it = this_relocs.begin(); it != this_relocs.end(); )
        {
            if( this->meta.indirect_meta.offset + ofs <= it->slot_ofs && it->slot_ofs < this->meta.indirect_meta.offset + ofs + count)
            {
                LOG_TRACE("Delete " << it->backing_alloc);
                it = this_relocs.erase(it);
            }
            else 
            {
                ++it;
            }
        }

    }
    else
    {
        if(ofs >= this->meta.direct_data.size )
            throw "ERROR";
        if(count > this->meta.direct_data.size )
            throw "ERROR";
        if(ofs+count > this->meta.direct_data.size )
            throw "ERROR";
    }
    ::std::memcpy(this->data_ptr() + ofs, src, count);
    mark_bytes_valid(ofs, count);
}
void Value::write_value(size_t ofs, Value v)
{
    if( v.allocation )
    {
        size_t  v_size = v.meta.indirect_meta.size;
        v.check_bytes_valid(0, v_size);
        const auto& src_alloc = v.allocation.alloc();
        write_bytes(ofs, v.data_ptr(), v_size);
        // Find any relocations that apply and copy those in.
        // - Any relocations in the source within `v.meta.indirect_meta.offset` .. `v.meta.indirect_meta.offset + v_size`
        ::std::vector<Relocation>   new_relocs;
        for(const auto& r : src_alloc.relocations)
        {
            // TODO: Negative offsets in destination?
            if( v.meta.indirect_meta.offset <= r.slot_ofs && r.slot_ofs < v.meta.indirect_meta.offset + v_size )
            {
                LOG_TRACE("Copy " << r.backing_alloc);
                // Applicable, save for later
                new_relocs.push_back( r );
            }
        }
        if( !new_relocs.empty() )
        {
            if( !this->allocation ) {
                throw ::std::runtime_error("TODO: Writing value with a relocation into a slot without a relocation");
            }
            // 1. Remove any relocations already within this region
            auto& this_relocs = this->allocation.alloc().relocations;
            for(auto it = this_relocs.begin(); it != this_relocs.end(); )
            {
                if( this->meta.indirect_meta.offset + ofs <= it->slot_ofs && it->slot_ofs < this->meta.indirect_meta.offset + ofs + v_size)
                {
                    LOG_TRACE("Delete " << it->backing_alloc);
                    it = this_relocs.erase(it);
                }
                else 
                {
                    ++it;
                }
            }
            // 2. Move the new relocations into this allocation
            for(auto& r : new_relocs)
            {
                LOG_TRACE("Insert " << r.backing_alloc);
                r.slot_ofs -= v.meta.indirect_meta.offset;
                r.slot_ofs += this->meta.indirect_meta.offset + ofs;
                this_relocs.push_back( ::std::move(r) );
            }
        }
    }
    else
    {
        v.check_bytes_valid(0, v.meta.direct_data.size);
        write_bytes(ofs, v.meta.direct_data.data, v.meta.direct_data.size);
    }
}

uint64_t Value::read_usize(size_t ofs) const
{
    uint64_t    v = 0;
    // TODO: Handle different pointer sizes
    this->read_bytes(0, &v, 8);
    return v;
}

::std::ostream& operator<<(::std::ostream& os, const Value& v)
{
    auto flags = os.flags();
    os << ::std::hex;
    if( v.allocation )
    {
        const auto& alloc = v.allocation.alloc();
        for(size_t i = 0; i < v.meta.indirect_meta.size; i++)
        {
            if( i != 0 )
                os << " ";
            size_t j = i + v.meta.indirect_meta.offset;

            if( alloc.mask[j/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)alloc.data_ptr()[j];
            }
            else
            {
                os << "--";
            }
        }

        os << " {";
        for(const auto& r : alloc.relocations)
        {
            if( v.meta.indirect_meta.offset <= r.slot_ofs && r.slot_ofs < v.meta.indirect_meta.offset + v.meta.indirect_meta.size )
            {
                os << " @" << (r.slot_ofs - v.meta.indirect_meta.offset) << "=" << r.backing_alloc;
            }
        }
        os << " }";
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