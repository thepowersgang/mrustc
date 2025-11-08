#pragma once
#include "manifest.h"
#include <set>

/// The contents of a parsed (or generated) lockfile
struct LockfileContents {
    struct PackageKey {
        std::string name;
        PackageVersion  version;

        explicit PackageKey(const std::string& name, const PackageVersion& version)
            : name(name)
            , version(version)
        {}
        explicit PackageKey(const PackageManifest& m)
            : PackageKey(m.name(), m.version())
        {}

        int ord(const PackageKey& x) const {
            if( name != x.name )  return name < x.name ? -1 : 1;
            return version.cmp(x.version);
        }
        bool operator==(const PackageKey& x) const {
            return ord(x) == 0;
        }
        bool operator<(const PackageKey& x) const {
            return ord(x) == -1;
        }
    };
    /// Dependencies for each package
    std::map<PackageKey, std::set<PackageKey>>   package_deps;
    void clear() {
        package_deps.clear();
    }
};

extern LockfileContents ResolveDependencies_MinicargoOriginal(Repository& repo, const PackageManifest& root_manifest);
extern LockfileContents ResolveDependencies_Cargo(Repository& repo, const PackageManifest& root_manifest, unsigned version);
