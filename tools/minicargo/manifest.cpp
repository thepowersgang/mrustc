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

/// <summary>
/// Interface for error messages
/// </summary>
struct ErrorHandler
{
    template<typename ...Args>
    [[noreturn]] void todo(Args... args) {
        do_fatal("TODO", [&](std::ostream& os){ format_to_stream(os, args...); });
        abort();
    }
    template<typename ...Args>
    [[noreturn]] void error(Args... args) {
        do_fatal("error", [&](std::ostream& os){ format_to_stream(os, args...); });
        abort();
    }
    template<typename ...Args>
    void warning(Args... args) /*__attribute__((noreturn))*/ {
        do_log("warning", [&](std::ostream& os){ format_to_stream(os, args...); });
    }

    virtual void do_fatal(const char* prefix, std::function<void(std::ostream&)> cb) = 0;
    virtual void do_log(const char* prefix, std::function<void(std::ostream&)> cb) = 0;
};

// Error messages from a .toml lexer
struct ErrorHandlerLex: public ErrorHandler
{
    const TomlLexer& lex;
    ErrorHandlerLex(const TomlLexer& lex): lex(lex) {}

    void fmt_message(::std::ostream& os, const char* prefix, std::function<void(std::ostream&)> cb) const {
        os << prefix << ": " << this->lex << ": ";
        cb(os);
    }

    void do_fatal(const char* prefix, std::function<void(std::ostream&)> cb) override {
        std::stringstream   ss;
        this->fmt_message(ss, prefix, cb);
        throw std::runtime_error(ss.str());
    }
    void do_log(const char* prefix, std::function<void(std::ostream&)> cb) override {
        this->fmt_message(std::cerr, prefix, cb);
        std::cerr << std::endl;
    }
};

class ManifestOverrides
{
public:
    class Override
    {
        friend class ManifestOverrides;

        ::std::vector<TomlKeyValue::Path>    m_deletes;
        ::std::vector<TomlKeyValue> m_adds;

        Override()
        {
        }

    public:
        bool is_filtered_out(const TomlKeyValue::Path& kv) const;
        const ::std::vector<TomlKeyValue>& overrides() const;
    };
private:
    std::map<std::string, Override>   m_entries;

public:
    ManifestOverrides()
    {
    }

    /// Load the overrides list from a file
    void load_from_toml(const ::std::string& path);

    /// Search for an override that applies to the given package directory
    const Override* lookup(const ::helpers::path& package_dir) const;
};
static ManifestOverrides    s_overrides;

namespace
{
    void target_edit_from_kv(ErrorHandler& eh, PackageTarget& target, const TomlKeyValue& kv, unsigned base_idx);
}

void Manifest_LoadOverrides(const ::std::string& s)
{
    s_overrides.load_from_toml(s);
}

PackageManifest::PackageManifest()
{
}

PackageManifest PackageManifest::load_from_toml(const ::std::string& path)
{
    PackageManifest rv;
    rv.m_manifest_path = path;
    auto package_dir = ::helpers::path(path).parent();

    TomlFile    toml_file(path);
    ErrorHandlerLex error_handler(toml_file.lexer());

    const auto* overrides = s_overrides.lookup(package_dir);
    if(overrides) {
        DEBUG("Overrides present for " << package_dir);
    }

    for(auto key_val : toml_file)
    {
        assert(key_val.path.size() > 0);

        if( overrides && overrides->is_filtered_out(key_val.path) ) {
            DEBUG("DELETE " << key_val.path << " = " << key_val.value);
            continue ;
        }
        DEBUG(key_val.path << " = " << key_val.value);

        rv.fill_from_kv(error_handler, key_val);
    }
    if(overrides)
    {
        for(auto key_val : overrides->overrides())
        {
            assert(key_val.path.size() > 0);
            DEBUG("ADD " << key_val.path << " = " << key_val.value);

            rv.fill_from_kv(error_handler, key_val);
        }
    }

    if( rv.m_name == "" )
    {
        throw ::std::runtime_error(format("Manifest file ", path, " doesn't specify a package name"));
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

    //if(rv.m_edition == Edition::Unspec)
    //{
    //    rv.m_edition = Edition::Rust2015;
    //}

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
                tgt.m_path = ::helpers::path("src") / "examples" / tgt.m_name.c_str() + ".rs";
                if( !::std::ifstream(package_dir / tgt.m_path).good() )
                    tgt.m_path = ::helpers::path("src") / "examples" / tgt.m_name.c_str() / "main.rs";
                break;
            }
        }
        if(tgt.m_name == "")
        {
            tgt.m_name.reserve(rv.m_name.size());
            for(auto c : rv.m_name)
                tgt.m_name += (c == '-' ? '_' : c);
        }
        if(tgt.m_edition == Edition::Unspec)
        {
            tgt.m_edition = rv.m_edition;
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
            rv.m_targets.back().m_edition = rv.m_edition;
        }
    }

    rv.iter_main_dependencies([&](const PackageRef& dep) {
        if( dep.m_optional )
        {
            rv.m_features.insert(::std::make_pair( dep.m_name, ::std::vector<::std::string>() ));
        }
    });

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


