/*
 */
#include "../ast/ast.hpp"

void Render_Type(::std::ostream& os, const TypeRef& type, const char *name)
{
    /*
    swicth(type.class())
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
    FOREACH(::std::vector<std::pair<std::string,TypeRef> >, f, str.fields())
    {
        os << "\t";
        Render_Type(os, f->second(), f->first().c_str());
        os << ";\n";
    }
    os << "}\n"
}

void Render_Crate(::std::ostream& os, const AST::Flat& crate)
{
    // First off, print forward declarations of all structs + enums
    FOREACH(::std::vector<AST::CStruct>, s, crate.structs())
        os << "struct " << s->mangled_name() << ";\n";

    FOREACH(::std::vector<AST::Function>, fcn, crate.functions())
    {
        Render_Type(os, fcn->rettype(), nullptr);
        os << " " << fcn->name() << "(";
        bool is_first = true;
        FOREACH(::std::vector<std::pair<std::string,TypeRef> >, f, fcn.args())
        {
            if( !is_first )
                os << ", ";
            is_first = false;
            Render_Type(os, f->second(), f->first().c_str());
        }
        os << ")\n{\n";
        // Dump expression AST
        os << "}\n";
    }
}

