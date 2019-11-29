/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * manifest.cpp
 * - Cargo.toml manifest loading and manipulation code
 */
#include "manifest.h"
#include "toml.h"
#include "debug.h"
#include "path.h"
#include <cassert>
#include <algorithm>
#include <cctype>   // toupper
#include "repository.h"
#include "cfg.hpp"

// TODO: Extract this from the target at runtime (by invoking the compiler on the passed target)
#ifdef _WIN32
# define TARGET_NAME    "i586-windows-msvc"
#elif defined(__NetBSD__)
# define TARGET_NAME "x86_64-unknown-netbsd"
#else
# define TARGET_NAME "x86_64-unknown-linux-gnu"
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
    auto package_dir = ::helpers::path(path).parent();

    TomlFile    toml_file(path);

    for(auto key_val : toml_file)
    {
        assert(key_val.path.size() > 0);
        DEBUG(key_val.path << " = " << key_val.value);
        const auto& section = key_val.path[0];
        if( section == "package" )
        {
            assert(key_val.path.size() > 1);
            const auto& key = key_val.path[1];
            if( key == "name" )
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
                try
                {
                    rv.m_version = PackageVersion::from_string(key_val.value.as_string());
                }
                catch(const ::std::invalid_argument& e)
                {
                    throw ::std::runtime_error(format("Unable to parse package verison in '", path, "' - ", e.what()));
                }
            }
            else if( key == "build" )
            {
                if(rv.m_build_script != "" )
                {
                    // TODO: Warn/error
                    throw ::std::runtime_error("Build script path set twice");
                }
                if( key_val.value.m_type == TomlValue::Type::Boolean ) {
                    if( key_val.value.as_bool() )
                        throw ::std::runtime_error("Build script set to 'true'?");
                    rv.m_build_script = "-";
                }
                else {
                    rv.m_build_script = key_val.value.as_string();
                }
                if(rv.m_build_script == "")
                {
                    // TODO: Error
                    throw ::std::runtime_error("Build script path cannot be empty");
                }
            }
            else if( key == "authors"
                  || key == "description"
                  || key == "homepage"
                  || key == "documentation"
                  || key == "repository"
                  || key == "readme"
                  || key == "categories"
                  || key == "keywords"
                  || key == "license"
                )
            {
                // Informational only, ignore
            }
            else if( key == "exclude"
                  || key == "include"
                  )
            {
                // Packaging
            }
            else if( key == "metadata" )
            {
                // Unknown.
            }
            else if( key == "links" )
            {
                if(rv.m_links != "" )
                {
                    // TODO: Warn/error
                    throw ::std::runtime_error("Package 'links' attribute set twice");
                }
                rv.m_links = key_val.value.as_string();
            }
            else if( key == "autotests" )
            {
                // TODO: Fix the outer makefile so it doesn't need `foo-test`
                // to be created.
                //rv.m_create_auto_test = key_val.value.as_bool();
            }
            else if( key == "autobenches" )
            {
                //rv.m_create_auto_bench = key_val.value.as_bool();
            }
            else if( key == "workspace" )
            {
                if( rv.m_workspace_manifest.is_valid() )
                {
                    ::std::cerr << toml_file.lexer() << ": Duplicate workspace specification" << ::std::endl;
                }
                else
                {
                    rv.m_workspace_manifest = key_val.value.as_string();
                }
            }
            else
            {
                // Unknown value in `package`
                ::std::cerr << "WARNING: Unknown key `" + key + "` in [package]" << ::std::endl;
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
        else if( section == "dependencies" || section == "build-dependencies" || section == "dev-dependencies" )
        {
            ::std::vector<PackageRef>& dep_list =
                section == "dependencies" ? rv.m_dependencies :
                section == "build-dependencies" ? rv.m_build_dependencies :
                /*section == "dev-dependencies" ? */ rv.m_dev_dependencies /*:
                throw ""*/
                ;
            assert(key_val.path.size() > 1);

            const auto& depname = key_val.path[1];

            // Find/create dependency descriptor
            auto it = ::std::find_if(dep_list.begin(), dep_list.end(), [&](const auto& x) { return x.m_name == depname; });
            bool was_added = (it == dep_list.end());
            if( was_added )
            {
                it = dep_list.insert(it, PackageRef{ depname });
            }

            it->fill_from_kv(was_added, key_val, 2);
        }
        else if( section == "patch" )
        {
            //const auto& repo = key_val.path[1];
            TODO("Support repository patches");
        }
        else if( section == "profile" )
        {
            // TODO: Various profiles (debug, release, ...)
        }
        else if( section == "target" )
        {
            // TODO: Target opts?
            if( key_val.path.size() < 3 ) {
                throw ::std::runtime_error("Expected at least three path components in `[target.{...}]`");
            }
            const auto& cfg = key_val.path[1];
            const auto& real_section = key_val.path[2];
            // Check if `cfg` currently applies.
            // - It can be a target spec, or a cfg(foo) same as rustc
            bool success;
            if( cfg.substr(0, 4) == "cfg(" ) {
                success = Cfg_Check(cfg.c_str());
            }
            else {
                // It's a target name
                success = (cfg == TARGET_NAME);
            }
            // If so, parse as if the path was `real_section....`
            if( success )
            {
                if( real_section == "dependencies"
                    || real_section == "dev-dependencies" 
                    || real_section == "build-dependencies" 
                    )
                {
                    ::std::vector<PackageRef>& dep_list =
                        real_section == "dependencies" ? rv.m_dependencies :
                        real_section == "build-dependencies" ? rv.m_build_dependencies :
                        /*real_section == "dev-dependencies" ? */ rv.m_dev_dependencies /*:
                        throw ""*/
                        ;
                    assert(key_val.path.size() > 3);

                    const auto& depname = key_val.path[3];

                    // Find/create dependency descriptor
                    auto it = ::std::find_if(dep_list.begin(), dep_list.end(), [&](const auto& x) { return x.m_name == depname; });
                    bool was_added = (it == dep_list.end());
                    if( was_added )
                    {
                        it = dep_list.insert(it, PackageRef{ depname });
                    }

                    it->fill_from_kv(was_added, key_val, 4);
                }
                else
                {
                    TODO(toml_file.lexer() << ": Unknown manifest section '" << real_section << "' in `target`");
                }
            }
        }
        else if( section == "features" )
        {
            const auto& name = key_val.path[1];
            // TODO: Features
            ::std::vector<::std::string>*   list;
            if(name == "default") {
                list = &rv.m_default_features;
            }
            else {
                auto tmp = rv.m_features.insert(::std::make_pair(name, ::std::vector<::std::string>()));
                list = &tmp.first->second;
            }

            for(const auto& sv : key_val.value.m_sub_values)
            {
                list->push_back( sv.as_string() );
            }
        }
        else if( section == "workspace" )
        {
            // NOTE: This will be parsed in full by other code?
            if( ! rv.m_workspace_manifest.is_valid() )
            {
                // TODO: if the workspace was specified via `[package] workspace` then error
                rv.m_workspace_manifest = rv.m_manifest_path;
            }
        }
        // crates.io metadata
        else if( section == "badges" )
        {
        }
        else
        {
            // Unknown manifest section
            ::std::cerr << "WARNING: Unknown manifest section `" + section + "`" << ::std::endl;
            // TODO: Prevent this from firing multiple times in a row.
        }
    }

    if( rv.m_name == "" )
    {
        throw ::std::runtime_error(format("Manifest file ",path," doesn't specify a package name"));
    }

    // Default targets
    // - If there's no library section, but src/lib.rs exists, add one
    if( ! ::std::any_of(rv.m_targets.begin(), rv.m_targets.end(), [](const auto& x){ return x.m_type == PackageTarget::Type::Lib; }) )
    {
        // No library, add one pointing to lib.rs
        if( ::std::ifstream(package_dir / "src" / "lib.rs").good() )
        {
            DEBUG("- Implicit library");
            rv.m_targets.push_back(PackageTarget { PackageTarget::Type::Lib });
        }
    }
    // - If there's no binary section, but src/main.rs exists, add as a binary
    if( ! ::std::any_of(rv.m_targets.begin(), rv.m_targets.end(), [](const auto& x){ return x.m_type == PackageTarget::Type::Bin; }) )
    {
        // No library, add one pointing to lib.rs
        if( ::std::ifstream(package_dir / "src" / "main.rs").good() )
        {
            DEBUG("- Implicit binary");
            rv.m_targets.push_back(PackageTarget { PackageTarget::Type::Bin });
        }
    }
    if( rv.m_targets.empty() )
    {
        throw ::std::runtime_error(format("Manifest file ", path, " didn't specify any targets (and src/{main,lib}.rs doesn't exist)"));
    }

    // Default target names
    for(auto& tgt : rv.m_targets)
    {
        if(tgt.m_path == "")
        {
            switch(tgt.m_type)
            {
            case PackageTarget::Type::Lib:
                tgt.m_path = "src/lib.rs";
                break;
            case PackageTarget::Type::Bin:
                if(tgt.m_name == "") {
                    tgt.m_path = "src/main.rs";
                }
                else {
                    // TODO: Error if both exist
                    // TODO: More complex search rules
                    tgt.m_path = ::helpers::path("src") / "bin" / tgt.m_name.c_str() + ".rs";
                    if( !::std::ifstream(package_dir / tgt.m_path).good() )
                        tgt.m_path = ::helpers::path("src") / "bin" / tgt.m_name.c_str() / "main.rs";
                    //if( !::std::ifstream(package_dir / tgt.m_path).good() )
                    //    throw ::std::runtime_error(format("Unable to find source file for ", tgt.m_name, " - ", package_dir / tgt.m_path));
                }
                break;
            case PackageTarget::Type::Test:
            case PackageTarget::Type::Bench:
                // no defaults
                break;
            case PackageTarget::Type::Example:
                TODO("Default/implicit path for examples");
            }
        }
        if(tgt.m_name == "")
        {
            tgt.m_name.reserve(rv.m_name.size());
            for(auto c : rv.m_name)
                tgt.m_name += (c == '-' ? '_' : c);
        }
    }

    // If there's a lib target, add a test target using the same path
    {
        auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&](const auto& t) { return t.m_type == PackageTarget::Type::Lib; });
        if( it != rv.m_targets.end() )
        {
            auto path = it->m_path;
            auto name = it->m_name + "-test";
            rv.m_targets.push_back(PackageTarget { PackageTarget::Type::Test });
            rv.m_targets.back().m_name = name;
            rv.m_targets.back().m_path = path;
        }
    }

    for(const auto& dep : rv.m_dependencies)
    {
        if( dep.m_optional )
        {
            rv.m_features.insert(::std::make_pair( dep.m_name, ::std::vector<::std::string>() ));
        }
    }

    // Auto-detect the build script if required
    if( rv.m_build_script == "-" )
    {
        // Explicitly disabled `[package] build = false`
        rv.m_build_script = "";
    }
    else if( rv.m_build_script == "" )
    {
        // Not set, check for a "build.rs" file
        if( ::std::ifstream( package_dir / "build.rs").good() )
        {
            DEBUG("- Implicit build.rs");
            rv.m_build_script = "build.rs";
        }
    }
    else
    {
        // Explicitly set
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
        else if( key == "doctest" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_doctest = kv.value.as_bool();
        }
        else if( key == "bench" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_bench = kv.value.as_bool();
        }
        else if( key == "doc" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_enable_doc = kv.value.as_bool();
        }
        else if( key == "plugin" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_plugin = kv.value.as_bool();
        }
        else if( key == "proc-macro" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_proc_macro = kv.value.as_bool();
        }
        else if( key == "harness" )
        {
            assert(kv.path.size() == base_idx + 1);
            target.m_is_own_harness = kv.value.as_bool();
        }
        else if( key == "crate-type" )
        {
            //assert_kv_size(kv, base_idx + 1);
            //assert_type(kv, base_idx + 1);
            assert(kv.path.size() == base_idx + 1);
            if( !target.m_crate_types.empty() ) {
                // TODO: Error, multiple instances
            }
            for(const auto& sv : kv.value.m_sub_values)
            {
                const auto& s = sv.as_string();
                if(s == "rlib") {
                    target.m_crate_types.push_back(PackageTarget::CrateType::rlib);
                }
                else if(s == "dylib") {
                    target.m_crate_types.push_back(PackageTarget::CrateType::dylib);
                }
                // TODO: Other crate types
                else {
                    throw ::std::runtime_error(format("Unknown crate type - ", s));
                }
            }
        }
        else if( key == "required-features" )
        {
            assert(kv.path.size() == base_idx + 1);
            for(const auto& sv : kv.value.m_sub_values)
            {
                target.m_required_features.push_back( sv.as_string() );
            }
        }
        else
        {
            throw ::std::runtime_error( ::format("TODO: Handle target option `", key, "`") );
        }
    }
}

