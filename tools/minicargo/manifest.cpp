/*
 */
#include "manifest.h"
#include "toml.h"
#include "debug.h"
#include "helpers.h"
#include <cassert>
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

static ::std::vector<::std::shared_ptr<PackageManifest>>    g_loaded_manifests;

PackageManifest::PackageManifest()
{
}

namespace
{
    void target_edit_from_kv(PackageTarget& target, TomlKeyValue& kv, unsigned base_idx);
}

PackageManifest PackageManifest::load_from_toml(const ::std::string& path)
{
    PackageManifest rv;
    rv.m_manifest_path = path;

    TomlFile    toml_file(path);

    for(auto key_val : toml_file)
    {
        assert(key_val.path.size() > 0);
        const auto& section = key_val.path[0];
        if( section == "package" )
        {
            assert(key_val.path.size() > 1);
            const auto& key = key_val.path[1];
            if(key == "authors")
            {
                // TODO: Use the `authors` key
                // - Ignore ofr now.
            }
            else if( key == "name" )
            {
                if(rv.m_name != "" )
                {
                    // TODO: Warn/error
                    throw ::std::runtime_error("Package name set twice");
                }
                rv.m_name = key_val.value.as_string();
                if(rv.m_name == "")
                {
                    // TODO: Error
                    throw ::std::runtime_error("Package name cannot be empty");
                }
            }
            else if( key == "version" )
            {
                rv.m_version = PackageVersion::from_string(key_val.value.as_string());
            }
            else
            {
                // Unknown value in `package`
                throw ::std::runtime_error("Unknown key `" + key + "` in [package]");
            }
        }
        else if( section == "lib" )
        {
            // 1. Find (and add if needed) the `lib` descriptor
            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [](const auto& x){ return x.m_type == PackageTarget::Type::Lib; });
            if(it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget { PackageTarget::Type::Lib });

            // 2. Parse from the key-value pair
            target_edit_from_kv(*it, key_val, 1);
        }
        else if( section == "bin" )
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi( key_val.path[1] );

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Bin && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Bin });

            target_edit_from_kv(*it, key_val, 2);
        }
        else if (section == "test")
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi(key_val.path[1]);

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Test && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Test });

            target_edit_from_kv(*it, key_val, 2);
        }
        else if (section == "bench")
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi(key_val.path[1]);

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Bench && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Bench });

            target_edit_from_kv(*it, key_val, 2);
        }
        else if( section == "dependencies" )
        {
            assert(key_val.path.size() > 1);

            const auto& depname = key_val.path[1];

            // Find/create dependency descriptor
            auto it = ::std::find_if(rv.m_dependencies.begin(), rv.m_dependencies.end(), [&](const auto& x) { return x.m_name == depname; });
            bool was_added = (it == rv.m_dependencies.end());
            if (it == rv.m_dependencies.end())
            {
                it = rv.m_dependencies.insert(it, PackageRef{ depname });
            }
            auto& ref = *it;

            if( key_val.path.size() == 2 )
            {
                // Shorthand, picks a version from the package repository
                if(!was_added)
                {
                    throw ::std::runtime_error(::format("ERROR: Duplicate dependency `", depname, "`"));
                }

                const auto& version_spec_str = key_val.value.as_string();
                ref.m_version = PackageVersionSpec::from_string(version_spec_str);
            }
            else
            {

                // (part of a) Full dependency specification
                const auto& attr = key_val.path[2];
                if( attr == "path" )
                {
                    // Set path specification of the named depenency
                    ref.m_path = key_val.value.as_string();
                }
                else if (attr == "git")
                {
                    // Load from git repo.
                    TODO("Support git dependencies");
                }
                else if (attr == "branch")
                {
                    // Specify git branch
                    TODO("Support git dependencies (branch)");
                }
                else if( attr == "version")
                {
                    assert(key_val.path.size() == 3);
                    // Parse version specifier
                    ref.m_version = PackageVersionSpec::from_string(key_val.value.as_string());
                }
                else
                {
                    // TODO: Error
                    throw ::std::runtime_error(::format("ERROR: Unkown depencency attribute `", attr, "` on dependency `", depname, "`"));
                }
            }
        }
        else if( section == "patch" )
        {
            //const auto& repo = key_val.path[1];
            TODO("Support repository patches");
        }
        else
        {
            // Unknown manifest section
            TODO("Unknown manifest section " << section);
        }
    }

    return rv;
}

namespace
{
    void target_edit_from_kv(PackageTarget& target, TomlKeyValue& kv, unsigned base_idx)
    {
        const auto& key = kv.path[base_idx];
        if(key == "name")
        {
            assert(kv.path.size() == base_idx+1);
            target.m_name = kv.value.as_string();
        }
        else if(key == "path")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_path = kv.value.as_string();
        }
        else if(key == "test")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_test = kv.value.as_bool();
        }
        else if (key == "doctest")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_doctest = kv.value.as_bool();
        }
        else if (key == "bench")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_bench = kv.value.as_bool();
        }
        else if (key == "doc")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_doc = kv.value.as_bool();
        }
        else if (key == "plugin")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_plugin = kv.value.as_bool();
        }
        else if (key == "proc-macro")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_proc_macro = kv.value.as_bool();
        }
        else if (key == "harness")
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_own_harness = kv.value.as_bool();
        }
        else
        {
            throw ::std::runtime_error( ::format("TODO: Handle target option `", key, "`") );
        }
    }
}

const PackageTarget& PackageManifest::get_library() const
{
    auto it = ::std::find_if(m_targets.begin(), m_targets.end(), [](const auto& x) { return x.m_type == PackageTarget::Type::Lib; });
    if (it == m_targets.end())
    {
        throw ::std::runtime_error(::format("Package ", m_name, " doesn't have a library"));
    }
    return *it;
}

const PackageManifest& PackageRef::get_package() const
{
    DEBUG("TODO: PackageRef::get_package()");
    throw "";
}

PackageVersion PackageVersion::from_string(const ::std::string& s)
{
    PackageVersion  rv;
    ::std::istringstream    iss { s };
    iss >> rv.major;
    iss.get();
    iss >> rv.minor;
    if( iss.get() != EOF )
    {
        iss >> rv.patch;
    }
    return rv;
}

PackageVersionSpec PackageVersionSpec::from_string(const ::std::string& s)
{
    PackageVersionSpec  rv;
    throw "";
    return rv;
}