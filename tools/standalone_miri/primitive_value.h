/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * primitive_value.h
 * - Generic primitive values (integers)
 */
#pragma once
#include "debug.hpp"
#include "value.hpp"

enum class OverflowType {
    None,   // Result was within range
    Max,    // Result overflowed the maximum value
    Min,    // Result overflowed the minimum value
};
class PrimitiveValue
{
public:
    virtual ~PrimitiveValue() {}

    virtual bool is_zero() const = 0;
    virtual bool is_negative() const = 0;
    virtual void add_imm(int64_t v) = 0;
    virtual OverflowType add(const PrimitiveValue& v) = 0;
    virtual OverflowType subtract(const PrimitiveValue& v) = 0;
    virtual OverflowType multiply(const PrimitiveValue& v) = 0;
    virtual bool divide(const PrimitiveValue& v) = 0;
    virtual bool modulo(const PrimitiveValue& v) = 0;
    virtual void write_to_value(ValueCommonWrite& tgt, size_t ofs) const = 0;

    virtual U128 as_u128() const = 0;

    template<typename T>
    const T& check(const char* opname) const
    {
        const auto* xp = dynamic_cast<const T*>(this);
        LOG_ASSERT(xp, "Attempting to " << opname << " mismatched types, expected " << typeid(T).name() << " got " << typeid(*this).name());
        return *xp;
    }
};
template<typename T>
struct PrimitiveUInt:
    public PrimitiveValue
{
    typedef PrimitiveUInt<T>    Self;
    T   v;

    PrimitiveUInt(T v): v(v) {}
    ~PrimitiveUInt() override {}

    bool is_zero() const override {
        return this->v == 0;
    }
    bool is_negative() const override {
        return false;
    }
    void add_imm(int64_t imm) override {
        this->v += static_cast<T>(imm);
    }
    OverflowType add(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("add");
        T newv = this->v + xp->v;
        bool did_overflow = newv < this->v;
        this->v = newv;
        return did_overflow ? OverflowType::Max : OverflowType::None;
    }
    OverflowType subtract(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("subtract");
        T newv = this->v - xp->v;
        bool did_overflow = newv > this->v;
        this->v = newv;
        return did_overflow ? OverflowType::Min : OverflowType::None;
    }
    OverflowType multiply(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("multiply");
        T newv = this->v * xp->v;
        bool did_overflow = newv < this->v && newv < xp->v;
        this->v = newv;
        return did_overflow ? OverflowType::Max : OverflowType::None;
    }
    bool divide(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("divide");
        if(xp->v == 0)  return false;
        T newv = this->v / xp->v;
        this->v = newv;
        return true;
    }
    bool modulo(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("modulo");
        if(xp->v == 0)  return false;
        T newv = this->v % xp->v;
        this->v = newv;
        return true;
    }

    U128 as_u128() const override {
        return U128(static_cast<uint64_t>(this->v));
    }
};
struct PrimitiveU64: public PrimitiveUInt<uint64_t>
{
    PrimitiveU64(uint64_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u64(ofs, this->v);
    }
};
struct PrimitiveU32: public PrimitiveUInt<uint32_t>
{
    PrimitiveU32(uint32_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u32(ofs, this->v);
    }
};
struct PrimitiveU16: public PrimitiveUInt<uint16_t>
{
    PrimitiveU16(uint16_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u16(ofs, this->v);
    }
};
struct PrimitiveU8: public PrimitiveUInt<uint8_t>
{
    PrimitiveU8(uint8_t v): PrimitiveUInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_u8(ofs, this->v);
    }
};
template<typename T>
struct PrimitiveSInt:
    public PrimitiveValue
{
    typedef PrimitiveSInt<T>    Self;
    T   v;

    PrimitiveSInt(T v): v(v) {}
    ~PrimitiveSInt() override {}

    bool is_zero() const override {
        return this->v == 0;
    }
    bool is_negative() const override {
        return this->v < 0;
    }
    void add_imm(int64_t imm) override {
        this->v += static_cast<T>(imm);
    }
    // TODO: Make this correct.
    OverflowType add(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("add");
        T newv = this->v + xp->v;
        bool did_overflow = newv < this->v;
        this->v = newv;
        return did_overflow ? (this->v < 0 ? OverflowType::Max : OverflowType::Min) : OverflowType::None;
    }
    OverflowType subtract(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("subtract");
        T newv = this->v - xp->v;
        bool did_overflow = newv > this->v;
        this->v = newv;
        return did_overflow ? (this->v < 0 ? OverflowType::Max : OverflowType::Min) : OverflowType::None;
    }
    OverflowType multiply(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("multiply");
        T newv = this->v * xp->v;
        bool did_overflow = newv < this->v && newv < xp->v;
        this->v = newv;
        return did_overflow ? (this->v < 0 ? OverflowType::Max : OverflowType::Min) : OverflowType::None;
    }
    bool divide(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("divide");
        if(xp->v == 0)  return false;
        // TODO: INT_MIN * -1 == overflow
        T newv = this->v / xp->v;
        this->v = newv;
        return true;
    }
    bool modulo(const PrimitiveValue& x) override {
        const auto* xp = &x.check<Self>("modulo");
        if(xp->v == 0)  return false;
        T newv = this->v % xp->v;
        this->v = newv;
        return true;
    }

    U128 as_u128() const override {
        return U128(static_cast<uint64_t>(this->v));
    }
};
struct PrimitiveI64: public PrimitiveSInt<int64_t>
{
    PrimitiveI64(int64_t v): PrimitiveSInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_i64(ofs, this->v);
    }
};
struct PrimitiveI32: public PrimitiveSInt<int32_t>
{
    PrimitiveI32(int32_t v): PrimitiveSInt(v) {}
    void write_to_value(ValueCommonWrite& tgt, size_t ofs) const override {
        tgt.write_i32(ofs, this->v);
    }
};

