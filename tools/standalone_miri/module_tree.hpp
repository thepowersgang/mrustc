//
//
//
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

#include "../../src/mir/mir.hpp"
#include "hir_sim.hpp"

struct Function
{
    ::std::vector<::HIR::TypeRef>   args;
    ::HIR::TypeRef   ret_ty;
    ::MIR::Function m_mir;
};

/// Container for loaded code and structures 
class ModuleTree
{
    friend struct Parser;

    ::std::set<::std::string>   loaded_files;

    ::std::map<::HIR::Path, Function>    functions;
    // Hack: Tuples are stored as `::""::<A,B,C,...>`
    ::std::map<::HIR::GenericPath, ::std::unique_ptr<DataType>>  data_types;
public:
    ModuleTree();

    void load_file(const ::std::string& path);

    ::HIR::SimplePath find_lang_item(const char* name) const;
    const Function& get_function(const ::HIR::Path& p) const;
};

// struct/union/enum
struct DataType
{
    // TODO: Metadata type! (indicates an unsized wrapper)
    // TODO: Drop glue

    size_t  alignment;
    size_t  size;
    // Offset and datatype
    ::std::vector<::std::pair<size_t, ::HIR::TypeRef>> fields;
    // Values for variants
    struct VariantValue {
        size_t base_field;
        ::std::vector<size_t>   field_path;
        uint64_t    value;  // TODO: This should be arbitary data? what size?
    };
    ::std::vector<VariantValue> variants;
};
