/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * manifest.h
 * - Cargo.toml package manifests
 */
#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <path.h>

#ifdef __OpenBSD__
// major() and minor() are defined as macros in <sys/types.h> on OpenBSD
// see: https://man.openbsd.org/major.3
# undef major
# undef minor
#endif

class WorkspaceManifest;
class PackageManifest;
class Repository;
struct TomlKeyValue;
struct ErrorHandler;

void Manifest_LoadOverrides(const ::std::string& s);

struct PackageVersion
{
    unsigned major;
    unsigned minor;
    unsigned patch;
    bool patch_set;
    std::string free_text;

    explicit PackageVersion()
        : major(0), minor(0), patch(0), patch_set(false) {}
    PackageVersion(unsigned major)
        : major(major), minor(0), patch(0), patch_set(false) {}
    PackageVersion(unsigned major, unsigned minor)
        : major(major), minor(minor), patch(0), patch_set(false) {}
    PackageVersion(unsigned major, unsigned minor, unsigned patch)
        : major(major), minor(minor), patch(patch), patch_set(true) {}

    static PackageVersion from_string(const ::std::string& s);

    PackageVersion next_minor() const {
        //if(major == 0) {
        //    return PackageVersion { 0, minor, patch+1 };
        //}
        //else {
            return PackageVersion { major, minor+1 };
        //}
    }
    PackageVersion next_breaking() const {
        if(major == 0) {
            return PackageVersion { 0, minor + 1 };
        }
        else {
            return PackageVersion { major + 1, 0 };
        }
    }
    PackageVersion prev_compat() const {
        if(major == 0) {
            // Before 1.0, there's no patch levels
            return *this;
        }
        else {
            // Anything from the same patch series
            return PackageVersion { major, minor, 0 };
        }
    }

    int cmp(const PackageVersion& x) const {
        if( major != x.major )  return (major < x.major) ? -1 : 1;
        if( minor != x.minor )  return (minor < x.minor) ? -1 : 1;
        if( patch_set && x.patch_set ) {
            if( patch != x.patch )  return (patch < x.patch) ? -1 : 1;
        }
        return 0;
    }

    bool operator==(const PackageVersion& x) const {
        return cmp(x) == 0;
    }
    bool operator!=(const PackageVersion& x) const {
        return cmp(x) != 0;
    }
    bool operator<(const PackageVersion& x) const {
        return cmp(x) < 0;
    }
    bool operator<=(const PackageVersion& x) const {
        return cmp(x) <= 0;
    }
    bool operator>(const PackageVersion& x) const {
        return cmp(x) > 0;
    }
    bool operator>=(const PackageVersion& x) const {
        return cmp(x) >= 0;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const PackageVersion& v) {
        os << v.major << "." << v.minor;
        if(v.patch_set)
            os << "." << v.patch;
        if(v.free_text != "") {
            os << "+" << v.free_text;
        }
        return os;
    }
};
struct PackageVersionSpec
{
    struct Bound
    {
        enum class Type
        {
            Compatible, // "^" - Allows anything up to the next major version
            MinorCompatible,   // "~X.Y" - Allows anything up to the next minor version
            Greater,
            GreaterEqual,
            Equal,
            LessEqual,
            Less,
        };

        Type    ty;
        PackageVersion  ver;
    };
    // TODO: Just upper and lower?
    ::std::vector<Bound>   m_bounds;

    // Construct from a string
    static PackageVersionSpec from_string(const ::std::string& s);

    /// Check if this spec accepts the passed version
    bool accepts(const PackageVersion& v) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const PackageVersionSpec& v) {
        bool first = true;
        for(const auto& b : v.m_bounds) {
            if(!first)
                os << ",";
            first = false;
            switch(b.ty)
            {
            case Bound::Type::Compatible: os << "^";  break;
            case Bound::Type::MinorCompatible: os << "~"; break;
            case Bound::Type::Greater:    os << ">";  break;
            case Bound::Type::GreaterEqual: os << ">=";  break;
            case Bound::Type::Equal:      os << "=";  break;
            case Bound::Type::LessEqual:  os << "<=";  break;
            case Bound::Type::Less:       os << "<";  break;
            }
            os << b.ver;
        }
        return os;
    }
};

