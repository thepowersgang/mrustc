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
AllocationPtr AllocationPtr::new_string(const ::std::string* ptr)
{
    AllocationPtr   rv;
    rv.m_ptr = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>(ptr) + static_cast<uintptr_t>(Ty::StdString) );
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
        case Ty::StdString:
            // No ownership semantics, just clone the pointer
            m_ptr = x.m_ptr;
            break;
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
        case Ty::StdString: {
            // No ownership semantics
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
            os << x.fcn();
            break;
        case AllocationPtr::Ty::StdString:
            os << "\"" << x.str() << "\"";
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


void Allocation::resize(size_t new_size)
{
    //size_t old_size = this->size();
    //size_t extra_bytes = (new_size > old_size ? new_size - old_size : 0);

    this->data.resize( (new_size + 8-1) / 8 );
    this->mask.resize( (new_size + 8-1) / 8 );
}

void Allocation::check_bytes_valid(size_t ofs, size_t size) const
{
    if( !(ofs + size <= this->size()) ) {
        LOG_FATAL("Out of range - " << ofs << "+" << size << " > " << this->size());
    }
    for(size_t i = ofs; i < ofs + size; i++)
    {
        if( !(this->mask[i/8] & (1 << i%8)) )
        {
            ::std::cerr << "ERROR: Invalid bytes in value" << ::std::endl;
            throw "ERROR";
        }
    }
}
void Allocation::mark_bytes_valid(size_t ofs, size_t size)
{
    assert( ofs+size <= this->mask.size() * 8 );
    for(size_t i = ofs; i < ofs + size; i++)
    {
        this->mask[i/8] |= (1 << i%8);
    }
}
Value Allocation::read_value(size_t ofs, size_t size) const
{
    Value rv;
    // TODO: Determine if this can become an inline allocation.
    bool has_reloc = false;
    for(const auto& r : this->relocations)
    {
        if( ofs <= r.slot_ofs && r.slot_ofs < ofs + size )
        {
            has_reloc = true;
        }
    }
    if( has_reloc || size > sizeof(rv.direct_data.data) )
    {
        rv.allocation = Allocation::new_alloc(size);

        rv.write_bytes(0, this->data_ptr() + ofs, size);

        for(const auto& r : this->relocations)
        {
            if( ofs <= r.slot_ofs && r.slot_ofs < ofs + size )
            {
                rv.allocation.alloc().relocations.push_back({ r.slot_ofs - ofs, r.backing_alloc });
            }
        }
    }
    else
    {
        rv.direct_data.size = static_cast<uint8_t>(size);
        rv.write_bytes(0, this->data_ptr() + ofs, size);
    }
    return rv;
}
void Allocation::read_bytes(size_t ofs, void* dst, size_t count) const
{
    if(count == 0)
        return ;

    if(ofs >= this->size() ) {
        LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    if(count > this->size() ) {
        LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    if(ofs+count > this->size() ) {
        LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    check_bytes_valid(ofs, count);


    ::std::memcpy(dst, this->data_ptr() + ofs, count);
}
void Allocation::write_value(size_t ofs, Value v)
{
    if( v.allocation )
    {
        size_t  v_size = v.allocation.alloc().size();
        const auto& src_alloc = v.allocation.alloc();
        // Take a copy of the source mask
        auto s_mask = src_alloc.mask;

        // Save relocations first, because `Foo = Foo` is valid.
        ::std::vector<Relocation>   new_relocs = src_alloc.relocations;
        // - write_bytes removes any relocations in this region.
        write_bytes(ofs, src_alloc.data_ptr(), v_size);

        // Find any relocations that apply and copy those in.
        // - Any relocations in the source within `v.meta.indirect_meta.offset` .. `v.meta.indirect_meta.offset + v_size`
        if( !new_relocs.empty() )
        {
            // 2. Move the new relocations into this allocation
            for(auto& r : new_relocs)
            {
                LOG_TRACE("Insert " << r.backing_alloc);
                r.slot_ofs += ofs;
                this->relocations.push_back( ::std::move(r) );
            }
        }

        // Set mask in destination
        if( ofs % 8 != 0 || v_size % 8 != 0 )
        {
            // Lazy way, sets/clears individual bits
            for(size_t i = 0; i < v_size; i ++)
            {
                uint8_t dbit = 1 << ((ofs+i) % 8);
                if( s_mask[i/8] & (1 << (i %8)) )
                    this->mask[ (ofs+i) / 8 ] |= dbit;
                else
                    this->mask[ (ofs+i) / 8 ] &= ~dbit;
            }
        }
        else
        {
            // Copy the mask bytes directly
            for(size_t i = 0; i < v_size / 8; i ++)
            {
                this->mask[ofs/8+i] = s_mask[i];
            }
        }
    }
    else
    {
        this->write_bytes(ofs, v.direct_data.data, v.direct_data.size);

        // Lazy way, sets/clears individual bits
        for(size_t i = 0; i < v.direct_data.size; i ++)
        {
            uint8_t dbit = 1 << ((ofs+i) % 8);
            if( v.direct_data.mask[i/8] & (1 << (i %8)) )
                this->mask[ (ofs+i) / 8 ] |= dbit;
            else
                this->mask[ (ofs+i) / 8 ] &= ~dbit;
        }
    }
}
void Allocation::write_bytes(size_t ofs, const void* src, size_t count)
{
    if(count == 0)
        return ;
    if(ofs >= this->size() ) {
        LOG_ERROR("Out of bounds write, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    if(count > this->size() ) {
        LOG_ERROR("Out of bounds write, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }
    if(ofs+count > this->size() ) {
        LOG_ERROR("Out of bounds write, " << ofs << "+" << count << " > " << this->size());
        throw "ERROR";
    }


    // - Remove any relocations already within this region
    auto& this_relocs = this->relocations;
    for(auto it = this_relocs.begin(); it != this_relocs.end(); )
    {
        if( ofs <= it->slot_ofs && it->slot_ofs < ofs + count)
        {
            LOG_TRACE("Delete " << it->backing_alloc);
            it = this_relocs.erase(it);
        }
        else 
        {
            ++it;
        }
    }

    ::std::memcpy(this->data_ptr() + ofs, src, count);
    mark_bytes_valid(ofs, count);
}
void Allocation::write_usize(size_t ofs, uint64_t v)
{
    this->write_bytes(ofs, &v, POINTER_SIZE);
}
::std::ostream& operator<<(::std::ostream& os, const Allocation& x)
{
    for(size_t i = 0; i < x.size(); i++)
    {
        if( i != 0 )
            os << " ";

        if( x.mask[i/8] & (1 << i%8) )
        {
            os << ::std::setw(2) << ::std::setfill('0') << (int)x.data_ptr()[i];
        }
        else
        {
            os << "--";
        }
    }

    os << " {";
    for(const auto& r : x.relocations)
    {
        if( 0 <= r.slot_ofs && r.slot_ofs < x.size() )
        {
            os << " @" << r.slot_ofs << "=" << r.backing_alloc;
        }
    }
    os << " }";
    return os;
}

Value::Value()
{
    this->direct_data.size = 0;
    this->direct_data.mask[0] = 0;
    this->direct_data.mask[1] = 0;
}
Value::Value(::HIR::TypeRef ty)
{
    size_t size = ty.get_size();
#if 1
    // Support inline data if the data will fit within the inline region (which is the size of the metadata)
    if( ty.get_size() <= sizeof(this->direct_data.data) )
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
            this->direct_data.size = static_cast<uint8_t>(size);
            this->direct_data.mask[0] = 0;
            this->direct_data.mask[1] = 0;
            return ;
        }
    }
#endif

    // Fallback: Make a new allocation
    //LOG_TRACE(" Creating allocation for " << ty);
    this->allocation = Allocation::new_alloc(size);
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

void Value::create_allocation()
{
    assert(!this->allocation);
    this->allocation = Allocation::new_alloc(this->direct_data.size);
    this->allocation.alloc().mask[0] = this->direct_data.mask[0];
    this->allocation.alloc().mask[1] = this->direct_data.mask[1];
    ::std::memcpy(this->allocation.alloc().data.data(), this->direct_data.data, this->direct_data.size);
}
void Value::check_bytes_valid(size_t ofs, size_t size) const
{
    if( size == 0 )
        return ;
    if( this->allocation )
    {
        this->allocation.alloc().check_bytes_valid(ofs, size);
    }
    else
    {
        if( size == 0 && this->direct_data.size > 0 ) {
            return ;
        }
        if( ofs >= this->direct_data.size ) {
            LOG_ERROR("Read out of bounds " << ofs << "+" << size << " > " << int(this->direct_data.size));
            throw "ERROR";
        }
        if( ofs+size > this->direct_data.size ) {
            LOG_ERROR("Read out of bounds " << ofs+size << " >= " << int(this->direct_data.size));
            throw "ERROR";
        }
        for(size_t i = ofs; i < ofs + size; i++)
        {
            if( !(this->direct_data.mask[i/8] & (1 << i%8)) )
            {
                LOG_ERROR("Accessing invalid bytes in value");
                throw "ERROR";
            }
        }
    }
}
void Value::mark_bytes_valid(size_t ofs, size_t size)
{
    if( this->allocation )
    {
        this->allocation.alloc().mark_bytes_valid(ofs, size);
    }
    else
    {
        for(size_t i = 0; i < this->direct_data.size; i++)
        {
            this->direct_data.mask[i/8] |= (1 << i%8);
        }
    }
}

Value Value::read_value(size_t ofs, size_t size) const
{
    Value   rv;
    //TRACE_FUNCTION_R(ofs << ", " << size << ") - " << *this, rv);
    if( this->allocation )
    {
        rv = this->allocation.alloc().read_value(ofs, size);
    }
    else
    {
        // Inline can become inline.
        rv.direct_data.size = static_cast<uint8_t>(size);
        rv.write_bytes(0, this->direct_data.data+ofs, size);
        rv.direct_data.mask[0] = this->direct_data.mask[0];
        rv.direct_data.mask[1] = this->direct_data.mask[1];
    }
    return rv;
}
void Value::read_bytes(size_t ofs, void* dst, size_t count) const
{
    if(count == 0)
        return ;
    if( this->allocation )
    {
        this->allocation.alloc().read_bytes(ofs, dst, count);
    }
    else
    {
        check_bytes_valid(ofs, count);

        if(ofs >= this->direct_data.size ) {
            LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
            throw "ERROR";
        }
        if(count > this->direct_data.size ) {
            LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
            throw "ERROR";
        }
        if(ofs+count > this->direct_data.size ) {
            LOG_ERROR("Out of bounds read, " << ofs << "+" << count << " > " << this->size());
            throw "ERROR";
        }
        ::std::memcpy(dst, this->direct_data.data + ofs, count);
    }
}

void Value::write_bytes(size_t ofs, const void* src, size_t count)
{
    if( count == 0 )
        return ;
    if( this->allocation )
    {
        this->allocation.alloc().write_bytes(ofs, src, count);
    }
    else
    {
        if(ofs >= this->direct_data.size ) {
            LOG_BUG("Write to offset outside value size (" << ofs << "+" << count << " >= " << (int)this->direct_data.size << ")");
        }
        if(count > this->direct_data.size ){
            LOG_BUG("Write larger than value size (" << ofs << "+" << count << " >= " << (int)this->direct_data.size << ")");
        }
        if(ofs+count > this->direct_data.size ) {
            LOG_BUG("Write extends outside value size (" << ofs << "+" << count << " >= " << (int)this->direct_data.size << ")");
        }
        ::std::memcpy(this->direct_data.data + ofs, src, count);
        mark_bytes_valid(ofs, count);
    }
}
void Value::write_value(size_t ofs, Value v)
{
    if( this->allocation )
    {
        this->allocation.alloc().write_value(ofs, ::std::move(v));
    }
    else
    {
        if( v.allocation && !v.allocation.alloc().relocations.empty() )
        {
            this->create_allocation();
            this->allocation.alloc().write_value(ofs, ::std::move(v));
        }
        else
        {
            v.check_bytes_valid(0, v.direct_data.size);
            write_bytes(ofs, v.direct_data.data, v.direct_data.size);
        }
    }
}
void Value::write_usize(size_t ofs, uint64_t v)
{
    this->write_bytes(ofs, &v, POINTER_SIZE);
}

::std::ostream& operator<<(::std::ostream& os, const Value& v)
{
    if( v.allocation )
    {
        os << v.allocation.alloc();
    }
    else
    {
        auto flags = os.flags();
        os << ::std::hex;
        for(size_t i = 0; i < v.direct_data.size; i++)
        {
            if( i != 0 )
                os << " ";
            if( v.direct_data.mask[i/8] & (1 << i%8) )
            {
                os << ::std::setw(2) << ::std::setfill('0') << (int)v.direct_data.data[i];
            }
            else
            {
                os << "--";
            }
        }
        os.setf(flags);
    }
    return os;
}
extern ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v)
{
    if( v.m_alloc )
    {
        os << v.m_alloc.alloc();
    }
    else
    {
        os << *v.m_value;
    }
    return os;
}

uint64_t ValueRef::read_usize(size_t ofs) const
{
    uint64_t    v = 0;
    this->read_bytes(0, &v, POINTER_SIZE);
    return v;
}
uint64_t Value::read_usize(size_t ofs) const
{
    uint64_t    v = 0;
    this->read_bytes(0, &v, POINTER_SIZE);
    return v;
}
uint64_t Allocation::read_usize(size_t ofs) const
{
    uint64_t    v = 0;
    this->read_bytes(0, &v, POINTER_SIZE);
    return v;
}