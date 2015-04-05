/*
 */
#include "ast.hpp"

void AST_InitProvidedModule_Impls();

AST::Module g_compiler_module;
AST::Path   g_copy_marker_path;
AST::Path   g_sized_marker_path;

void AST_InitProvidedModule()
{
    // "struct str([u8])"
    g_compiler_module.add_struct(true, "str",
        AST::Struct( AST::MetaItems(), AST::TypeParams(), ::std::vector<AST::StructItem> {
            AST::StructItem("", TypeRef(TypeRef::TagUnsizedArray(), TypeRef(CORETYPE_U8)), false),
        }));
    
    // TODO: Defer this until AFTER 
    AST_InitProvidedModule_Impls();
}

void AST_InitProvidedModule_Impls()
{
    if( !g_copy_marker_path.is_valid() ) {
        g_copy_marker_path = AST::Path( {AST::PathNode("marker"),AST::PathNode("Copy")} );
    }
    
    if( !g_sized_marker_path.is_valid() ) {
        g_sized_marker_path = AST::Path( {AST::PathNode("marker"),AST::PathNode("Sized")} );
    }
    
    #define impl(trait, type) \
        g_compiler_module.add_impl(AST::Impl(AST::MetaItems(), AST::TypeParams(), type, trait))
    impl(g_copy_marker_path, TypeRef(CORETYPE_U8));
    impl(g_copy_marker_path, TypeRef(CORETYPE_U16));
    impl(g_copy_marker_path, TypeRef(CORETYPE_U32));
    impl(g_copy_marker_path, TypeRef(CORETYPE_U64));
    impl(g_copy_marker_path, TypeRef(CORETYPE_UINT));
    impl(g_copy_marker_path, TypeRef(CORETYPE_I8));
    impl(g_copy_marker_path, TypeRef(CORETYPE_I16));
    impl(g_copy_marker_path, TypeRef(CORETYPE_I32));
    impl(g_copy_marker_path, TypeRef(CORETYPE_I64));
    impl(g_copy_marker_path, TypeRef(CORETYPE_INT));
    impl(g_copy_marker_path, TypeRef(CORETYPE_F32));
    impl(g_copy_marker_path, TypeRef(CORETYPE_F64));
    
    // A hacky default impl of 'Sized', with a negative impl on [T]
    impl(g_sized_marker_path, TypeRef());
    
    {
        AST::TypeParams tps;
        tps.add_ty_param( AST::TypeParam("T") );
        g_compiler_module.add_neg_impl(AST::ImplDef(
            AST::MetaItems(), ::std::move(tps),
            g_sized_marker_path,
            TypeRef(TypeRef::TagUnsizedArray(), TypeRef(TypeRef::TagArg(), "T"))
            ));
    }
}

