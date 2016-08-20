
#pragma once

#include <string>
#include <serialise.hpp>

namespace AST {

template <typename T>
struct NamedNS
{
    ::std::string   name;
    T   data;
    bool    is_pub;
    
    NamedNS():
        is_pub(false)
    {}
    NamedNS(NamedNS&&) = default;
    NamedNS(const NamedNS&) = default;
    NamedNS(::std::string name, T data, bool is_pub):
        name( ::std::move(name) ),
        data( ::std::move(data) ),
        is_pub( is_pub )
    {
    }
    
    //friend ::std::ostream& operator<<(::std::ostream& os, const Named& i) {
    //    return os << (i.is_pub ? "pub " : " ") << i.name << ": " << i.data;
    //}
};

template <typename T>
struct Named:
    public NamedNS<T>
{
    Named():
        NamedNS<T>()
    {}
    Named(Named&&) = default;
    Named(const Named&) = default;
    Named(::std::string name, T data, bool is_pub):
        NamedNS<T>( ::std::move(name), ::std::move(data), is_pub )
    {}
};

template <typename T>
using NamedList = ::std::vector<Named<T> >;

}   // namespace AST