class PackageRef
{
    friend class PackageManifest;
    friend class WorkspaceManifest;
    ::std::string   m_key;
    ::std::string   m_name;
    PackageVersionSpec  m_version;

    bool m_optional = false;
    bool m_public = false;
    ::std::string   m_path;

    // Features requested by this reference
    bool    m_use_default_features = true;
    ::std::vector<::std::string>    m_features;
    bool    m_optional_enabled = false;

    ::std::shared_ptr<PackageManifest> m_manifest;

    PackageRef(const ::std::string& n) :
        m_key(n),
        m_name(n)
    {
    }

    void fill_from_kv(
        ErrorHandler& eh,
        bool was_created, const TomlKeyValue& kv, size_t ofs,
        const WorkspaceManifest* wm, const ::helpers::path& base_dir
        );

public:
    const ::std::string& key() const { return m_key; }
    const ::std::string& name() const { return m_name; }
    //const ::std::string& get_repo_name() const  { return m_repo; }
    const PackageVersionSpec& get_version() const { return m_version; }

    bool is_disabled() const { return m_optional && !m_optional_enabled; }

    const bool has_path() const { return m_path != ""; }
    const ::std::string& path() const { return m_path; }
    const bool has_git() const { return false; }

    const PackageManifest& get_package() const {
        if(!m_manifest) throw ::std::runtime_error("Manifest not loaded for package " + m_name);
        return *m_manifest;
    }

    std::shared_ptr<PackageManifest> load_manifest_raw(Repository& repo, const ::helpers::path& base_path);
    void load_manifest(Repository& repo, const ::helpers::path& base_path, bool include_build_deps);

    friend std::ostream& operator<<(std::ostream& os, const PackageRef& pr);
};

enum class Edition
{
    Unspec,
    Rust2015,
    Rust2018,
    Rust2021,
};
struct PackageTarget
{
    enum class Type
    {
        Lib,
        Bin,
        Test,
        Bench,
        Example,
    };
    enum class CrateType
    {
        dylib,
        rlib,
        staticlib,
        cdylib,
        proc_macro,
    };

    Type    m_type;
    ::std::string   m_name;
    ::std::string   m_path;
    Edition m_edition = Edition::Unspec;
    bool    m_enable_test = true;
    bool    m_enable_doctest = true;
    bool    m_enable_bench = true;
    bool    m_enable_doc = true;
    bool    m_is_plugin = false;
    bool    m_is_proc_macro = false;
    bool    m_is_own_harness = false;

    ::std::vector<CrateType>    m_crate_types;
    ::std::vector<::std::string>    m_required_features;

    PackageTarget(Type ty):
        m_type(ty)
    {
        switch(ty)
        {
        case Type::Lib:
            m_path = "src/lib.rs";
            break;
        case Type::Bin:
            //m_path = "src/main.rs";
            break;
        default:
            break;
        }
    }
};

class BuildScriptOutput
{
public:
    // `cargo:minicargo-pre-build=make -C bar/`
    // MiniCargo hack
    ::std::vector<::std::string>    pre_build_commands;

    // cargo:rustc-link-search=foo/bar/baz
    ::std::vector<::std::pair<const char*,::std::string>>    rustc_link_search;
    // cargo:rustc-link-lib=mysql
    ::std::vector<::std::pair<const char*,::std::string>>    rustc_link_lib;
    // cargo:rustc-cfg=foo
    ::std::vector<::std::string>    rustc_cfg;
    // cargo:rustc-flags=-l foo
    ::std::vector<::std::string>    rustc_flags;
    // cargo:rustc-env=FOO=BAR
    ::std::vector<::std::pair<::std::string, ::std::string>>    rustc_env;

    // cargo:foo=bar when [package]links=baz
    ::std::vector<::std::pair<::std::string, ::std::string>>    downstream_env;
};

class WorkspaceManifest
{
    ::std::map< ::std::string, ::helpers::path> m_patches;

    Edition m_edition = Edition::Unspec;
    struct Dependencies {
        ::std::vector<PackageRef>   main;
        ::std::vector<PackageRef>   build;
        ::std::vector<PackageRef>   dev;
    };
    Dependencies    m_dependencies;

public:
    WorkspaceManifest();
    static WorkspaceManifest load_from_toml(const ::helpers::path& workspace_manifest_path);

