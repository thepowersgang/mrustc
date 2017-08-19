/*
 */
#include "manifest.h"
#include <vector>
#include <algorithm>

struct BuildList
{
    struct BuildEnt {
        const PackageManifest*  package;
        unsigned level;
    };
    ::std::vector<BuildEnt>  m_list;

    void add_package(const PackageManifest& p, unsigned level);
    void sort_list();

    struct Iter {
        const BuildList& l;
        size_t  i;

        const PackageManifest& operator*() const {
            return *this->l.m_list[this->l.m_list.size() - this->i - 1].package;
        }
        void operator++() {
            this->i++;
        }
        bool operator!=(const Iter& x) const {
            return this->i != x.i;
        }
        Iter begin() const {
            return *this;
        }
        Iter end() {
            return Iter{ this->l, this->l.m_list.size() };
        }
    };

    Iter iter() const {
        return Iter { *this, 0 };
    }
};

void MiniCargo_Build(const PackageManifest& manifest)
{
    BuildList   list;
    // Generate sorted dependency list
    for (const auto& dep : manifest.dependencies())
    {
        list.add_package(dep.get_package(), 1);
    }


    // Build dependencies
    for(const auto& p : list.iter())
    {
        if( ! p.build_lib() )
            return ;
    }

    if( ! manifest.build_lib() )
        return ;
    // TODO: If the manifest doesn't have a library, build the binary
}

void BuildList::add_package(const PackageManifest& p, unsigned level)
{
    for(auto& ent : m_list)
    {
        if(ent.package == &p)
        {
            ent.level = level;
            return ;
        }
    }
    m_list.push_back({ &p, level });
    for (const auto& dep : p.dependencies())
    {
        add_package(dep.get_package(), level+1);
    }
}
void BuildList::sort_list()
{
    ::std::sort(m_list.begin(), m_list.end(), [](const auto& a, const auto& b){ return a.level < b.level; });
}