void PackageManifest::fill_from_kv(ErrorHandler& eh, const TomlKeyValue& key_val)
{
    auto& rv = *this;

    {
        const auto& section = key_val.path[0];
        if( section == "package" )
        {
            assert(key_val.path.size() > 1);
            const auto& key = key_val.path[1];
            if( key == "name" )
            {
                if(rv.m_name != "" )
                {
                    eh.error("Package name set twice");
                }
                rv.m_name = key_val.value.as_string();
                if(rv.m_name == "")
                {
                    eh.error("Package name cannot be empty");
                }
            }
            else if( key == "version" )
            {
                try
                {
                    rv.m_version = PackageVersion::from_string(key_val.value.as_string());
                    rv.m_version.patch_set = true;
                }
                catch(const ::std::invalid_argument& e)
                {
                    eh.error("Unable to parse package verison - ", e.what());
                }
            }
            else if( key == "build" )
            {
                if(rv.m_build_script != "" )
                {
                    eh.error("Build script path set twice");
                }
                if( key_val.value.m_type == TomlValue::Type::Boolean ) {
                    if( key_val.value.as_bool() )
                        eh.todo("Build script set to 'true'?");
                    rv.m_build_script = "-";
                }
                else {
                    rv.m_build_script = key_val.value.as_string();
                }
                if(rv.m_build_script == "")
                {
                    eh.error("Build script path cannot be empty");
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
            else if( key == "autoexamples" )
            {
                //rv.m_create_auto_example = key_val.value.as_bool();
            }
            else if( key == "workspace" )
            {
                if( rv.m_workspace_manifest.is_valid() )
                {
                    eh.warning("Duplicate workspace specification");
                }
                else
                {
                    rv.m_workspace_manifest = key_val.value.as_string();
                }
            }
            else if( key == "edition" )
            {
                assert(key_val.path.size() == 2);
                assert(key_val.value.m_type == TomlValue::Type::String);
                if(key_val.value.as_string() == "2015") {
                    rv.m_edition = Edition::Rust2015;
                }
                else if(key_val.value.as_string() == "2018") {
                    rv.m_edition = Edition::Rust2018;
                }
                else {
                    eh.error("Unknown edition value ", key_val.value);
                }
            }
            else
            {
                // Unknown value in `package`
                eh.warning("Unknown key `", key, "` in [package]");
            }
        }
        else if( section == "lib" )
        {
            // 1. Find (and add if needed) the `lib` descriptor
            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [](const auto& x){ return x.m_type == PackageTarget::Type::Lib; });
            if(it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget { PackageTarget::Type::Lib });

            // 2. Parse from the key-value pair
            target_edit_from_kv(eh, *it, key_val, 1);
        }
        else if( section == "bin" )
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi( key_val.path[1] );

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Bin && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Bin });

            target_edit_from_kv(eh, *it, key_val, 2);
        }
        else if( section == "test" )
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi(key_val.path[1]);

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Test && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Test });

            target_edit_from_kv(eh, *it, key_val, 2);
        }
        else if( section == "bench" )
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi(key_val.path[1]);

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Bench && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Bench });

            target_edit_from_kv(eh, *it, key_val, 2);
        }
        else if( section == "example")
        {
            assert(key_val.path.size() > 1);
            unsigned idx = ::std::stoi(key_val.path[1]);

            auto it = ::std::find_if(rv.m_targets.begin(), rv.m_targets.end(), [&idx](const auto& x) { return x.m_type == PackageTarget::Type::Example && idx-- == 0; });
            if (it == rv.m_targets.end())
                it = rv.m_targets.insert(it, PackageTarget{ PackageTarget::Type::Example });

            target_edit_from_kv(eh, *it, key_val, 2);
        }
        else if( section == "dependencies" || section == "build-dependencies" || section == "dev-dependencies" )
        {
            auto& dep_group = rv.m_dependencies;
            ::std::vector<PackageRef>& dep_list =
                section == "dependencies" ? dep_group.main :
                section == "build-dependencies" ? dep_group.build :
                /*section == "dev-dependencies" ? */ dep_group.dev /*:
                                                                           throw ""*/
                ;
            assert(key_val.path.size() > 1);

            const auto& depname = key_val.path[1];

            // Find/create dependency descriptor
            auto it = ::std::find_if(dep_list.begin(), dep_list.end(), [&](const auto& x) { return x.m_key == depname; });
            bool was_added = (it == dep_list.end());
            if( was_added )
            {
                it = dep_list.insert(it, PackageRef{ depname });
            }

            it->fill_from_kv(eh, was_added, key_val, 2);
        }
        else if( section == "patch" )
        {
            //const auto& repo = key_val.path[1];
            eh.todo("Support repository patches");
        }
        else if( section == "profile" )
        {
            // TODO: Various profiles (debug, release, ...)
        }
        else if( section == "target" )
        {
            // TODO: Target opts?
            if( key_val.path.size() < 3 ) {
                eh.error("Expected at least three path components in `[target.{...}]`");
            }
            const auto& cfg = key_val.path[1];
            const auto& real_section = key_val.path[2];
            // Check if `cfg` currently applies.
            // - It can be a target spec, or a cfg(foo) same as rustc
#if 1
            auto& dep_group = rv.m_target_dependencies[cfg];
#else
            bool success;
            if( cfg.substr(0, 4) == "cfg(" ) {
                try {
                    success = Cfg_Check(cfg.c_str());
                }
                catch(const std::exception& e)
                {
                    eh.error(e.what());
                }
            }
            else {
                // It's a target name
                success = (cfg == TARGET_NAME);
            }
            auto& dep_group = rv.m_dependencies;

            // If so, parse as if the path was `real_section....`
            if( success )
#endif
            {
                if( real_section == "dependencies"
                    || real_section == "dev-dependencies" 
                    || real_section == "build-dependencies" 
                    )
                {
                    ::std::vector<PackageRef>& dep_list =
                        real_section == "dependencies" ? dep_group.main :
                        real_section == "build-dependencies" ? dep_group.build :
                        /*real_section == "dev-dependencies" ? */ dep_group.dev /*:
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

                    it->fill_from_kv(eh, was_added, key_val, 4);
                }
                else
                {
                    eh.todo("Unknown manifest section '", real_section, "' in `target`");
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
            eh.warning("Unknown manifest section `", section, "`");
            // TODO: Prevent this from firing multiple times in a row.
        }
    }
    }

namespace
{
    void target_edit_from_kv(ErrorHandler& eh, PackageTarget& target, const TomlKeyValue& kv, unsigned base_idx)
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
        else if( key == "edition" )
        {
            assert(kv.path.size() == base_idx + 1);
            assert(kv.value.m_type == TomlValue::Type::String);
            if(kv.value.as_string() == "2015") {
                target.m_edition = Edition::Rust2015;
            }
            else if(kv.value.as_string() == "2018") {
                target.m_edition = Edition::Rust2018;
            }
            else {
                eh.error("Unknown edition value ", kv.value);
            }
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
                    eh.error("Unknown crate type - ", s);
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
            eh.error("TODO: Handle target option `", key, "`");
        }
    }
}

void PackageRef::fill_from_kv(ErrorHandler& eh, bool was_added, const TomlKeyValue& key_val, size_t base_idx)
{
    if( key_val.path.size() == base_idx )
    {
        // Shorthand, picks a version from the package repository
        if(!was_added)
        {
            eh.error("ERROR: Duplicate dependency `", this->m_name, "`");
        }

        const auto& version_spec_str = key_val.value.as_string();
        try
        {
            this->m_version = PackageVersionSpec::from_string(version_spec_str);
        }
        catch(const ::std::invalid_argument& e)
        {
            eh.error("Unable to parse dependency verison for ", this->m_name, " - ", e.what());
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
            eh.todo("Support git dependencies");
        }
        else if( attr == "branch" )
        {
            // Specify git branch
            eh.todo("Support git dependencies (branch)");
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
                eh.error("Unable to parse dependency verison for '", this->m_name, "' - ", e.what());
            }
        }
        else if( attr == "optional" )
        {
            assert(key_val.path.size() == base_idx+1);
            this->m_optional = key_val.value.as_bool();
        }
        else if( attr == "default-features" || attr == "default_features" ) // Huh, either is valid?
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
            eh.error("ERROR: Unkown dependency attribute `", attr, "` on dependency `", this->m_name, "`");
        }
    }
}

void PackageManifest::dump(std::ostream& os) const
{
    os
        << "PackageManifest {\n"
        << "  '" << m_name << "' v" << m_version << (m_links != "" ? " links=" : "") << m_links << "\n"
        << "  m_manifest_path = " << m_manifest_path << "\n"
        << "  m_workspace_manifest = " << m_workspace_manifest << "\n"
        << "  m_edition = " << (int)m_edition << "\n"
        << "  m_dependencies = [\n";
    this->iter_main_dependencies([&](const PackageRef& dep) {
        os << "    " << dep << "\n";
    });
    os
        << "  ]\n"
        << "  m_build_dependencies = [\n"
        ;
    this->iter_build_dependencies([&](const PackageRef& dep) {
        os << "    " << dep << "\n";
        });
    os
        << "  ]\n"
        << "  m_dev_dependencies = [\n"
        ;
    this->iter_dev_dependencies([&](const PackageRef& dep) {
        os << "    " << dep << "\n";
        });
    os
        << "  ]\n"
        << "}\n"
        ;
}

void PackageManifest::iter_dep_groups(std::function<void(const Dependencies&)> cb) const
{
    cb(m_dependencies);
    for(const auto& td : m_target_dependencies)
    {
        const auto& cfg = td.first;
        bool success;
        if( cfg.substr(0, 4) == "cfg(" ) {
            try {
                // TODO: Pass feature list
                success = Cfg_Check(cfg.c_str(), this->m_active_features);
            }
            catch(const std::exception& /*e*/)
            {
                //eh.error(e.what());
                throw ;
            }
        }
        else {
            // It's a target name
            success = (cfg == TARGET_NAME);
        }
        if(success) {
            cb(td.second);
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

        auto iter_all_deps = [&](std::function<void(PackageRef&)> cb) {
            iter_dep_groups([&](const Dependencies& dg_c) {
                Dependencies& dg = const_cast<Dependencies&>(dg_c);
                for(auto& dep : dg.main)
                    cb(dep);
                for(auto& dep : dg.build)
                    cb(dep);
                for(auto& dep : dg.dev)
                    cb(dep);
            });
        };

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
                    iter_all_deps([&](PackageRef& dep) {
                        if(dep.m_key == depname) {
                            dep.m_features.push_back(depfeat);
                            // TODO: Does this need to call `set_features` again?
                        }
                        });
                }
                else {
                    add_feature(sub_feat);
                }
            }
        }

        iter_all_deps([&](PackageRef& dep) {
            if(dep.m_key == featname) {
                dep.m_optional_enabled = true;
            }
            });
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
    iter_main_dependencies([&](const PackageRef& dep_c) {
        auto& dep = const_cast<PackageRef&>(dep_c);
        if( dep.m_optional && !dep.m_optional_enabled )
        {
            return ;
        }
        try {
            dep.load_manifest(repo, base_path, include_build);
        }
        catch(const std::exception& )
        {
            std::cerr << "While processing " << this->manifest_path() << std::endl;
            throw ;
        }
    });

    // Load build deps if there's a build script AND build scripts are enabled
    if( m_build_script != "" && include_build )
    {
        DEBUG("- Build dependencies");
        iter_build_dependencies([&](const PackageRef& dep_c) {
            auto& dep = const_cast<PackageRef&>(dep_c);
            if( dep.m_optional && !dep.m_optional_enabled )
            {
                return ;
            }
            try {
                dep.load_manifest(repo, base_path, include_build);
            }
            catch(const std::exception& )
            {
                std::cerr << "While processing build deps " << this->manifest_path() << std::endl;
                throw ;
            }
        });
    }
    // Load dev dependencies if the caller has indicated they should be
    if( include_dev )
    {
        DEBUG("- Dev dependencies");
        iter_dev_dependencies([&](const PackageRef& dep_c) {
            auto& dep = const_cast<PackageRef&>(dep_c);
            if( dep.m_optional && !dep.m_optional_enabled )
            {
                return ;
            }
            try {
                dep.load_manifest(repo, base_path, include_build);
            }
            catch(const std::exception& )
            {
                std::cerr << "While processing dev deps " << this->manifest_path() << std::endl;
                throw ;
            }
        });
    }
}

