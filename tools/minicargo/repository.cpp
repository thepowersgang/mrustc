/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * repository.cpp
 * - Handling of (vendored) crates.io dependencies
 */
#include "repository.h"
#include "debug.h"
#include <fstream>
#if _WIN32
# include <Windows.h>
#else
# include <dirent.h>
#endif
#include "toml.h"

void Repository::load_cache(const ::helpers::path& path)
{
    throw ::std::runtime_error("TODO: Repository::load_cache");
}
void Repository::load_vendored(const ::helpers::path& path)
{
    // Enumerate folders in this folder, try to open Cargo.toml files
    // Extract package name and version from each manifest
    #if _WIN32
    WIN32_FIND_DATA find_data;
    HANDLE find_handle = FindFirstFile( (path / "*").str().c_str(), &find_data );
    if( find_handle == INVALID_HANDLE_VALUE )
        throw ::std::runtime_error(::format( "Unable to open vendor directory '", path, "'" ));
    do
    {
        auto manifest_path = path / find_data.cFileName / "Cargo.toml";
    #else
    auto* dp = opendir(path.str().c_str());
    if( dp == nullptr )
        throw ::std::runtime_error(::format( "Unable to open vendor directory '", path, "'" ));
    while( const auto* dent = readdir(dp) )
    {
        auto manifest_path = path / dent->d_name / "Cargo.toml";
    #endif

        if( ! ::std::ifstream(manifest_path.str()).good() )
            continue ;
        //DEBUG("Opening manifest " << manifest_path);

        // Scan the manifiest until both the name and version are set
        bool name_set = false;
        ::std::string   name;
        bool ver_set = false;
        PackageVersion  ver;

        TomlFile toml_file(manifest_path);
        for(auto key_val : toml_file)
        {
            if(key_val.path.size() != 2)
                continue ;
            if(key_val.path[0] == "package") {
                if( key_val.path[1] == "name" ) {
                    //assert( !name_set );
                    name = key_val.value.as_string();
                    name_set = true;
                    if( name_set && ver_set )
                        break;
                }
                else if( key_val.path[1] == "version" ) {
                    //assert( !ver_set );
                    ver = PackageVersion::from_string(key_val.value.as_string());
                    ver_set = true;
                    if( name_set && ver_set )
                        break;
                }
                else
                    ;
            }
        }

        DEBUG("Vendored package '" << name << "' v" << ver);
        if(name == "")
            continue ;

        Entry   cache_ent;
        cache_ent.manifest_path = manifest_path;
        cache_ent.version = ver;
        m_cache.insert(::std::make_pair( name, ::std::move(cache_ent) ));

    #ifndef _WIN32
    }
    closedir(dp);
    #else
    } while( FindNextFile(find_handle, &find_data) );
    FindClose(find_handle);
    #endif
    DEBUG("Loaded " << m_cache.size() << " vendored packages");
}

::std::shared_ptr<PackageManifest> Repository::from_path(::helpers::path in_path)
{
    DEBUG("Repository::from_path(" << in_path << ")");
    // 1. Normalise path
    auto path = in_path.normalise();
    DEBUG("path = " << path);

    auto it = m_path_cache.find(path);
    if(it == m_path_cache.end())
    {
        ::std::shared_ptr<PackageManifest> rv ( new PackageManifest(PackageManifest::load_from_toml(path, m_workspace_manifest)) );

        m_path_cache.insert( ::std::make_pair(::std::move(path), rv) );

        if( rv->name().compare(0, 6+4+10, "rustc-std-workspace-", 6+4+10) == 0 && m_cache.count(rv->name()) == 0 ) {
            Entry   cache_ent;
            cache_ent.manifest_path = path;
            cache_ent.loaded_manifest = rv;
            cache_ent.version = rv->version();
            m_cache.insert(::std::make_pair(rv->name(), cache_ent));
        }

        return rv;
    }
    else
    {
        return it->second;
    }
}
void Repository::add_patch_path(const std::string& package_name, ::helpers::path path)
{
    auto manifest_path = path / "Cargo.toml";

    auto loaded_manifest = ::std::shared_ptr<PackageManifest>( new PackageManifest(PackageManifest::load_from_toml(manifest_path, m_workspace_manifest)) );

    Entry   cache_ent;
    cache_ent.manifest_path = manifest_path;
    cache_ent.version = loaded_manifest->version();
    cache_ent.loaded_manifest = ::std::move(loaded_manifest);
    m_cache.insert(::std::make_pair( package_name, ::std::move(cache_ent) ));
    // TODO: If there's other packages with the same name, check for compatability (or otherwise ensure that this is the chosen version)
}
::std::shared_ptr<PackageManifest> Repository::find(const ::std::string& name, const PackageVersionSpec& version, std::function<bool(const PackageVersion&)> filter_cb)
{
    TRACE_FUNCTION_F("FIND " << name << " matching " << version);
    auto itp = m_cache.equal_range(name);

    Entry* best = nullptr;
    for(auto i = itp.first; i != itp.second; ++i)
    {
        if( version.accepts(i->second.version) )
        {
            if( !filter_cb(i->second.version) )
            {
                DEBUG("Filtered out " << i->second.version);
            }
            else if( !best || i->second.version > best->version )
            {
                DEBUG("Accept " << i->second.version);
                best = &i->second;
            }
            else {
                DEBUG("Matched " << i->second.version);
            }
        }
        else
        {
            DEBUG("Ignore " << i->second.version);
        }
    }

    if( best )
    {
        DEBUG("USING " << name << " " << best->version);
        if( !best->loaded_manifest )
        {
            if( best->manifest_path == "" )
            {
                throw ::std::runtime_error("TODO: Download package");
            }
            try
            {
                best->loaded_manifest = ::std::shared_ptr<PackageManifest>( new PackageManifest(PackageManifest::load_from_toml(best->manifest_path, m_workspace_manifest)) );
            }
            catch(const ::std::exception& e)
            {
                throw ::std::runtime_error( format("Error loading manifest '", best->manifest_path, "' - ", e.what()) );
            }
        }

        return best->loaded_manifest;
    }
    else
    {
        return {};
    }
}