void PackageRef::fill_from_kv(bool was_added, const TomlKeyValue& key_val, size_t base_idx)
{
    if( key_val.path.size() == base_idx )
    {
        // Shorthand, picks a version from the package repository
        if(!was_added)
        {
            throw ::std::runtime_error(::format("ERROR: Duplicate dependency `", this->m_name, "`"));
        }

        const auto& version_spec_str = key_val.value.as_string();
        try
        {
            this->m_version = PackageVersionSpec::from_string(version_spec_str);
        }
        catch(const ::std::invalid_argument& e)
        {
            throw ::std::runtime_error(format("Unable to parse dependency verison for ", this->m_name, " - ", e.what()));
        }
    }
    else
    {

        // (part of a) Full dependency specification
        const auto& attr = key_val.path[base_idx];
        if( attr == "path" )
        {
            assert(key_val.path.size() == base_idx+1);
            // Set path specification of the named depenency
            this->m_path = key_val.value.as_string();
        }
        else if( attr == "git" )
        {
            // Load from git repo.
            TODO("Support git dependencies");
        }
        else if( attr == "branch" )
        {
            // Specify git branch
            TODO("Support git dependencies (branch)");
        }
        else if( attr == "version" )
        {
            assert(key_val.path.size() == base_idx+1);
            // Parse version specifier
            try
            {
                this->m_version = PackageVersionSpec::from_string(key_val.value.as_string());
            }
            catch(const ::std::invalid_argument& e)
            {
                throw ::std::runtime_error(format("Unable to parse dependency verison for '", this->m_name, "' - ", e.what()));
            }
        }
        else if( attr == "optional" )
        {
            assert(key_val.path.size() == base_idx+1);
            this->m_optional = key_val.value.as_bool();
        }
        else if( attr == "default-features" )
        {
            assert(key_val.path.size() == base_idx+1);
            this->m_use_default_features = key_val.value.as_bool();
        }
        else if( attr == "features" )
        {
            for(const auto& sv : key_val.value.m_sub_values)
            {
                this->m_features.push_back( sv.as_string() );
            }
        }
        else if ( attr == "package" )
        {
            assert(key_val.path.size() == base_idx+1);
            this->m_name = key_val.value.as_string();
        }
        else
        {
            // TODO: Error
            throw ::std::runtime_error(::format("ERROR: Unkown dependency attribute `", attr, "` on dependency `", this->m_name, "`"));
        }
    }
}