void PackageManifest::load_build_script(const ::std::string& path)
{
    ::std::ifstream is( path );
    if( !is.good() )
        throw ::std::runtime_error(format("Unable to open build script file '" + path + "' for ", this->m_manifest_path));

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
                    // on Apple operating systems only, an application framework.
                    else if( ty_str == "framework" ) {
                        type = "framework";
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
                auto eq = std::find(value.begin(), value.end(), '=');
                if( eq == value.end() )
                    throw ::std::runtime_error(::format("rustc-env has no `=` - `", value, "`"));

                rv.rustc_env.push_back(std::make_pair( std::string(value.begin(), eq), std::string(eq+1, value.end()) ));
            }
            // cargo:rerun-if-changed=foo.rs
            else if( key == "rerun-if-changed" ) {
                DEBUG("TODO: '" << key << "' = '" << value << "'");
            }
            // - Ignore
            else {
                if( this->m_links != "" && std::find(key.begin(), key.end(), '-') == key.end() ) {
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
        if( this->has_git() )
        {
            DEBUG("Load dependency " << this->name() << " from git");
            throw "TODO: Git";
        }

        if( !this->get_version().m_bounds.empty() )
        {
            DEBUG("Load dependency " << this->name() << " from repo");
            m_manifest = repo.find(this->name(), this->get_version());
        }

        // NOTE: kernel32-sys specifies both a path and a version for its `winapi` dep
        // - Ideally, path would be processed first BUT the path in this case points at the workspace (which isn't handled well)
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
            else
            {
                throw ::std::runtime_error(format("Cannot open manifest ", path, " for ", this->name()));
            }
        }

        if( !(this->has_git() || !this->get_version().m_bounds.empty() || this->has_path()) )
        {
            //DEBUG("Load dependency " << this->name() << " from repo");
            //m_manifest = repo.find(this->name(), this->get_version());
            throw ::std::runtime_error(format("No source for ", this->name()));
        }

        if( !m_manifest )
        {
            throw ::std::runtime_error(::format( "Unable to find a manifest for ", this->name(), ":", this->get_version() ));
        }
    }

    m_manifest->set_features(this->m_features, this->m_use_default_features);
    m_manifest->load_dependencies(repo, include_build_deps);
}


