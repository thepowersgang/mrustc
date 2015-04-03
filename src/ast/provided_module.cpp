/*
 */
#include "ast.hpp"

AST::Module g_compiler_module;

void AST_InitProvidedModule()
{
    // "struct str([u8])"
    g_compiler_module.add_struct(true, "str",
        AST::Struct( AST::MetaItems(), AST::TypeParams(), ::std::vector<AST::StructItem> {
            AST::StructItem("", TypeRef(TypeRef::TagUnsizedArray(), TypeRef(CORETYPE_U8)), false),
        }));
    
    AST::Path   copy_marker_path({AST::PathNode("marker"),AST::PathNode("Copy")});
    #define impl(trait, type) \
        g_compiler_module.add_impl(AST::Impl(AST::MetaItems(), AST::TypeParams(), type, trait))
    impl(copy_marker_path, TypeRef(CORETYPE_U8));
    impl(copy_marker_path, TypeRef(CORETYPE_U16));
    impl(copy_marker_path, TypeRef(CORETYPE_U32));
    impl(copy_marker_path, TypeRef(CORETYPE_U64));
    impl(copy_marker_path, TypeRef(CORETYPE_UINT));
    impl(copy_marker_path, TypeRef(CORETYPE_I8));
    impl(copy_marker_path, TypeRef(CORETYPE_I16));
    impl(copy_marker_path, TypeRef(CORETYPE_I32));
    impl(copy_marker_path, TypeRef(CORETYPE_I64));
    impl(copy_marker_path, TypeRef(CORETYPE_INT));
    impl(copy_marker_path, TypeRef(CORETYPE_F32));
    impl(copy_marker_path, TypeRef(CORETYPE_F64));
    
    // A hacky default impl of 'Sized', with a negative impl on [T]
    AST::Path   sized_marker_path({AST::PathNode("marker"),AST::PathNode("Sized")});
    impl(sized_marker_path, TypeRef());
    {
        AST::TypeParams tps;
        tps.add_ty_param( AST::TypeParam("T") );
        g_compiler_module.add_neg_impl(AST::ImplDef(
            AST::MetaItems(), ::std::move(tps),
            sized_marker_path,
            TypeRef(TypeRef::TagUnsizedArray(), TypeRef(TypeRef::TagArg(), "T"))
            ));
    }
}

