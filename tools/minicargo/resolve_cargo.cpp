/**
 * Clone of cargo's resolver
 * 
 * See https://doc.rust-lang.org/cargo/reference/resolver.html
 */
#include "resolve.h"
#include "repository.h"
#include <debug.h>

#include <cassert>
#include <set>
#include <algorithm>    // std::sort

struct DepSpec {
    LockfileContents::PackageKey    source;
    std::string package_name;
    PackageVersionSpec  version_spec;

    DepSpec(): source({},PackageVersion()) {}
    DepSpec(LockfileContents::PackageKey source, const std::string& package_name, PackageVersionSpec version_spec)
        : source(source)
        , package_name(package_name)
        , version_spec(version_spec)
    {}
};
struct DepSpecQueue {
    std::vector<DepSpec> stack;
};
class Policy {
public:
    bool pick_next_dep(DepSpec& out, DepSpecQueue& queue) const {
        if( queue.stack.empty() ) {
            return false;
        }
        else {
            out = std::move(queue.stack.back());
            queue.stack.pop_back();
            return true;
        }
    }
    /// Check if the provided dependency specification is met by an existing package version in the output
    bool try_unify_version(LockfileContents& resolved, const DepSpec& dep_spec) const {
        for(const auto& v : resolved.package_deps) {
            if(v.first.name == dep_spec.package_name) {
                if(dep_spec.version_spec.accepts(v.first.version)) {
                    resolved.package_deps[dep_spec.source].insert(v.first);
                    return true;
                }
            }
        }
        return false;
    }
    /// Filter package versions according to policy restrictions, and sort the output list
    std::vector<PackageVersion> filter_versions(const DepSpec& dep_spec, std::vector<PackageVersion> dep_versions) const {
        ::std::sort(dep_versions.begin(), dep_versions.end());
        return dep_versions;
    }

    /// Get the next highest version from the output of `filter_versions` (which sorts its output)
    const PackageManifest* pick_next_version(Repository& repo, const DepSpec& dep_spec,  std::vector<PackageVersion>& available_versions) const {
        if( available_versions.empty() ) {
            return nullptr;
        }
        auto sel_version = available_versions.back();
        available_versions.pop_back();
        auto ret = repo.get_package(dep_spec.package_name, sel_version);
        return ret.get();
    }
    /// ? Checks if this package version is compatible with one already selected
    ///
    /// E.g. A package with `foo = "1"` is loaded, bringing in foo 1.2, but then `foo = "1.1"` is seen. This should indicate unification require
    bool needs_version_unification(const std::string& package_name, const PackageVersion& version, const LockfileContents& resolved) const {
        for(const auto& v : resolved.package_deps) {
            if(v.first.name == package_name) {
                // An exact match means that the `try_unify_version` call didn't work
                assert( version != v.first.version );

                // Two versions need to unify if they're they're the same patch level.
                if( version.prev_compat() == v.first.version.prev_compat() ) {
                    DEBUG("Found " << package_name << " v" << v.first.version);
                    return true;
                }
                // Any other cases?
                if( version.next_breaking() == v.first.version.next_breaking() ) {
                    // These two are semver compatible, so need to be unified
                    DEBUG("Found " << package_name << " v" << v.first.version);
                    return true;
                }
            }
        }
        return false;
    }
};

