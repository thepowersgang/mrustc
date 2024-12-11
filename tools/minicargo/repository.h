/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * repository.h
 * - Handling (vendored) crates.io dependencies
 */
#pragma once

#include <string>
#include <map>
#include "helpers.h"
#include "manifest.h"

class Repository
{
    struct Entry
    {
        /// Path to the Cargo.toml file in the package root
        ::std::string   manifest_path;
        /// Package version
        PackageVersion  version;
        /// (Cached) loaded manifest
        ::std::shared_ptr<PackageManifest>  loaded_manifest;
        /// Indicates that the package shouldn't be considered by `find`
        bool blacklisted;

        Entry(): blacklisted(false) {}
    };

    ::std::multimap<::std::string, Entry>    m_cache;
    // path => manifest
    ::std::map<::std::string, ::std::shared_ptr<PackageManifest>>   m_path_cache;
    const WorkspaceManifest*    m_workspace_manifest = nullptr;
public:
    void load_cache(const ::helpers::path& path);
    void load_vendored(const ::helpers::path& path);

    void set_workspace(const WorkspaceManifest& wm) { m_workspace_manifest = &wm; }

    void add_patch_path(const std::string& package_name, ::helpers::path path);
    /// Mark a dependency to be excluded from calls to `find`
    void blacklist_dependency(const PackageManifest* dep_ptr);

    ::std::shared_ptr<PackageManifest> from_path(::helpers::path path);
    ::std::shared_ptr<PackageManifest> find(const ::std::string& name, const PackageVersionSpec& version);
};
