/*
 */
#include "../common.hpp"
#include "../ast/ast.hpp"
#include <iostream>

typedef ::std::vector< ::std::pair< ::std::string, TypeRef> >	item_vec_t;

void Render_Type(::std::ostream& os, const TypeRef& type, const char *name)
{
    /*
    switch(type.class())
    {
    case TYPECLASS_STRUCT:
        os << "struct " << type.struct().mangled_name() << " " << name;
        break;
    }
    */
}

void Render_CStruct(::std::ostream& os, const AST::CStruct& str)
{
    os << "struct " << str.name() << "{\n";
    for(auto& f : str.fields())
    {
        os << "\t";
        Render_Type(os, f.second, f.first.c_str());
        os << ";\n";
    }
    os << "}\n";
}

void Render_Crate(::std::ostream& os, const AST::Flat& crate)
{
    // First off, print forward declarations of all structs + enums
    for(const auto& s : crate.structs())
        os << "struct " << s.mangled_name() << ";\n";

    for(const auto& item : crate.functions())
    {
        const auto& name = item.first;
        const auto& fcn = item.second;
        Render_Type(os, fcn.rettype(), nullptr);
        os << " " << name << "(";
        bool is_first = true;
        for(const auto& f : fcn.args())
        {
            if( !is_first )
                os << ", ";
            is_first = false;
            Render_Type(os, f.second, f.first.c_str());
        }
        os << ")\n{\n";
        // Dump expression AST
        os << "}\n";
    }
}

