/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * module_tree.hpp
 * - In-memory representation of a Monomorphised MIR executable (HEADER)
 */
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

#include "../../src/include/rc_string.hpp"
#include "../../src/mir/mir.hpp"
#include "hir_sim.hpp"
#include "value.hpp"

struct Function
{
    ::HIR::Path my_path;
    ::std::vector<::HIR::TypeRef>   args;
    ::HIR::TypeRef   ret_ty;

    // If `link_name` is non-empty, then the function is an external
    struct ExtInfo {
        ::std::string   link_name;
        ::std::string   link_abi;
    } external;
    ::MIR::Function m_mir;
};
struct Static
{
    ::HIR::TypeRef  ty;
    // TODO: Should this value be stored in the program state (making the entire `ModuleTree` const)
    Value   val;
};

/// Container for loaded code and structures 
class ModuleTree
{
    friend struct Parser;

    ::std::set<::std::string>   loaded_files;

    ::std::map<::HIR::Path, Function>    functions;
    ::std::map<::HIR::Path, Static>    statics;

    // Hack: Tuples are stored as `::""::<A,B,C,...>`
    ::std::map<::HIR::GenericPath, ::std::unique_ptr<DataType>>  data_types;

    ::std::map<::std::string, const Function*> ext_functions;
public:
    ModuleTree();

    void load_file(const ::std::string& path);
    void validate();

    ::HIR::SimplePath find_lang_item(const char* name) const;
    const Function& get_function(const ::HIR::Path& p) const;
    const Function* get_function_opt(const ::HIR::Path& p) const;
    const Function* get_ext_function(const char* name) const;
    Static& get_static(const ::HIR::Path& p);
    Static* get_static_opt(const ::HIR::Path& p);

    const DataType& get_composite(const ::HIR::GenericPath& p) const {
        return *data_types.at(p);
    }
};

// struct/union/enum
struct DataType
{
    bool   populated;
    ::HIR::GenericPath my_path;

    size_t  alignment;
    size_t  size;

    // Drop glue
    ::HIR::Path drop_glue;
    // Metadata type! (indicates an unsized wrapper)
    ::HIR::TypeRef  dst_meta;

    // Offset and datatype
    ::std::vector<::std::pair<size_t, ::HIR::TypeRef>> fields;
    // Values for variants
    struct VariantValue {
        size_t data_field;
        size_t base_field;
        ::std::vector<size_t>   field_path;

        //size_t tag_offset;  // Cached.
        ::std::string tag_data;
    };
    ::std::vector<VariantValue> variants;
};
