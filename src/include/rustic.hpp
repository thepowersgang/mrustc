/*
 */
#pragma once

template<typename T>
class slice
{
    T*  m_first;
    unsigned int    m_len;
public:
    slice(::std::vector<T>& v):
        m_first(&v[0]),
        m_len(v.size())
    {}
    
    ::std::vector<T> to_vec() const {
        return ::std::vector<T>(begin(), end());
    }
    
    unsigned int size() const {
        return m_len;
    }
    T& operator[](unsigned int i) const {
        assert(i < m_len);
        return m_first[i];
    }
    
    T* begin() const { return m_first; }
    T* end() const { return m_first + m_len; }
};

template<typename T>
::std::ostream& operator<<(::std::ostream& os, slice<T> s) {
    if( s.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : s )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i;
        }
    }
    return os;
}

namespace rust {

template<typename T>
class option
{
    char    m_data[ sizeof(T) ];
    bool    m_set;
public:
    option(T ent):
        m_set(true)
    {
        new (m_data) T(::std::move(ent));
    }
    option():
        m_set(false)
    {}
    ~option() {
        if( m_set ) {
            reinterpret_cast<T*>(m_data)->~T();
        }
    }
    
    bool is_none() const { return !m_set; }
    bool is_some() const { return m_set; }
    
    const T& unwrap() const {
        assert(is_some());
        return *reinterpret_cast<const T*>(m_data);
    }
    
    void if_set(::std::function<void (const T&)> f) const {
        if( m_set ) {
            return f(m_data);
        }
    }
    //template<typename U/*, class FcnSome, class FcnNone*/>
    //U match(::std::function<U(const T&)> if_some, ::Std::function<U()> if_none) const {
    //    if( m_set ) {
    //        return if_some(m_data);
    //    }
    //    else {
    //        return if_none();
    //    }
    //}
};
template<typename T>
class option<T&>
{
    T* m_ptr;
public:
    option(T& ent):
        m_ptr(&ent)
    {}
    option():
        m_ptr(nullptr)
    {}
    
    bool is_none() const { return m_ptr == nullptr; }
    bool is_some() const { return m_ptr != nullptr; }
    T& unwrap() const {
        assert(is_some());
        return *m_ptr;
    }
    void if_set(::std::function<void (const T&)> f) const {
        if( m_ptr ) {
            return f(*m_ptr);
        }
    }
};
template<typename T>
option<T> Some(T data) {
    return option<T>( ::std::move(data) );
}
template<typename T>
option<T> None() {
    return option<T>( );
}

#define IF_OPTION_SOME(bind, var, ...)    { auto __v = (var); if( var.is_some() ) { auto bind = __v.unwrap(); (void)&bind; __VA_ARGS__ } }

};
