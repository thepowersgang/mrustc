/*
 */
#include "repository.h"
#include "debug.h"

void Repository::load_cache(const ::helpers::path& path)
{
    throw "";
}
void Repository::load_vendored(const ::helpers::path& path)
{
    // Enumerate folders in this folder, try to open Cargo.toml files
    // Extract package name and version from each manifest
}

::std::shared_ptr<PackageManifest> Repository::from_path(::helpers::path path)
{
    DEBUG("Repository::from_path(" << path << ")");
    // 1. Normalise path
    path = path.normalise();
    DEBUG("path = " << path);

    auto it = m_path_cache.find(path);
    if(it == m_path_cache.end())
    {
        ::std::shared_ptr<PackageManifest> rv ( new PackageManifest(PackageManifest::load_from_toml(path)) );

        m_path_cache.insert( ::std::make_pair(::std::move(path), rv) );

        return rv;
    }
    else
    {
        return it->second;
    }
}
::std::shared_ptr<PackageManifest> Repository::find(const ::std::string& name, const PackageVersionSpec& version)
{
    auto itp = m_cache.equal_range(name);

    Entry* best = nullptr;
    for(auto i = itp.first; i != itp.second; ++i)
    {
        if( version.accepts(i->second.version) )
        {
            if( !best || best->version < i->second.version )
            {
                best = &i->second;
            }
        }
    }

    if( best )
    {
        if( !best->loaded_manifest )
        {
            if( best->manifest_path == "" )
            {
                throw "TODO: Download package";
            }
            best->loaded_manifest = ::std::shared_ptr<PackageManifest>( new PackageManifest(PackageManifest::load_from_toml(best->manifest_path)) );
        }

        return best->loaded_manifest;
    }
    else
    {
        return {};
    }
}
