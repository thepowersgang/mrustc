/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/ident.hpp
 * - Identifiers with hygine
 */
#pragma once
#include <vector>
#include <string>

struct Ident
{
    struct Hygine
    {
        unsigned int file_num;
        ::std::vector<unsigned int> indexes;
        
        Hygine(unsigned int file, ::std::vector<unsigned int> indexes):
            file_num(file),
            indexes(::std::move(indexes))
        {}
        
        Hygine(Hygine&& x) = default;
        Hygine(const Hygine& x) = default;
        Hygine& operator=(Hygine&& x) = default;
        Hygine& operator=(const Hygine& x) = default;
        
        // Returns true if an ident with hygine `souce` can see an ident with this hygine
        bool is_visible(const Hygine& source) const;
        bool operator==(const Hygine& x) const { return file_num == x.file_num && indexes == x.indexes; }
        bool operator!=(const Hygine& x) const { return file_num != x.file_num || indexes != x.indexes; }
        
        friend ::std::ostream& operator<<(::std::ostream& os, const Hygine& v);
    };
    
    Hygine  hygine;
    ::std::string   name;
    
    Ident(const char* name):
        hygine(~0u, {}),
        name(name)
    { }
    Ident(::std::string name):
        hygine(~0u, {}),
        name(::std::move(name))
    { }
    Ident(Hygine hygine, ::std::string name):
        hygine(::std::move(hygine)), name(::std::move(name))
    { }
    
    Ident(Ident&& x) = default;
    Ident(const Ident& x) = default;
    Ident& operator=(Ident&& x) = default;
    Ident& operator=(const Ident& x) = default;
    
    ::std::string into_string() {
        return ::std::move(name);
    }
    
    bool operator==(const char* s) const {
        return this->name == s;
    }
    
    bool operator==(const Ident& x) const {
        if( this->name != x.name )
            return false;
        //if( this->hygine.indexes != x.hygine.indexes )
        //    return false;
        return true;
    }
    bool operator!=(const Ident& x) const {
        return !(*this == x);
    }
    bool operator<(const Ident& x) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const Ident& x);
};
