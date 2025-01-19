#pragma once

#include "debug_windows.h"
#include <unordered_map>

struct TypeDefinition;
struct TypeRef
{
private:
    typedef std::unordered_map<DWORD, TypeRef>  t_cache;
    static t_cache  s_cache;
public:
    struct Wrapper {
        unsigned    count;
        DWORD   inner_ty_idx;
        Wrapper(unsigned c): count(c) {}

        static Wrapper new_pointer() {
            return Wrapper(~0u);
        }
        bool is_pointer() const { return count == ~0u; }
    };
    struct TypeNotLoaded: public ::std::exception {
        std::string name;
        std::string msg;
        TypeNotLoaded(const std::string& name);
        const char* what() const noexcept { return msg.c_str(); }
    };

    std::vector<Wrapper>    wrappers;
    enum {
        ClassBasic,
        ClassUdt,
        ClassMisc,
    } m_class;
    union {
        struct {
            BasicType   bt;
            uint8_t size;
        } basic;
        struct {
            const char* name;
            uint8_t size;
        } misc;
        TypeDefinition* udt;
    } m_data;

    static TypeRef lookup(HANDLE hProcess, ULONG64 mod_base, DWORD type_id);
    static TypeRef lookup_by_name(const std::string& name);

    size_t size() const;

    bool is_any_basic() const { return wrappers.empty() && m_class == ClassBasic; }
    bool is_basic(BasicType bt) const { return wrappers.empty() && m_class == ClassBasic && m_data.basic.bt == bt; }
    const TypeDefinition* any_udt() const { return (wrappers.empty() && m_class == ClassUdt) ? m_data.udt : nullptr; }
    bool is_udt_ptr(const char* name) const;
    bool is_udt(const char* name) const;
    bool is_udt_suffix(const char* name) const;

    TypeRef get_field(std::initializer_list<const char*> fields, size_t* ofs=nullptr) const;
    size_t get_field_ofs(std::initializer_list<const char*> fields, TypeRef* out_ty=nullptr) const
    {
        size_t rv;
        auto ty = get_field(fields, &rv);
        if(out_ty)  *out_ty = ty;
        return rv;
    }
    TypeRef deref() const;
    void fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level=0) const;

    friend std::ostream& operator<<(std::ostream& os, const TypeRef& x) {
        x.fmt(os, 0);
        return os;
    }
};
struct TypeDefinition
{
    struct Field {
        std::string name;
        size_t  offset;
        TypeRef ty;
    };

    std::string name;
    size_t  size;
    std::vector<Field>  fields;

    void fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level=0) const;

    static TypeDefinition* from_syminfo(HANDLE hProcess, ULONG64 mod_base, DWORD type_id);
};