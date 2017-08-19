#pragma once

#include <string>
#include <vector>
#include <memory>

class PackageManifest;

struct PackageVersion
{
    unsigned major;
    unsigned minor;
    unsigned patch;

    static PackageVersion from_string(const ::std::string& s);
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
    ::std::vector<Bound>   m_bounds;

    // TODO: Just upper and lower?
    static PackageVersionSpec from_string(const ::std::string& s);
};

class PackageRef
{
    friend class PackageManifest;
    ::std::string   m_name;
    PackageVersionSpec  m_version;

    ::std::string   m_path;
    ::std::shared_ptr<PackageManifest> m_manifest;

    PackageRef(const ::std::string& n) :
        m_name(n)
    {
    }

public:
    const PackageManifest& get_package() const;
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
    ::std::string   m_manmifest_path;

    ::std::string   m_name;
    PackageVersion  m_version;

    ::std::vector<PackageRef>   m_dependencies;

    ::std::vector<PackageTarget>    m_targets;

    struct BuildScript
    {
    };

    PackageManifest();
public:
    static PackageManifest load_from_toml(const ::std::string& path);
    void build_lib() const;

    const ::std::vector<PackageRef>& dependencies() const {
        return m_dependencies;
    }
};