std::ostream& operator<<(std::ostream& os, const PackageRef& pr)
{
    os << "PackageRef {";
    os << " '" << pr.m_name << "'";
    if( pr.m_key != pr.m_key) {
        os << " key='" << pr.m_key << "'";
    }
    if(!pr.m_version.m_bounds.empty()) {
        os << " " << pr.m_version;
    }
    if(pr.m_optional) {
        os << " optional";
    }
    if(pr.has_path()) {
        os << " path='" << pr.m_path << "'";
    }
    os << " }";
    //::std::string   m_name;
    //PackageVersionSpec  m_version;
    //
    //bool m_optional = false;
    //::std::string   m_path;
    //
    //// Features requested by this reference
    //bool    m_use_default_features = true;
    //::std::vector<::std::string>    m_features;
    //bool    m_optional_enabled = false;
    //
    //::std::shared_ptr<PackageManifest> m_manifest;
    return os;
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
        rv.patch_set = true;
    }
    else
    {
        rv.patch = 0;
        rv.patch_set = false;
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


void ManifestOverrides::load_from_toml(const ::std::string& path)
{
    TRACE_FUNCTION_F(path);

    TomlFile    toml_file(path);
    ErrorHandlerLex error_handler(toml_file.lexer());

    for(auto kv : toml_file)
    {
        assert(kv.path.size() > 0);
        if(kv.path.size() < 2) {
            error_handler.error("Must have at least two path components");
        }

        auto v = m_entries.insert( std::make_pair(::helpers::path(kv.path[1].c_str()).str(), Override()) );
        if(v.second) {
            DEBUG("Adding overrides for " << v.first->first);
        }
        auto& ent = v.first->second;

        if( kv.path[0] == "add" ) {
            if(kv.path.size() < 3) {
                error_handler.error("Must have at least three path components");
            }
            // Remove the first two components, and push to the list of new key/values
            kv.path.erase(kv.path.begin(), kv.path.begin()+2);
            ent.m_adds.push_back(std::move(kv));
        }
        else if( kv.path[0] == "delete" ) {
            auto add_delete = [&ent,&error_handler](const std::string& s) {
                std::vector<std::string>    del_path;

                const char* start = s.c_str();
                const char* end;
                do {
                    end = strchr(start, '.');
                    if(end) {
                        del_path.push_back(std::string(start, end));
                        start = end+1;
                    }
                    else {
                        del_path.push_back(start);
                    }
                    if(del_path.back() == "") {
                        error_handler.error("`delete` entries must not contain empty components");
                    }
                } while(end);

                ent.m_deletes.push_back( std::move(del_path) );
                };
            switch( kv.path.size() )
            {
            case 2:
                if( kv.value.m_type != TomlValue::Type::List )
                    error_handler.error("`delete` entries must be strings or lists of strings, got ", kv.value.m_type);
                for(const auto& e : kv.value.m_sub_values)
                {
                    if( e.m_type != TomlValue::Type::String )
                        error_handler.error("`delete` entries must be strings or lists of strings, got ", e.m_type, " in list");
                    add_delete(e.as_string());
                }
                break;
            case 3:
                // Ensure correct format (three entry path, string)
                // NOTE: The third path component is ignored (it's assumed to be an array, but anything counts)
                if( kv.value.m_type != TomlValue::Type::String )
                    error_handler.error("`delete` entries must be strings or lists of strings, got ", kv.value.m_type);
                add_delete(kv.value.as_string());
                break;
            default:
                error_handler.error("`delete` entries must have at two/three path components");
            }
        }
        else {
            error_handler.error("Unknown entry in overrides file : `", kv.path[0], "`");
        }
    }
}
const ManifestOverrides::Override* ManifestOverrides::lookup(const ::helpers::path& package_dir) const
{
    TRACE_FUNCTION_F(package_dir);
    const auto& s = package_dir.str();
    for(const auto& ent : m_entries)
    {
        const auto& ent_s = ent.first;
        DEBUG(ent_s);
        if( ent_s.size() <= s.size() )
        {
            size_t ofs = s.size() - ent_s.size();
            if( strcmp(s.c_str() + ofs, ent_s.c_str()) == 0 )
            {
                return &ent.second;
            }
        }
    }

    return nullptr;
}

bool ManifestOverrides::Override::is_filtered_out(const TomlKeyValue::Path& path) const
{
    for(const auto& del : m_deletes)
    {
        if( del.size() <= path.size() )
        {
            if( std::equal(del.begin(), del.end(), path.begin()) )
            {
                return true;
            }
        }
    } 
    return false;
}
const std::vector<TomlKeyValue>& ManifestOverrides::Override::overrides() const
{
    return m_adds;
}
