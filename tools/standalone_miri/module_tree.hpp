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
    RcString    my_path;
    ::std::vector<::HIR::TypeRef>   args;
    ::HIR::TypeRef   ret_ty;
    bool is_variadic;

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

    struct InitValue {
        struct Relocation {
            size_t  ofs;
            size_t  len;

            std::string string;
            HIR::Path   fcn_path;

            static Relocation new_string(size_t ofs, size_t len, std::string data) {
                Relocation  rv;
                rv.ofs = ofs;
                rv.len = len;
                rv.string = std::move(data);
                return rv;
            }
            static Relocation new_item(size_t ofs, size_t len, HIR::Path path) {
                Relocation  rv;
                rv.ofs = ofs;
                rv.len = len;
                rv.fcn_path = std::move(path);
                return rv;
            }
        };

        std::vector<uint8_t>    bytes;
        std::vector<Relocation> relocs;
    };
    InitValue   init;
};

/// Container for loaded code and structures 
class ModuleTree
{
    friend struct Parser;

    ::std::set<::std::string>   loaded_files;

    ::std::map<RcString, Function>    functions;
    ::std::map<RcString, Static>    statics;

    ::std::map<RcString, ::std::unique_ptr<DataType>>  data_types;

    ::std::set<FunctionType>    function_types; // note: insertion doesn't invaliate pointers.

    ::std::map<RcString, const Function*> ext_functions;
public:
    ModuleTree();

    void load_file(const ::std::string& path);
    void validate();

    const Function& get_function(const HIR::Path& p) const;
    const Function* get_function_opt(const HIR::Path& p) const;
    const Function* get_ext_function(const char* name) const;

    const Static& get_static(const HIR::Path& p) const;
    const Static* get_static_opt(const HIR::Path& p) const;

    const DataType& get_composite(const RcString& p) const {
        return *data_types.at(p);
    }

    void iterate_statics(std::function<void(RcString name, const Static& s)> cb) const {
        for(const auto& e : this->statics)
        {
            cb(e.first, e.second);
        }
    }
    void iterate_functions(std::function<void(RcString name, const Function& s)> cb) const {
        for(const auto& e : this->functions)
        {
            cb(e.first, e.second);
        }
    }
    void iterate_composites(std::function<void(RcString name, const DataType& s)> cb) const {
        for(const auto& e : this->data_types)
        {
            cb(e.first, *e.second);
        }
    }
};

// struct/union/enum
struct DataType
{
    bool   populated;
    RcString    my_path;

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
        // If empty, this is the wildcard variant
        ::std::string tag_data;
        // If SIZE_MAX, this has no associated data
        size_t data_field;
    };

    struct TagPath {
        size_t  base_field;
        std::vector<size_t> other_indexes;
    } tag_path;
    ::std::vector<VariantValue> variants;
};

struct FunctionType
{
    bool    unsafe;
    bool    is_variadic;
    ::std::string   abi;
    ::std::vector<HIR::TypeRef> args;
    HIR::TypeRef    ret;

    bool operator<(const FunctionType& x) const {
        #define _(f)    if(f != x.f) return f < x.f
        _(unsafe);
        _(is_variadic);
        _(abi);
        _(args);
        _(ret);
        #undef _
        return false;
    }
};