    Edition edition() const { return m_edition; }
    const ::std::map< ::std::string, ::helpers::path>& patches() const { return m_patches; }
    const ::std::vector<PackageRef>& dependencies() const { return m_dependencies.main; }
    const ::std::vector<PackageRef>& build_dependencies() const { return m_dependencies.build; }
    const ::std::vector<PackageRef>& dev_dependencies() const { return m_dependencies.dev; }
private:
    void fill_from_kv(ErrorHandler& eh, const TomlKeyValue& kv);
};

class PackageManifest
{
    ::helpers::path m_manifest_dir;

    ::std::string   m_name;
    PackageVersion  m_version;
    ::std::string   m_links;

    Edition m_edition = Edition::Unspec;

    ::std::string   m_build_script;

    struct Dependencies {
        ::std::vector<PackageRef>   main;
        ::std::vector<PackageRef>   build;
        ::std::vector<PackageRef>   dev;
    };
    Dependencies    m_dependencies;
    std::map<std::string,Dependencies>  m_target_dependencies;

    ::std::vector<PackageTarget>    m_targets;

    BuildScriptOutput   m_build_script_output;

    ::std::map<::std::string, ::std::vector<::std::string>>    m_features;
    ::std::vector<::std::string>    m_default_features;
    ::std::vector<::std::string>    m_active_features;
    // Set once `load_dependencies` runs, clears when a new feature is activated (as that may activate features in deps)
    bool m_dependencies_loaded = false;

    // Cleared if any features mention `dep:`
    bool m_enable_implicit_optional_dep_features = true;

    PackageManifest();

public:
    static PackageManifest load_from_toml(const ::std::string& path, const WorkspaceManifest* wm);
    static PackageManifest magic_manifest(const char* name);
private:
    void fill_from_kv(ErrorHandler& eh, const WorkspaceManifest* wm, const TomlKeyValue& kv);

public:
    const PackageVersion& version() const { return m_version; }
    bool is_std_magic() const {
        return m_manifest_dir == ::helpers::path();
    }
    bool has_library() const;
    const PackageTarget& get_library() const;

    void dump(std::ostream& os) const;

    bool foreach_ty(PackageTarget::Type ty, ::std::function<bool(const PackageTarget&)> cb) const {
        for(const auto& t : m_targets ) {
            if( t.m_type == ty ) {
                if( !cb(t) )
                    return false;
            }
        }
        return true;
    }
    bool foreach_binaries(::std::function<bool(const PackageTarget&)> cb) const {
        return foreach_ty(PackageTarget::Type::Bin, cb);
    }

    const ::helpers::path directory() const {
        return m_manifest_dir;
    }
    const ::std::string& name() const {
        return m_name;
    }
    Edition edition() const {
        return m_edition;
    }
    const ::std::string& build_script() const {
        return m_build_script;
    }
    const BuildScriptOutput& build_script_output() const {
        return m_build_script_output;
    }
    void iter_dep_groups(std::function<void(const Dependencies&)> cb) const;
    void iter_main_dependencies(std::function<void(const PackageRef&)> cb) const {
        iter_dep_groups([=](const Dependencies& deps) {
            for(const auto& d : deps.main)
                cb(d);
            });
    }
    void iter_build_dependencies(std::function<void(const PackageRef&)> cb) const {
        iter_dep_groups([=](const Dependencies& deps) {
            for(const auto& d : deps.build)
                cb(d);
            });
    }
    void iter_dev_dependencies(std::function<void(const PackageRef&)> cb) const {
        iter_dep_groups([=](const Dependencies& deps) {
            for(const auto& d : deps.dev)
                cb(d);
            });
    }
    const ::std::vector<::std::string>& active_features() const {
        return m_active_features;
    }
    const ::std::map<::std::string, ::std::vector<::std::string>>& all_features() const {
        return m_features;
    }

    void set_features(const ::std::vector<::std::string>& features, bool enable_default);
    void load_dependencies(Repository& repo, bool include_build, bool include_dev=false);

    void load_build_script(const ::std::string& path);
};

