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
    const ItemPath* parent = nullptr;
    const ::HIR::TypeRef* ty = nullptr;
    const ::HIR::SimplePath* trait = nullptr;
    const ::HIR::PathParams* trait_params = nullptr;
    const char* name = nullptr;
    
public:
    ItemPath() {}
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
    
    ::HIR::SimplePath get_simple_path() const {
        if( parent ) {
            assert(name);
            return parent->get_simple_path() + name;
        }
        else {
            assert(!name);
            return ::HIR::SimplePath();
        }
    }
    const char* get_name() const {
        return name ? name : "";
    }
    
    ItemPath operator+(const ::std::string& name) const {
        return ItemPath(*this, name.c_str());
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
        else {
        }
        return os;
    }
};

}

