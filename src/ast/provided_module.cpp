/*
 */
#include "ast.hpp"

AST::Module g_compiler_module;

void AST_InitProvidedModule()
{
    // "struct str([u8])"
    g_compiler_module.add_struct(true, "str", AST::TypeParams(), ::std::vector<AST::StructItem> {
        AST::StructItem("", TypeRef(TypeRef::TagUnsizedArray(), TypeRef(CORETYPE_U8)), false),
        });
    
    AST::Path   copy_marker_path({AST::PathNode("marker"),AST::PathNode("Copy")});
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_U8), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_U16), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_U32), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_U64), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_UINT), copy_marker_path));
    
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_I8), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_I16), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_I32), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_I64), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_INT), copy_marker_path));
    
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_F32), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_F64), copy_marker_path));
    
    // A hacky default impl of 'Sized', with a negative impl on [T]
    AST::Path   sized_marker_path({AST::PathNode("marker"),AST::PathNode("Sized")});
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(), copy_marker_path));
    AST::TypeParams tps;
    tps.add_ty_param( AST::TypeParam("T") );
    g_compiler_module.add_impl(AST::Impl(
        AST::Impl::TagNegative(),
        ::std::move(tps),
        TypeRef(TypeRef::TagUnsizedArray(), TypeRef(TypeRef::TagArg(), "T")),
        copy_marker_path
        ));
}