bool PackageManifest::has_library() const
{
    auto it = ::std::find_if(m_targets.begin(), m_targets.end(), [](const auto& x) { return x.m_type == PackageTarget::Type::Lib; });
    return it != m_targets.end();
}
const PackageTarget& PackageManifest::get_library() const
{
    auto it = ::std::find_if(m_targets.begin(), m_targets.end(), [](const auto& x) { return x.m_type == PackageTarget::Type::Lib; });
    if (it == m_targets.end())
    {
        throw ::std::runtime_error(::format("Package '", m_name, "' doesn't have a library"));
    }
    return *it;
}

void PackageManifest::set_features(const ::std::vector<::std::string>& features, bool enable_default)
{
    TRACE_FUNCTION_F(m_name << " [" << features << "] " << enable_default);

    size_t start = m_active_features.size();
    // 1. Install default features.
    if(enable_default)
    {
        DEBUG("Including default features [" << m_default_features << "]");
        for(const auto& feat : m_default_features)
        {
            auto it = ::std::find(m_active_features.begin(), m_active_features.end(), feat);
            if(it != m_active_features.end()) {
                continue ;
            }
            m_active_features.push_back(feat);
        }
    }

    auto add_feature = [&](const auto& feat) {
        auto it = ::std::find(m_active_features.begin(), m_active_features.end(), feat);
        if(it != m_active_features.end()) {
            DEBUG("`" << feat << "` already active");
        }
        else {
            m_active_features.push_back(feat);
        }
        };
    for(const auto& feat : features)
    {
        add_feature(feat);
    }

    for(size_t i = start; i < m_active_features.size(); i ++)
    {
        const auto featname = m_active_features[i];
        // Look up this feature
        auto it = m_features.find(featname);
        if( it != m_features.end() )
        {
            DEBUG("Activating feature " << featname << " = [" << it->second << "]");
            for(const auto& sub_feat : it->second)
            {
                auto slash_pos = sub_feat.find('/');
                if( slash_pos != ::std::string::npos ) {
                    ::std::string depname = sub_feat.substr(0, slash_pos);
                    ::std::string depfeat = sub_feat.substr(slash_pos+1);
                    DEBUG("Activate feature '" << depfeat << "' from dependency '" << depname << "'");
                    auto it2 = ::std::find_if(m_dependencies.begin(), m_dependencies.end(), [&](const auto& x){ return x.m_name == depname; });
                    if(it2 != m_dependencies.end())
                    {
                        it2->m_features.push_back(depfeat);
                        // TODO: Does this need to be set again?
                    }
                }
                else {
                    add_feature(sub_feat);
                }
            }
        }

        {
            auto it2 = ::std::find_if(m_dependencies.begin(), m_dependencies.end(), [&](const auto& x){ return x.m_name == featname; });
            if(it2 != m_dependencies.end())
            {
                it2->m_optional_enabled = true;
            }
        }

        {
            auto it2 = ::std::find_if(m_build_dependencies.begin(), m_build_dependencies.end(), [&](const auto& x){ return x.m_name == featname; });
            if(it2 != m_build_dependencies.end())
            {
                it2->m_optional_enabled = true;
            }
        }
        {
            auto it2 = ::std::find_if(m_dev_dependencies.begin(), m_dev_dependencies.end(), [&](const auto& x){ return x.m_name == featname; });
            if(it2 != m_dev_dependencies.end())
            {
                it2->m_optional_enabled = true;
            }
        }
    }

    // Return true if any features were activated
    //return start < m_active_features.size();
}
void PackageManifest::load_dependencies(Repository& repo, bool include_build, bool include_dev)
{
    TRACE_FUNCTION_F(m_name);
    DEBUG("Loading depencencies for " << m_name);
    auto base_path = ::helpers::path(m_manifest_path).parent();

    // 2. Recursively load dependency manifests
    for(auto& dep : m_dependencies)
    {
        if( dep.m_optional && !dep.m_optional_enabled )
        {
            continue ;
        }
        dep.load_manifest(repo, base_path, include_build);
    }

    // Load build deps if there's a build script AND build scripts are enabled
    if( m_build_script != "" && include_build )
    {
        DEBUG("- Build dependencies");
        for(auto& dep : m_build_dependencies)
        {
            if( dep.m_optional && !dep.m_optional_enabled )
            {
                continue ;
            }
            dep.load_manifest(repo, base_path, include_build);
        }
    }
    // Load dev dependencies if the caller has indicated they should be
    if( include_dev )
    {
        DEBUG("- Dev dependencies");
        for(auto& dep : m_dev_dependencies)
        {
            if( dep.m_optional && !dep.m_optional_enabled )
            {
                continue ;
            }
            dep.load_manifest(repo, base_path, include_build);
        }
    }
}

