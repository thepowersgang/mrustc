#pragma once

#include <string>
#include <vector>
#include <memory>
#include "helpers.h"

class PackageManifest;
class Repository;

struct PackageVersion
{
    unsigned major;
    unsigned minor;
    unsigned patch;

    static PackageVersion from_string(const ::std::string& s);
    bool operator<(const PackageVersion& x) const {
        if( major < x.major )   return true;
        if( major > x.major )   return false;
        if( minor < x.minor )   return true;
        if( minor > x.minor )   return false;
        if( minor < x.patch )   return true;
        if( patch > x.patch )   return false;
        return false;
    }
};
struct PackageVersionSpec
{
    struct Bound
    {
        enum class Type
        {
            Compatible,
            Equal,
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
};

class PackageRef
{
    friend class PackageManifest;
    ::std::string   m_name;
    PackageVersionSpec  m_version;

    bool m_optional = false;
    ::std::string   m_path;
    ::std::shared_ptr<PackageManifest> m_manifest;

    PackageRef(const ::std::string& n) :
        m_name(n)
    {
    }

public:
    const ::std::string& name() const { return m_name; }
    //const ::std::string& get_repo_name() const  { return m_repo; }
    const PackageVersionSpec& get_version() const { return m_version; }

    bool is_optional() const { return m_optional; }

    const bool has_path() const { return m_path != ""; }
    const ::std::string& path() const { return m_path; }
    const bool has_git() const { return false; }

    const PackageManifest& get_package() const {
        if(!m_manifest) throw ::std::runtime_error("Manifest not loaded for package " + m_name);
        return *m_manifest;
    }

    void load_manifest(Repository& repo, const ::helpers::path& base_path);
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

    Type    m_type;
    ::std::string   m_name;
    ::std::string   m_path;
    bool    m_enable_test = true;
    bool    m_enable_doctest = true;
    bool    m_enable_bench = true;
    bool    m_enable_doc = true;
    bool    m_is_plugin = false;
    bool    m_is_proc_macro = false;
    bool    m_is_own_harness = false;

    PackageTarget(Type ty):
        m_type(ty)
    {
        switch(ty)
        {
        case Type::Lib:
            m_path = "src/lib.rs";
            break;
        case Type::Bin:
            m_path = "src/main.rs";
            break;
        default:
            break;
        }
    }
};

class PackageManifest
{
    ::std::string   m_manifest_path;

    ::std::string   m_name;
    PackageVersion  m_version;

    ::std::string   m_build_script;

    ::std::vector<PackageRef>   m_dependencies;

    ::std::vector<PackageTarget>    m_targets;

    struct BuildScript
    {
    };

    PackageManifest();
public:
    static PackageManifest load_from_toml(const ::std::string& path);

    const PackageTarget& get_library() const;


    const ::std::string& manifest_path() const {
        return m_manifest_path;
    }
    const ::std::string& name() const {
        return m_name;
    }
    const ::std::vector<PackageRef>& dependencies() const {
        return m_dependencies;
    }
    
    void load_dependencies(Repository& repo);
};
