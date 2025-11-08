/**
 * 
 */
#include "resolve.h"
#include "repository.h"
#include <debug.h>

#include <set>

struct SemverRelease {
    unsigned level;
    unsigned version;
    static SemverRelease for_package(const PackageVersion& ver) {
        if( ver.major != 0 ) {
            return SemverRelease { 0, ver.major };
        }
        if( ver.minor != 0 ) {
            return SemverRelease { 1, ver.minor };
        }
        return SemverRelease { 2, ver.patch };
    }
    int ord(const SemverRelease& x) const {
        if( level != x.level )  return level < x.level ? -1 : 1;
        if( version != x.version )  return version < x.version ? -1 : 1;
        return 0;
    }
    bool operator==(const SemverRelease& x) const {
        return ord(x) == 0;
    }
    bool operator<(const SemverRelease& x) const {
        return ord(x) == -1;
    }
};
struct Rule {
    std::string package;
    SemverRelease   ver;

    static Rule for_manifest(const PackageManifest& m) {
        return Rule { m.name(), SemverRelease::for_package(m.version()) };
    }
    int ord(const Rule& x) const {
        if( package != x.package )  return package < x.package ? -1 : 1;
        return ver.ord(x.ver);
    }
    bool operator==(const Rule& x) const {
        return ord(x) == 0;
    }
    bool operator<(const Rule& x) const {
        return ord(x) == -1;
    }
};

struct LockfileEnumState {
    /// Root package (used to re-seed the stack on `reset`)
    const PackageManifest&  root;

    /// Final lockfile
    LockfileContents    lockfile_contents;

    /// Package seen/selected for each semver version line
    std::map<Rule, const PackageManifest*> selected_versions;
    /// Which manfiests have been seen so far
    std::set<const PackageManifest*> seen_manifests;
    /// Stack of packages to visit
    std::vector<const PackageManifest*> visit_stack;

    /// Package versions that should be excluded, because they lead to version conflicts
    ::std::map<std::string, std::set<PackageVersion>> blacklist;

    LockfileEnumState(const PackageManifest& root): root(root) {
        reset();
    }
    void reset() {
        lockfile_contents.clear();
        selected_versions.clear();
        seen_manifests.clear();
        visit_stack.clear();
        visit_stack.push_back(&root);
    }
    const PackageManifest* pop() {
        if( visit_stack.empty() ) {
            return nullptr;
        }
        else {
            auto rv = visit_stack.back();
            visit_stack.pop_back();
            return rv;
        }
    }

    bool blacklist_dependency(const PackageManifest& package) {
        return blacklist[package.name()].insert(package.version()).second;
    }
};

LockfileContents ResolveDependencies_MinicargoOriginal(Repository& repo, const PackageManifest& root_manifest)
{
    // a list of rules added whenever there's a conflict
    //std::map<Rule, std::vector<PackageVersionSpec>>   rules;

    // Enumeration state
    LockfileEnumState s(root_manifest);
    while(const auto* cur_manifest = s.pop())
    {
        auto base_path = cur_manifest->directory();
        TRACE_FUNCTION_F(cur_manifest->name() << " " << cur_manifest->version() << " (" << base_path << ")");
        auto cur_key = LockfileContents::PackageKey(*cur_manifest);
        // Make sure that this package exists
        s.lockfile_contents.package_deps[ cur_key ];

        // Enumerate all non-dev dependencies (don't care about features in this stage)
        ::std::vector<const PackageRef*>    deps;
        cur_manifest->iter_main_dependencies([&](const PackageRef& dep_c) { deps.push_back(&dep_c); });
        cur_manifest->iter_build_dependencies([&](const PackageRef& dep_c) { deps.push_back(&dep_c); });

        for(const auto* dep_ref : deps)
        {
            bool is_repo = !dep_ref->get_version().m_bounds.empty();
            const auto* dep = const_cast<PackageRef&>(*dep_ref)
                .load_manifest_raw(repo, base_path, [&](const PackageVersion& v){
                    return s.blacklist[dep_ref->name()].count(v) == 0;
                })
                .get();
            if( !dep ) {
                DEBUG("Failed to load dependency: " << *dep_ref);
                continue ;
            }
            s.lockfile_contents.package_deps[ cur_key ].insert( LockfileContents::PackageKey(*dep) );
            if( s.seen_manifests.insert(dep).second )
            {
                s.visit_stack.push_back(dep);

                // If this is from the repository (i.e. not a path/git dependency)
                if( is_repo )
                {
                    // Then check if it conflicts with a previously-selected dependency
                    auto r = s.selected_versions.insert(std::make_pair( Rule::for_manifest(*dep), dep ));
                    const auto* prev_dep = r.first->second;
                    if( prev_dep != dep )
                    {
                        // Uh-oh, conflict
                        // - Check if the existing one is a lower version, if it is then no issue
                        // - Otherwise, we need to blacklist `r.first->second` and restart
                        if( prev_dep->version() < dep->version() ) {
                            DEBUG("Blacklist " << dep->name() << " v" << dep->version() << ": from " << cur_manifest->name() << " v" << cur_manifest->version());
                            s.blacklist_dependency(*dep);
                            // No need to restart
                        }
                        else {
                            DEBUG("Blacklist " << prev_dep->name() << " v" << prev_dep->version()
                                << ": Use " << dep->name() << " v" << dep->version() << " from " << cur_manifest->name() << " v" << cur_manifest->version());
                            if( s.blacklist_dependency(*prev_dep) ) {
                                // Reset the enumeration state, but keep the blacklist
                                s.reset();
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return std::move(s.lockfile_contents);
}