void PackageManifest::load_build_script(const ::std::string& path)
{
    ::std::ifstream is( path );
    if( !is.good() )
        throw ::std::runtime_error("Unable to open build script file '" + path + "'");

    BuildScriptOutput   rv;

    while( !is.eof() )
    {
        ::std::string   line;
        std::getline(is, line);
        if( line.compare(0, 5+1, "cargo:") == 0 )
        {
            size_t start = 5+1;
            size_t eq_pos = line.find_first_of('=');
            ::helpers::string_view  key { line.c_str() + start, eq_pos - start };
            ::helpers::string_view  value { line.c_str() + eq_pos + 1, line.size() - eq_pos - 1 };

            if( key == "minicargo-pre-build" ) {
                rv.pre_build_commands.push_back(value);
            }
            // cargo:rustc-link-search=foo/bar/baz
            else if( key == "rustc-link-search" ) {
                // Check for an = (otherwise default to native)
                ::std::string   value_str = value;
                auto pos = value_str.find_first_of('=');
                const char* type = "native";
                ::std::string   name;
                if(pos == ::std::string::npos) {
                    name = ::std::move(value_str);
                }
                else {
                    name = value_str.substr(pos+1);
                    auto ty_str = value_str.substr(0, pos);
                    if( ty_str == "native" ) {
                        type = "native";
                    }
                    else {
                        throw ::std::runtime_error(::format("TODO: rustc-link-search ", ty_str));
                    }
                }
                rv.rustc_link_search.push_back(::std::make_pair(type, ::std::move(name)));
            }
            // cargo:rustc-link-lib=mysql
            else if( key == "rustc-link-lib" ) {
                // Check for an = (otherwise default to dynamic)
                ::std::string   value_str = value;
                auto pos = value_str.find_first_of('=');
                const char* type = "static";
                ::std::string   name;
                if(pos == ::std::string::npos) {
                    name = ::std::move(value_str);
                }
                else {
                    name = value_str.substr(pos+1);
                    auto ty_str = value_str.substr(0, pos);
                    if( ty_str == "static" ) {
                        type = "static";
                    }
                    else if( ty_str == "dylib" ) {
                        type = "dynamic";
                    }
                    else {
                        throw ::std::runtime_error(::format("TODO: rustc-link-lib ", ty_str));
                    }
                }
                rv.rustc_link_lib.push_back(::std::make_pair(type, ::std::move(name)));
            }
            // cargo:rustc-cfg=foo
            else if( key == "rustc-cfg" ) {
                // TODO: Validate
                rv.rustc_cfg.push_back( value );
            }
            // cargo:rustc-flags=-l foo
            else if( key == "rustc-flags" ) {
                std::istringstream iss(value);
                for(std::string s; iss >> s;) {
                    rv.rustc_flags.push_back(s);
                }
            }
            // cargo:rustc-env=FOO=BAR
            else if( key == "rustc-env" ) {
                rv.rustc_env.push_back( value );
            }
            // cargo:rerun-if-changed=foo.rs
            else if( key == "rerun-if-changed" ) {
                DEBUG("TODO: '" << key << "' = '" << value << "'");
            }
            // - Ignore
            else {
                if( this->m_links != "" && line.find_first_of('-') == ::std::string::npos ) {
                    ::std::string   varname;
                    varname += "DEP_";
                    for(auto c : this->m_links)
                        varname += ::std::toupper(c);
                    varname += "_";
                    for(auto c : key)
                        varname += ::std::toupper(c);
                    rv.downstream_env.push_back(::std::make_pair( varname, static_cast<::std::string>(value) ));
                }
                else {
                    DEBUG("TODO: '" << key << "' = '" << value << "'");
                }
            }
        }
    }

    m_build_script_output = rv;
}

