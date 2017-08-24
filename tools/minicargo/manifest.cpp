/*
 */
#include "manifest.h"
#include "toml.h"
#include "debug.h"
#include "path.h"
#include <cassert>
#include <algorithm>
#include "repository.h"

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
                rv.m_version = PackageVersion::from_string(key_val.value.as_string());
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

            it->fill_from_kv(was_added, key_val, 2);
        }
        else if( section == "build-dependencies" )
        {
            // TODO: Build deps
        }
        else if( section == "dev-dependencies" )
        {
            // TODO: Developemnt (test/bench) deps
        }
        else if( section == "patch" )
        {
            //const auto& repo = key_val.path[1];
            TODO("Support repository patches");
        }
        else if( section == "target" )
        {
            // TODO: Target opts?
        }
        else if( section == "features" )
        {
            // TODO: Features
        }
        else if( section == "workspace" )
        {
            // TODO: Workspaces?
        }
        // crates.io metadata
        else if( section == "badges" )
        {
        }
        else
        {
            // Unknown manifest section
            TODO("Unknown manifest section " << section);
        }
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

    // Default target names
    for(auto& tgt : rv.m_targets)
    {
        if(tgt.m_name == "")
        {
            tgt.m_name = rv.m_name;
        }
    }

    // Auto-detect the build script if required
    if( rv.m_build_script == "-" )
    {
        // Explicitly disabled `[package] build = false`
        rv.m_build_script = "";
    }
    else if( rv.m_build_script != "" )
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
            // TODO: Support crate types
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
        this->m_version = PackageVersionSpec::from_string(version_spec_str);
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
            this->m_version = PackageVersionSpec::from_string(key_val.value.as_string());
        }
        else if( attr == "optional" )
        {
            assert(key_val.path.size() == base_idx+1);
            this->m_optional = key_val.value.as_bool();
        }
        else if( attr == "default-features" )
        {
            assert(key_val.path.size() == base_idx+1);
            //this->m_use_default_features = key_val.value.as_bool();
        }
        else if( attr == "features" )
        {
            for(const auto& sv : key_val.value.m_sub_values)
            {
                this->m_features.push_back( sv.as_string() );
            }
        }
        else
        {
            // TODO: Error
            throw ::std::runtime_error(::format("ERROR: Unkown depencency attribute `", attr, "` on dependency `", this->m_name, "`"));
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

void PackageManifest::load_dependencies(Repository& repo)
{
    DEBUG("Loading depencencies for " << m_name);
    auto base_path = ::helpers::path(m_manifest_path).parent();

    // 2. Recursively load dependency manifests
    for(auto& dep : m_dependencies)
    {
        if( dep.m_optional )
        {
            // TODO: Check for feature that enables this (option to a feature with no '/')
            continue ;
        }
        dep.load_manifest(repo, base_path);
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
        is >> line;
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
                // TODO: Check for an = (otherwise default to dynamic)
                throw ::std::runtime_error("TODO: rustc-link-search");
            }
            // cargo:rustc-link-lib=mysql
            else if( key == "rustc-link-lib" ) {
                // TODO: Check for an = (otherwise default to dynamic)
                ::std::string lazy = value;
                auto pos = lazy.find_first_of('=');
                if(pos == ::std::string::npos) {
                    rv.rustc_link_lib.push_back(::std::make_pair("static", lazy));
                }
                else {
                    throw ::std::runtime_error("TODO: rustc-link-lib");
                }
            }
            // cargo:rustc-cfg=foo
            else if( key == "rustc-cfg" ) {
                // TODO: Validate
                rv.rustc_cfg.push_back( value );
            }
            // cargo:rustc-flags=-l foo
            else if( key == "rustc-flags" ) {
                // Split on space, then push each.
                throw ::std::runtime_error("TODO: rustc-flags");
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
                DEBUG("TODO: '" << key << "' = '" << value << "'");
            }
        }
    }

    m_build_script_output = rv;
}

void PackageRef::load_manifest(Repository& repo, const ::helpers::path& base_path)
{
    TRACE_FUNCTION_F(this->m_name);
    // If the path isn't set, check for:
    // - Git (checkout and use)
    // - Version and repository (check vendored, check cache, download into cache)
    if( ! this->has_path() )
    {
        if( this->has_git() )
        {
            DEBUG("Load dependency " << this->name() << " from git");
            throw "TODO: Git";
        }
        else
        {
            DEBUG("Load dependency " << this->name() << " from repo");
            m_manifest = repo.find(this->name(), this->get_version());
            if( !m_manifest ) {
                throw ::std::runtime_error(::format("Unable to load manifest for ", this->name(), ":", this->get_version()));
            }
        }
    }
    else
    {
        DEBUG("Load dependency " << m_name << " from path " << m_path);
        // Search for a copy of this already loaded
        m_manifest = repo.from_path(base_path / ::helpers::path(m_path) / "Cargo.toml");
        assert(m_manifest);
    }

    // TODO: Set features on this dependency

    m_manifest->load_dependencies(repo);
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
                throw ::std::invalid_argument(istr.c_str() + pos);
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
        auto ty = PackageVersionSpec::Bound::Type::Compatible;
        switch(s[pos])
        {
        case '^':
            // Default, compatible
            pos ++;
            break;
        case '=':
            ty = PackageVersionSpec::Bound::Type::Equal;
            pos ++;
            break;
        case '>':
            ty = PackageVersionSpec::Bound::Type::Greater;
            pos ++;
            break;
        case '<':
            ty = PackageVersionSpec::Bound::Type::Greater;
            pos ++;
            break;
        default:
            break;
        }
        while( pos < s.size() && isblank(s[pos]) )
            pos ++;
        if( pos == s.size() )
            throw ::std::runtime_error("Bad version string");

        PackageVersion  v;
        v.major = H::parse_i(s, pos);
        if( s[pos] != '.' )
            throw ::std::runtime_error("Bad version string");
        pos ++;
        v.minor = H::parse_i(s, pos);
        if(s[pos] == '.')
        {
            pos ++;
            v.patch = H::parse_i(s, pos);
        }
        else
        {
            v.patch = 0;
        }

        rv.m_bounds.push_back(PackageVersionSpec::Bound { ty, v });

        while( pos < s.size() && isblank(s[pos]) )
            pos ++;
        if(pos == s.size())
            break ;
    } while(pos < s.size() && s[pos++] == ',');
    if( pos != s.size() )
        throw ::std::runtime_error(::format( "Bad version string, pos=", pos ));
    return rv;
}
bool PackageVersionSpec::accepts(const PackageVersion& v) const
{
    for(const auto& b : m_bounds)
    {
        switch(b.ty)
        {
        case Bound::Type::Compatible:
            // To be compatible, it has to be higher?
            // - TODO: Isn't a patch version compatible?
            if( !(v >= b.ver) )
                return false;
            if( !(v < b.ver.next_breaking()) )
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
        case Bound::Type::Less:
            if( !(v < b.ver) )
                return false;
            break;
        }
    }
    return true;
}
