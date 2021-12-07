/**
 * Vector-backed map that supports range lookups using different keys
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

template <typename K, typename V, typename Cmp=std::less<K>>
class RangeVecMap
{
public:
    typedef std::pair<K,V>  item_t;
private:
    typedef std::vector<::std::unique_ptr<item_t>>  inner_t;
    inner_t m_data;
    Cmp m_cmp;
public:
    RangeVecMap() {}

    class iterator {
        friend class RangeVecMap<K,V,Cmp>;
        typename inner_t::iterator    m_inner;

        iterator(typename inner_t::iterator i): m_inner(i) {}
    public:
        item_t& operator*() { return **m_inner; }
        item_t* operator->() { return &**m_inner; }
        iterator& operator++() { ++m_inner; return *this; }
        iterator operator+(size_t i) const { return iterator(m_inner + i); }
        bool operator==(const iterator& x) const { return m_inner == x.m_inner; }
        bool operator!=(const iterator& x) const { return m_inner != x.m_inner; }
        ptrdiff_t operator-(const iterator& x) const { return m_inner - x.m_inner; }
    };
    class const_iterator {
        friend class RangeVecMap<K,V,Cmp>;
        typename inner_t::const_iterator    m_inner;

        const_iterator(typename inner_t::const_iterator i): m_inner(i) {}
    public:
        const item_t& operator*() { return **m_inner; }
        const item_t* operator->() { return &**m_inner; }
        const_iterator& operator++() { ++m_inner; return *this; }
        const_iterator operator+(size_t i) const { return const_iterator(m_inner + i); }
        bool operator==(const const_iterator& x) const { return m_inner == x.m_inner; }
        bool operator!=(const const_iterator& x) const { return m_inner != x.m_inner; }
        ptrdiff_t operator-(const const_iterator& x) const { return m_inner - x.m_inner; }
    };

    size_t size() const { return m_data.size(); }

    iterator begin()       { return iterator(m_data.begin()); }
    const_iterator begin() const { return const_iterator(m_data.begin()); }
    iterator end()       { return iterator(m_data.end()); }
    const_iterator end() const { return const_iterator(m_data.end()); }

    template<typename K2> iterator lower_bound(const K2& k) {
        return iterator(std::lower_bound(m_data.begin(), m_data.end(), k, [&](const ::std::unique_ptr<item_t>& kv, const K2& k){ return m_cmp(kv->first, k); }));
    }
    template<typename K2> iterator upper_bound(const K2& k) {
        return iterator(std::upper_bound(m_data.begin(), m_data.end(), k, [&](const K2& k, const ::std::unique_ptr<item_t>& kv){ return m_cmp(k, kv->first); }));
    }
    template<typename K2> std::pair<iterator,iterator> equal_range(const K2& k) {
        return std::make_pair(lower_bound(k), upper_bound(k));
    }

    /// Lower bound: First item in the map not less than the provided key (equal, or first after)
    template<typename K2> const_iterator lower_bound(const K2& k) const {
        return const_iterator(std::lower_bound(m_data.begin(), m_data.end(), k, [&](const ::std::unique_ptr<item_t>& kv, const K2& k){ return m_cmp(kv->first, k); }));
    }
    /// Upper bound: First item in the map after the provided key
    template<typename K2> const_iterator upper_bound(const K2& k) const {
        return const_iterator(std::upper_bound(m_data.begin(), m_data.end(), k, [&](const K2& k, const ::std::unique_ptr<item_t>& kv){ return m_cmp(k, kv->first); }));
    }
    /// Iterator pair of first and after-last items equal to the given key
    template<typename K2> std::pair<const_iterator,const_iterator> equal_range(const K2& k) const {
        return std::make_pair(lower_bound(k), upper_bound(k));
    }

    template<typename K2> iterator       find(const K2& k)       { auto v = equal_range(k); if( v.first == v.second ) return end(); return v.first; }
    template<typename K2> const_iterator find(const K2& k) const { auto v = equal_range(k); if( v.first == v.second ) return end(); return v.first; }


    std::pair<iterator,bool> insert(item_t kv) {
        auto its = this->equal_range(kv.first);
        if(its.first == its.second) {
            size_t i = its.first.m_inner - m_data.begin();
            m_data.insert(its.first.m_inner, std::make_unique<item_t>(std::move(kv)));
            return std::make_pair(iterator(m_data.begin() + i), true);
        }
        else {
            assert(its.first + 1 == its.second);
            return std::make_pair(its.first, false);
        }
    }
    V& operator[](K k) {
        auto its = equal_range(k);
        if(its.first == its.second) {
            size_t i = its.first.m_inner - m_data.begin();
            m_data.insert(its.first.m_inner, std::make_unique<item_t>(std::make_pair(std::move(k), V())));
            return m_data[i]->second;
        }
        else {
            assert(its.first.m_inner + 1 == its.second.m_inner);
            return its.first->second;
        }
    }
    void clear() {
        m_data.clear();
    }
};
