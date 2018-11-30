# This is a Bazel package. See: https://bazel.build/ . Bazel is a
# high-performance (parallel, cached) build system, with a strong focus on
# correct / reproducible builds.
#
# This package also builds zlib, which is a Git submodule of this Git
# repository. If this target fails to build, then make sure you have initialized
# and updated submodules, e.g.:
#
#   git submodule init
#   git submodule update
#   bazel build :mrustc
#
#
# To build with Clang:
#
#    bazel build --client_env=CC=clang ...
#
# To build with GCC:
#
#    bazel build --client_env=CC=gcc ...
#
# If you do not specify a compiler to use, Bazel will search your system for a
# reasonable default toolchain. If you're feeling ambitious, you can also use
# Bazel's CROSSTOOL support.

GCC_COPTS = [
        "-Wno-misleading-indentation",
]

CLANG_COPTS = [
        # These options are specific to clang++. g++ will report a warning,
        # but will otherwise ignore them.
        "-Wno-c++98-c++11-compat",
        "-Wno-pessimizing-move",
        "-Wno-unused-private-field",
]

COPTS = [
        "-fexceptions",
        "-std=c++14",
        "-fsized-deallocation",
        "-Wno-string-conversion",
        "-Wno-implicit-fallthrough",
        "-Wno-return-type",
        "-Wno-unused-variable",
] + CLANG_COPTS

# Note: The order of includes is significant.  Do not reorder.
INCLUDES = [
    "src/include",
    "src",
    "tools/common",
    "src/hir_typeck",
    "src/parse",
    "src/hir",
]

FEATURES = [
    "-use_header_modules",
]

cc_library(
    name = "zlib",
    srcs = [
        "zlib/adler32.c",
        "zlib/crc32.c",
        "zlib/deflate.c",
        "zlib/inffast.c",
        "zlib/inflate.c",
        "zlib/inftrees.c",
        "zlib/trees.c",
        "zlib/zutil.c",
    ],
    hdrs = [
        "zlib/crc32.h",
        "zlib/deflate.h",
        "zlib/gzguts.h",
        "zlib/inffast.h",
        "zlib/inffixed.h",
        "zlib/inflate.h",
        "zlib/inftrees.h",
        "zlib/trees.h",
        "zlib/zconf.h",
        "zlib/zlib.h",
        "zlib/zutil.h",
    ],
    includes = [
        "zlib",
    ]
)


# standalone_miri doesn't build yet.

# cc_binary(
#     name = "standalone_miri",
#     srcs = [
#         "tools/standalone_miri/main.cpp",
#     ],
#     features = FEATURES,
#     includes = INCLUDES,
#     deps = [
#         ":standalone_miri_lib",
#     ],
# )
#
# cc_library(
#     name = "standalone_miri_lib",
#     srcs = [
#         "tools/standalone_miri/debug.cpp",
#         "tools/standalone_miri/hir_sim.cpp",
#         "tools/standalone_miri/lex.cpp",
#         "tools/standalone_miri/mir.cpp",
#         "tools/standalone_miri/miri.cpp",
#         "tools/standalone_miri/module_tree.cpp",
#         "tools/standalone_miri/value.cpp",
#     ],
#     features = FEATURES,
#     # includes = INCLUDES,
#     includes = [
#         "src/include",
#         "tools/common",
#         "tools/standalone_miri",
#     ],
#     textual_hdrs = [
#         "tools/standalone_miri/value.hpp",
#         "tools/standalone_miri/hir_sim.hpp",
#         "tools/standalone_miri/debug.hpp",
#         "tools/standalone_miri/lex.hpp",
#         "tools/standalone_miri/miri.hpp",
#         "tools/standalone_miri/hir/type.hpp",
#         "tools/standalone_miri/module_tree.hpp",
#     ],
#     deps = [
#         ":mrust_lib",
#     ],
# )

cc_library(
    name = "tools_common_lib",
    srcs = [
        "tools/common/debug.cpp",
        "tools/common/path.cpp",
        "tools/common/toml.cpp",
    ],
    features = FEATURES,
    includes = INCLUDES,
    textual_hdrs = [
        "tools/common/debug.h",
        "tools/common/helpers.h",
        "tools/common/path.h",
        "tools/common/target_detect.h",
        "tools/common/toml.h",
    ],
    copts = COPTS,
)

cc_binary(
    name = "mrustc",
    srcs = [
        "src/main.cpp",
    ],
    features = FEATURES,
    includes = INCLUDES,
    deps = [
        ":mrust_lib",
        ":tools_common_lib",
        ":version_lib",
    ],
    copts = COPTS,
)