static void push_dependencies(const PackageManifest& package, LockfileContents& resolved, Repository& repo, DepSpecQueue& queue)
{
    TRACE_FUNCTION_F(package.name() << " v" << package.version());
    LockfileContents::PackageKey    package_key { package.name(), package.version() };
    resolved.package_deps[package_key];
    std::vector<const PackageRef*>  deps;
    package.iter_main_dependencies([&](const PackageRef& dep_c) {
        deps.push_back(&dep_c);
    });
    package.iter_build_dependencies([&](const PackageRef& dep_c) {
        deps.push_back(&dep_c);
    });
    for(const auto* dep_c : deps) {
        if( dep_c->get_version().m_bounds.empty() ) {
            DEBUG("Recurse " << dep_c->name());
            auto m = const_cast<PackageRef*>(dep_c)->load_manifest_raw(repo, package.directory(), [](const auto&){ return true; });
            LockfileContents::PackageKey    dep_key { m->name(), m->version() };
            if( resolved.package_deps[package_key].insert(dep_key).second ) {
                push_dependencies(*m, resolved, repo, queue);
            }
        }
        else {
            DEBUG("PUSH " << dep_c->name() << " " << dep_c->get_version());
            queue.stack.push_back(DepSpec(package_key, dep_c->name(), dep_c->get_version()));
        }
    }
}

// TODO: Flatten this call tree, with a generation counter for every time it has to roll back
// - Have a stack of rollback states, pushed when `dep_versions` has multiple items
// - When resolution would fail, pop from the saved state and restore to that
struct SavedState {
    DepSpec dep_spec;
    ::std::vector<PackageVersion> avail_versions;
    LockfileContents  saved_out;
    DepSpecQueue    saved_queue;
};

static bool resolve_next(LockfileContents& out, Repository& repo, DepSpecQueue& queue, const Policy& policy)
{
    DepSpec dep_spec;
    // Pop the next dependency to resolve, returning success if we're done.
    if( !policy.pick_next_dep(dep_spec, queue) ) {
        return true;
    }
    TRACE_FUNCTION_F("[FOR " << dep_spec.source.name << " v" << dep_spec.source.version << "] " << dep_spec.package_name << " " << dep_spec.version_spec);

    // Could this dependency be unified with an existing dependency? If yes, return now
    if( policy.try_unify_version(out, dep_spec) ) {
        DEBUG("Unifies");
        return resolve_next(out, repo, queue, policy);
    }

    // Get possible versions matching this spec
    auto avail_versions = repo.enum_matching_versions(dep_spec.package_name, dep_spec.version_spec);
    if( avail_versions.empty() ) {
        // libstd 1.90 depends on miniz_oxide, which has an optional dep on simd-adler32, which isn't vendor
        DEBUG("No versions: soft-fail");
        return resolve_next(out, repo, queue, policy);
    }
    auto dep_versions = policy.filter_versions(dep_spec, std::move(avail_versions));

    while( const auto* dep_package = policy.pick_next_version(repo, dep_spec, dep_versions) )
    {
        DEBUG("Try " << dep_package->version());
        // ?? If this version conflicts with an existing version?
        if( policy.needs_version_unification(dep_spec.package_name, dep_package->version(), out) ) {
            DEBUG("Unification required");
            continue ;
        }

        LockfileContents::PackageKey    dep_key { dep_spec.package_name, dep_package->version() };
        // Fork state
        auto saved_out = out;
        auto saved_queue = queue;
        if( out.package_deps[dep_spec.source].insert(dep_key).second ) {
            push_dependencies(*dep_package, out, repo, queue);
        }
        // Ensure that this package is in the output too
        out.package_deps[dep_key];
        if( resolve_next(out, repo, queue, policy) ) {
            return true;
        }
        out = std::move(saved_out);
        queue = std::move(saved_queue);
    }
    // If the above loop exits, no compatible versions were found
    DEBUG("FAIL");
    return false;
}

LockfileContents ResolveDependencies_Cargo(Repository& repo, const PackageManifest& root_manifest, unsigned version)
{
    LockfileContents    rv;

    // Enumeration state
    DepSpecQueue    queue;
    Policy  policy;
    // versions:
    // - 1: Standard
    // - 2: Feature unification changes
    // - 3: changes default fallback behaviour

    // Push dependencies from the root package
    push_dependencies(root_manifest, rv, repo, queue);
    // Start resolution
    if( !resolve_next(rv, repo, queue, policy) ) {
        // Error!
        throw std::runtime_error("Failed to resolve dependencies");
    }
    assert( queue.stack.empty() );
    return rv;
}