void PackageRef::load_manifest(Repository& repo, const ::helpers::path& base_path, bool include_build_deps)
{
    TRACE_FUNCTION_F(this->m_name);
    if( !m_manifest )
    {
        // If the path isn't set, check for:
        // - Git (checkout and use)
        // - Version and repository (check vendored, check cache, download into cache)
        if( !m_manifest && this->has_git() )
        {
            DEBUG("Load dependency " << this->name() << " from git");
            throw "TODO: Git";
        }

        if( !m_manifest && !this->get_version().m_bounds.empty() )
        {
            DEBUG("Load dependency " << this->name() << " from repo");
            m_manifest = repo.find(this->name(), this->get_version());
        }
        if( !m_manifest && this->has_path() )
        {
            DEBUG("Load dependency " << m_name << " from path " << m_path);
            // Search for a copy of this already loaded
            auto path = base_path / ::helpers::path(m_path) / "Cargo.toml";
            if( ::std::ifstream(path.str()).good() )
            {
                try
                {
                    m_manifest = repo.from_path(path);
                }
                catch(const ::std::exception& e)
                {
                    throw ::std::runtime_error( format("Error loading manifest '", path, "' - ", e.what()) );
                }
            }
        }


        if( !m_manifest )
        {
            throw ::std::runtime_error(::format( "Unable to find a manifest for ", this->name(), ":", this->get_version() ));
        }
    }

    m_manifest->set_features(this->m_features, this->m_use_default_features);
    m_manifest->load_dependencies(repo, include_build_deps);
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
    struct H {
        static unsigned parse_i(const ::std::string& istr, size_t& pos) {
            char* out_ptr = nullptr;
            long rv = ::std::strtol(istr.c_str() + pos, &out_ptr, 10);
            if( out_ptr == istr.c_str() + pos )
                throw ::std::invalid_argument(::format("Failed to parse integer from '", istr.c_str() + pos, "'"));
            pos = out_ptr - istr.c_str();
            return rv;
        }
    };
    PackageVersionSpec  rv;
    size_t pos = 0;
    do
    {
        while( pos < s.size() && isblank(s[pos]) )
            pos ++;
        if(pos == s.size())
            break ;
        // - Special case for wildcard
        if( s[pos] == '*' )
        {
            rv.m_bounds.push_back(PackageVersionSpec::Bound { PackageVersionSpec::Bound::Type::GreaterEqual, PackageVersion {} });

            while( pos < s.size() && isblank(s[pos]) )
                pos ++;
            if(pos == s.size())
                break ;
            continue ;
        }
        auto ty = PackageVersionSpec::Bound::Type::Compatible;
        switch(s[pos])
        {
        case '^':
            // Default, compatible
            pos ++;
            break;
        case '~':
            ty = PackageVersionSpec::Bound::Type::MinorCompatible;
            pos ++;
            break;
        case '=':
            ty = PackageVersionSpec::Bound::Type::Equal;
            pos ++;
            break;
        case '>':
            pos ++;
            switch(s[pos])
            {
            case '=':
                pos ++;
                ty = PackageVersionSpec::Bound::Type::GreaterEqual;
                break;
            default:
                ty = PackageVersionSpec::Bound::Type::Greater;
                break;
            }
            break;
        case '<':
            pos ++;
            switch(s[pos])
            {
            case '=':
                pos ++;
                ty = PackageVersionSpec::Bound::Type::LessEqual;
                break;
            default:
                ty = PackageVersionSpec::Bound::Type::Less;
                break;
            }
            break;
        default:
            break;
        }
        while( pos < s.size() && isblank(s[pos]) )
            pos ++;
        if( pos == s.size() )
            throw ::std::runtime_error("Bad version string - Expected version number");

        PackageVersion  v;
        v.major = H::parse_i(s, pos);
        if( s[pos] == '.' )
        {
            pos ++;
            v.minor = H::parse_i(s, pos);
            if(s[pos] == '.')
            {
                pos ++;
                v.patch = H::parse_i(s, pos);

                if( pos < s.size() && s[pos] == '-' )
                {
                    // Save tag (sequence of dot-seprated alpha-numeric identifiers)
                    auto tag_start = pos+1;
                    do {
                        // Could check the format, but meh.
                        pos ++;
                    } while(pos < s.size() && !isblank(s[pos]) && s[pos] != ',' );
                    //v.tag = ::std::string(s.c_str() + tag_start, s.c_str() + pos);
                }
            }
            else
            {
                v.patch = 0;
            }
        }
        else
        {
            // NOTE: This changes the behaviour of ~ rules to be bounded on the major version instead
            if( ty == PackageVersionSpec::Bound::Type::MinorCompatible )
                ty = PackageVersionSpec::Bound::Type::Compatible;
            v.minor = 0;
            v.patch = 0;
        }

        rv.m_bounds.push_back(PackageVersionSpec::Bound { ty, v });

        while( pos < s.size() && isblank(s[pos]) )
            pos ++;
        if(pos == s.size())
            break ;
    } while(pos < s.size() && s[pos++] == ',');
    if( pos != s.size() )
        throw ::std::runtime_error(::format( "Bad version string '", s, "', pos=", pos ));
    return rv;
}
bool PackageVersionSpec::accepts(const PackageVersion& v) const
{
    for(const auto& b : m_bounds)
    {
        switch(b.ty)
        {
        case Bound::Type::Compatible:
            // ^ rules are >= specified, and < next major/breaking
            if( !(v >= b.ver) )
                return false;
            if( !(v < b.ver.next_breaking()) )
                return false;
            break;
        case Bound::Type::MinorCompatible:
            // ~ rules are >= specified, and < next minor
            if( !(v >= b.ver) )
                return false;
            if( !(v < b.ver.next_minor()) )
                return false;
            break;
        case Bound::Type::GreaterEqual:
            if( !(v >= b.ver) )
                return false;
            break;
        case Bound::Type::Greater:
            if( !(v > b.ver) )
                return false;
            break;
        case Bound::Type::Equal:
            if( v != b.ver )
                return false;
            break;
        case Bound::Type::LessEqual:
            if( !(v <= b.ver) )
                return false;
            break;
        case Bound::Type::Less:
            if( !(v < b.ver) )
                return false;
            break;
        }
    }
    return true;
}