cc_library(
    name = "version_lib",
    srcs = [
        "src/version.cpp",
    ],
    copts = [
        "-DVERSION_GIT_ISDIRTY=false",
        "-DVERSION_GIT_FULLHASH=\\\"none\\\"",
        "-DVERSION_GIT_SHORTHASH=\\\"none\\\"",
        "-DVERSION_BUILDTIME=\\\"none\\\"",
        "-DVERSION_GIT_BRANCH=\\\"none\\\"",
    ],
    features = FEATURES,
    includes = INCLUDES,
    textual_hdrs = [
        "src/include/version.hpp",
    ],
    deps = [
    ],
)

cc_library(
    name = "mrust_lib",
    srcs = [
        "src/ast/ast.cpp",
        "src/ast/crate.cpp",
        "src/ast/dump.cpp",
        "src/ast/expr.cpp",
        "src/ast/path.cpp",
        "src/ast/pattern.cpp",
        "src/ast/types.cpp",
        "src/debug.cpp",
        "src/expand/asm.cpp",
        "src/expand/cfg.cpp",
        "src/expand/concat.cpp",
        "src/expand/crate_tags.cpp",
        "src/expand/derive.cpp",
        "src/expand/env.cpp",
        "src/expand/file_line.cpp",
        "src/expand/format_args.cpp",
        "src/expand/include.cpp",
        "src/expand/lang_item.cpp",
        "src/expand/macro_rules.cpp",
        "src/expand/mod.cpp",
        "src/expand/proc_macro.cpp",
        "src/expand/rustc_diagnostics.cpp",
        "src/expand/std_prelude.cpp",
        "src/expand/stringify.cpp",
        "src/expand/test.cpp",
        "src/expand/test_harness.cpp",
        "src/hir/crate_post_load.cpp",
        "src/hir/crate_ptr.cpp",
        "src/hir/deserialise.cpp",
        "src/hir/dump.cpp",
        "src/hir/expr.cpp",
        "src/hir/expr_ptr.cpp",
        "src/hir/expr_state.hpp",
        "src/hir/from_ast.cpp",
        "src/hir/from_ast_expr.cpp",
        "src/hir/generic_params.cpp",
        "src/hir/hir.cpp",
        "src/hir/path.cpp",
        "src/hir/pattern.cpp",
        "src/hir/serialise.cpp",
        "src/hir/serialise_lowlevel.cpp",
        "src/hir/type.cpp",
        "src/hir/visitor.cpp",
        "src/hir_conv/bind.cpp",
        "src/hir_conv/constant_evaluation.cpp",
        "src/hir_conv/expand_type.cpp",
        "src/hir_conv/markings.cpp",
        "src/hir_conv/resolve_ufcs.cpp",
        "src/hir_expand/annotate_value_usage.cpp",
        "src/hir_expand/closures.cpp",
        "src/hir_expand/erased_types.cpp",
        "src/hir_expand/reborrow.cpp",
        "src/hir_expand/ufcs_everything.cpp",
        "src/hir_expand/vtable.cpp",
        "src/hir_typeck/common.cpp",
        "src/hir_typeck/expr_check.cpp",
        "src/hir_typeck/expr_cs.cpp",
        "src/hir_typeck/expr_visit.cpp",
        "src/hir_typeck/helpers.cpp",
        "src/hir_typeck/impl_ref.cpp",
        "src/hir_typeck/outer.cpp",
        "src/hir_typeck/static.cpp",
        "src/ident.cpp",
        "src/macro_rules/eval.cpp",
        "src/macro_rules/mod.cpp",
        "src/macro_rules/parse.cpp",
        "src/mir/check.cpp",
        "src/mir/check_full.cpp",
        "src/mir/cleanup.cpp",
        "src/mir/dump.cpp",
        "src/mir/from_hir.cpp",
        "src/mir/from_hir_match.cpp",
        "src/mir/helpers.cpp",
        "src/mir/mir.cpp",
        "src/mir/mir_builder.cpp",
        "src/mir/mir_ptr.cpp",
        "src/mir/optimise.cpp",
        "src/mir/visit_crate_mir.cpp",
        "src/parse/expr.cpp",
        "src/parse/interpolated_fragment.cpp",
        "src/parse/lex.cpp",
        "src/parse/parseerror.cpp",
        "src/parse/paths.cpp",
        "src/parse/pattern.cpp",
        "src/parse/root.cpp",
        "src/parse/token.cpp",
        "src/parse/tokenstream.cpp",
        "src/parse/tokentree.cpp",
        "src/parse/ttstream.cpp",
        "src/parse/types.cpp",
        "src/rc_string.cpp",
        "src/resolve/absolute.cpp",
        "src/resolve/index.cpp",
        "src/resolve/use.cpp",
        "src/span.cpp",
        "src/trans/allocator.cpp",
        "src/trans/codegen.cpp",
        "src/trans/codegen_c.cpp",
        "src/trans/codegen_c_structured.cpp",
        "src/trans/codegen_mmir.cpp",
        "src/trans/enumerate.cpp",
        "src/trans/mangling.cpp",
        "src/trans/monomorphise.cpp",
        "src/trans/target.cpp",
        "src/trans/trans_list.cpp",
    ],
    features = FEATURES,
    includes = INCLUDES,
    textual_hdrs = [
        "src/ast/ast.hpp",
        "src/ast/attrs.hpp",
        "src/ast/crate.hpp",
        "src/ast/expr.hpp",
        "src/ast/expr_ptr.hpp",
        "src/ast/generics.hpp",
        "src/ast/item.hpp",
        "src/ast/macro.hpp",
        "src/ast/path.hpp",
        "src/ast/pattern.hpp",
        "src/ast/types.hpp",
        "src/common.hpp",
        "src/coretypes.hpp",
        "src/expand/cfg.hpp",
        "src/expand/proc_macro.hpp",
        "src/hir/crate_ptr.hpp",
        "src/hir/expr.hpp",
        "src/hir/expr_ptr.hpp",
        "src/hir/from_ast.hpp",
        "src/hir/generic_params.hpp",
        "src/hir/hir.hpp",
        "src/hir/item_path.hpp",
        "src/hir/main_bindings.hpp",
        "src/hir/path.hpp",
        "src/hir/pattern.hpp",
        "src/hir/serialise_lowlevel.hpp",
        "src/hir/type.hpp",
        "src/hir/visitor.hpp",
        "src/hir_conv/main_bindings.hpp",
        "src/hir_expand/main_bindings.hpp",
        "src/hir_typeck/common.hpp",
        "src/hir_typeck/expr_visit.hpp",
        "src/hir_typeck/helpers.hpp",
        "src/hir_typeck/impl_ref.hpp",
        "src/hir_typeck/main_bindings.hpp",
        "src/hir_typeck/static.hpp",
        "src/include/compile_error.hpp",
        "src/include/cpp_unpack.h",
        "src/include/debug.hpp",
        "src/include/ident.hpp",
        "src/include/main_bindings.hpp",
        "src/include/rc_string.hpp",
        "src/include/span.hpp",
        "src/include/stdspan.hpp",
        "src/include/string_view.hpp",
        "src/include/synext.hpp",
        "src/include/synext_decorator.hpp",
        "src/include/synext_macro.hpp",
        "src/include/tagged_union.hpp",
        "src/macro_rules/macro_rules.hpp",
        "src/macro_rules/macro_rules_ptr.hpp",
        "src/macro_rules/pattern_checks.hpp",
        "src/mir/from_hir.hpp",
        "src/mir/helpers.hpp",
        "src/mir/main_bindings.hpp",
        "src/mir/mir.hpp",
        "src/mir/mir_ptr.hpp",
        "src/mir/operations.hpp",
        "src/mir/visit_crate_mir.hpp",
        "src/parse/common.hpp",
        "src/parse/eTokenType.enum.h",
        "src/parse/interpolated_fragment.hpp",
        "src/parse/lex.hpp",
        "src/parse/parseerror.hpp",
        "src/parse/token.hpp",
        "src/parse/tokenstream.hpp",
        "src/parse/tokentree.hpp",
        "src/parse/ttstream.hpp",
        "src/resolve/main_bindings.hpp",
        "src/trans/allocator.hpp",
        "src/trans/codegen.hpp",
        "src/trans/codegen_c.hpp",
        "src/trans/main_bindings.hpp",
        "src/trans/mangling.hpp",
        "src/trans/monomorphise.hpp",
        "src/trans/target.hpp",
        "src/trans/trans_list.hpp",
    ],
    deps = [
        ":tools_common_lib",
        ":zlib",
    ],
    alwayslink = 1,
    copts = COPTS,
)

cc_binary(
    name = "minicargo",
    srcs = [
        "tools/minicargo/main.cpp",
    ],
    features = FEATURES,
    includes = INCLUDES,
    deps = [
        ":minicargo_lib",
        ":tools_common_lib",
    ],
)

cc_library(
    name = "minicargo_lib",
    srcs = [
        "tools/minicargo/build.cpp",
        "tools/minicargo/manifest.cpp",
        "tools/minicargo/repository.cpp",
    ],
    features = FEATURES,
    includes = INCLUDES,
    textual_hdrs = [
        "tools/minicargo/build.h",
        "tools/minicargo/manifest.h",
        "tools/minicargo/repository.h",
        "tools/minicargo/stringlist.h",
    ],
    deps = [
        ":tools_common_lib",
    ],
)
