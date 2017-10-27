/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/item-path.hpp
 * - Printable path to an item in the HIR
 */
#pragma once

namespace HIR {

class ItemPath
{
public:
    const ItemPath* parent = nullptr;
    const ::HIR::TypeRef* ty = nullptr;
    const ::HIR::SimplePath* trait = nullptr;
    const ::HIR::PathParams* trait_params = nullptr;
    const char* name = nullptr;
    const char* crate_name = nullptr;

    ItemPath(const ::std::string& crate): crate_name(crate.c_str()) {}
    ItemPath(const ItemPath& p, const char* n):
        parent(&p),
        name(n)
    {}
    ItemPath(const ::HIR::TypeRef& type):
        ty(&type)
    {}
    ItemPath(const ::HIR::TypeRef& type, const ::HIR::SimplePath& path, const ::HIR::PathParams& params):
        ty(&type),
        trait(&path),
        trait_params(&params)
    {}
    ItemPath(const ::HIR::SimplePath& path):
        trait(&path)
    {}
    ItemPath(const ::HIR::ItemPath& other) :
        parent(other.parent),
        ty(other.ty),
        trait(other.trait),
        trait_params(other.trait_params),
        name(other.name),
        crate_name(other.crate_name)
    {}


    const ::HIR::SimplePath* trait_path() const { return trait; }
    const ::HIR::PathParams* trait_args() const { return trait_params; }

    ::HIR::SimplePath get_simple_path() const {
        if( parent ) {
            assert(name);
            return parent->get_simple_path() + name;
        }
        else {
            assert(!name);
            assert(crate_name);
            return ::HIR::SimplePath(crate_name);
        }
    }
    ::HIR::Path get_full_path() const {
        assert(parent);
        assert(name);

        // If the parent has a name, or the parent is the crate root.
        if( parent->name || !parent->ty ) {
            return get_simple_path();
        }
        else if( parent->trait ) {
            assert(parent->ty);
            assert(parent->trait_params);
            return ::HIR::Path( parent->ty->clone(), ::HIR::GenericPath(parent->trait->clone(), parent->trait_params->clone()), ::std::string(name) );
        }
        else {
            assert(parent->ty);
            return ::HIR::Path( parent->ty->clone(), ::std::string(name) );
        }
    }
    const char* get_name() const {
        return name ? name : "";
    }

    ItemPath operator+(const ::std::string& name) const {
        return ItemPath(*this, name.c_str());
    }

    bool operator==(const ::HIR::SimplePath& sp) const {
        if( sp.m_crate_name != "" )  return false;

        unsigned int i = sp.m_components.size();
        const auto* n = this;
        while( n && i -- )
        {
            if( !n->name )
                return false;
            if( n->name != sp.m_components[i] )
                return false;
            n = n->parent;
        }
        if( i > 0 || n->name )
            return false;
        return true;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const ItemPath& x) {
        if( x.parent ) {
            os << *x.parent;
        }
        if( x.name ) {
            os << "::" << x.name;
        }
        else if( x.ty ) {
            os << "<" << *x.ty;
            if( x.trait ) {
                os << " as " << *x.trait;
                if( x.trait_params ) {
                    os << *x.trait_params;
                }
            }
            os << ">";
        }
        else if( x.trait ) {
            os << "<* as " << *x.trait << ">";
        }
        else if( x.crate_name ) {
            os << "\"" << x.crate_name << "\"";
        }
        return os;
    }
};

}