// Type-erased primitive value
class PrimitiveValueVirt
{
    union {
        uint64_t    u64[3]; // Allows i128 plus a vtable pointer
        uint8_t     u8[3*8];
    } buf;
    PrimitiveValueVirt() {}
public:
    // HACK: No copy/move constructors, assumes that contained data is always POD
    ~PrimitiveValueVirt() {
        reinterpret_cast<PrimitiveValue*>(&this->buf)->~PrimitiveValue();
    }
    PrimitiveValue& get() { return *reinterpret_cast<PrimitiveValue*>(&this->buf.u8); }
    const PrimitiveValue& get() const { return *reinterpret_cast<const PrimitiveValue*>(&this->buf.u8); }

    static PrimitiveValueVirt from_value(const ::HIR::TypeRef& t, const ValueRef& v) {
        PrimitiveValueVirt  rv;
        LOG_ASSERT(t.get_wrapper() == nullptr, "PrimitiveValueVirt::from_value: " << t);
        switch(t.inner_type)
        {
        case RawType::U8:
            new(&rv.buf) PrimitiveU8(v.read_u8(0));
            break;
        case RawType::U16:
            new(&rv.buf) PrimitiveU16(v.read_u16(0));
            break;
        case RawType::U32:
            new(&rv.buf) PrimitiveU32(v.read_u32(0));
            break;
        case RawType::U64:
            new(&rv.buf) PrimitiveU64(v.read_u64(0));
            break;
        case RawType::USize:
            if( POINTER_SIZE == 8 )
                new(&rv.buf) PrimitiveU64(v.read_u64(0));
            else
                new(&rv.buf) PrimitiveU32(v.read_u32(0));
            break;

        case RawType::I32:
            new(&rv.buf) PrimitiveI32(v.read_i32(0));
            break;
        case RawType::I64:
            new(&rv.buf) PrimitiveI64(v.read_i64(0));
            break;
        case RawType::ISize:
            if( POINTER_SIZE == 8 )
                new(&rv.buf) PrimitiveI64(v.read_i64(0));
            else
                new(&rv.buf) PrimitiveI32(v.read_i32(0));
            break;
        default:
            LOG_TODO("PrimitiveValueVirt::from_value: " << t);
        }
        return rv;
    }
};
