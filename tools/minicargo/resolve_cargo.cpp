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
#include <deque>
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
    std::deque<DepSpec> stack;
};
class Policy {
public:
    bool pick_next_dep(DepSpec& out, DepSpecQueue& queue) const {
        if( queue.stack.empty() ) {
            return false;
        }
        else {
            out = std::move(queue.stack.front());
            queue.stack.pop_front();
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
    std::vector<PackageVersion> filter_versions(const DepSpec& dep_spec, std::vector<PackageVersion> dep_versions, const LockfileContents& resolved) const {
        auto new_end = ::std::remove_if(dep_versions.begin(), dep_versions.end(), [&](const PackageVersion& v) {
            return !dep_spec.version_spec.accepts(v) || this->needs_version_unification(dep_spec.package_name, v, resolved);
        });
        dep_versions.erase(new_end, dep_versions.end());
        // Sort ascending, so pop_back gets the newest version
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
    const LockfileContents::PackageKey* needs_version_unification(const std::string& package_name, const PackageVersion& version, const LockfileContents& resolved) const {
        for(const auto& v : resolved.package_deps) {
            if(v.first.name == package_name) {
                // An exact match means that the `try_unify_version` call didn't work
                assert( version != v.first.version );

                // Two versions need to unify if they're they're the same patch level.
                if( version.prev_compat() == v.first.version.prev_compat() ) {
                    return &v.first;
                }
                // Any other cases?
                if( version.next_breaking() == v.first.version.next_breaking() ) {
                    // These two are semver compatible, so need to be unified
                    return &v.first;
                }
            }
        }
        return nullptr;
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
        //if( dep_c->get_version().m_bounds.empty() ) {
        if( dep_c->has_path() ) {
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

namespace {
    struct InnerState {
        /// Output lock file
        LockfileContents  rv;
        /// Queue of dependencies still to be resolved
        DepSpecQueue    queue;
        /// Stack sizes when each dependency was pushed into output, used to efficently roll back for unification
        std::map<LockfileContents::PackageKey, size_t>  push_depths;
        /// Stack size when a dependency was pushed for resolution, used to roll back selection of a dependency
        std::map<std::pair<LockfileContents::PackageKey,std::string>, size_t>  push_depths1;
    };
    struct StackEntry {
        /// @brief Triggering dependency specification
        DepSpec dep_spec;
        /// Remaining available dependency versions
        ::std::vector<PackageVersion> dep_versions;

        /// @brief Saved inner resolver state
        InnerState  saved;
    };

    struct State {
        Repository& repo;
        /// @brief Policy (configuration) for making decisions
        Policy  policy;
        /// @brief Saveable inner state
        InnerState  inner;

        /// @brief Stack of saved state information for when a descision was made
        std::vector<StackEntry> backtrack_stack;

        void push_backtrack(DepSpec dep_spec, ::std::vector<PackageVersion> dep_versions)
        {
            backtrack_stack.push_back(StackEntry { dep_spec, dep_versions, inner });
        }
        // Pop backtrack entries until we reach the entry that pushed this version
        void pop_to(const LockfileContents::PackageKey& triggering_source, const LockfileContents::PackageKey& triggering_version2)
        {
            #if 1
            // TODO: `triggering_source` isn't quite right, really this should just not be needed
            // BAD ASSUMPTION: The bad decision might be on whatever package caused the conflicting requirement
            TRACE_FUNCTION_F(triggering_source << " ,  " << triggering_version2);
            auto stack_size1 = (triggering_source.name == triggering_version2.name
                ? 0
                : this->inner.push_depths1.at(::std::make_pair(triggering_source, triggering_version2.name))
                );
            auto stack_size2 = this->inner.push_depths.at(triggering_version2);
            DEBUG(stack_size1 << "," << stack_size2 << "/" << this->backtrack_stack.size());
            auto stack_size = ::std::max(stack_size1, stack_size2);
            assert(stack_size <= this->backtrack_stack.size());
            while( this->backtrack_stack.size() > stack_size ) {
                const auto& v = this->backtrack_stack.back();
                DEBUG("-- " << v.dep_spec.source << " - " << v.dep_spec.package_name << " [" << v.dep_versions << "]");
                this->backtrack_stack.pop_back();
            }
            #endif
        }
        bool backtrack(const LockfileContents::PackageKey& triggering_version1, const LockfileContents::PackageKey* triggering_version2)
        {
            TRACE_FUNCTION_F("");
            if(triggering_version2) {
                this->pop_to(triggering_version1, *triggering_version2);
            }
            else {
                this->pop_to(triggering_version1, triggering_version1);
            }
            for(const auto& v : backtrack_stack) {
                DEBUG(">> " << v.dep_spec.source << " - " << v.dep_spec.package_name << " [" << v.dep_versions << "]");
            }
            while( !backtrack_stack.empty() )
            {
                auto& ent = backtrack_stack.back();
                DEBUG(ent.dep_spec.source << ": " << ent.dep_spec.package_name << " " << ent.dep_spec.version_spec << " - [" << ent.dep_versions << "]");
                const auto* dep_package = policy.pick_next_version(repo, ent.dep_spec, ent.dep_versions);
                if( !dep_package ) {
                    DEBUG("No filtered versions: hard-fail and backtrack");
                    backtrack_stack.pop_back();
                }
                else {
                    inner = ent.saved;
                    
                    if( const auto* v = try_push_package(ent.dep_spec, *dep_package) ) {
                        this->pop_to(ent.dep_spec.source, *v);
                        //backtrack_stack.pop_back();
                    }
                    else {
                        return true;
                    }
                }
            }
            return false;
        }

        const LockfileContents::PackageKey* try_push_package(const DepSpec& dep_spec, const PackageManifest& dep_package)
        {
            DEBUG("Try " << dep_package.name() << " v" << dep_package.version() << " for " << dep_spec.source);
            // ?? If this version conflicts with an existing version?
            if( const auto* v = policy.needs_version_unification(dep_spec.package_name, dep_package.version(), inner.rv) ) {
                DEBUG("FAIL: Unification required between "
                    << dep_spec.package_name << " v" << dep_package.version() << " and existing v" << v->version);
                // Caller calls `backtrack`
                return v;
            }

            LockfileContents::PackageKey    dep_key { dep_spec.package_name, dep_package.version() };
            this->inner.push_depths.insert(std::make_pair(dep_key, this->backtrack_stack.size()));
            if( inner.rv.package_deps[dep_spec.source].insert(dep_key).second ) {
                // Only push if not already in the output (avoids double-visiting)
                if( inner.rv.package_deps.count(dep_key) == 0 ) {
                    push_dependencies(dep_package, inner.rv, repo, inner.queue);
                    //inner.push_depths1[dep_key] = this->backtrack_stack.size();
                }
                else {
                    DEBUG("Already visited " << dep_key);
                }
            }
            else {
                DEBUG("BUG? " << dep_key << " already a dependency of " << dep_spec.source);
            }
            return nullptr;
        }
    };
}

LockfileContents ResolveDependencies_Cargo(Repository& repo, const PackageManifest& root_manifest, unsigned version)
{
    State   state { repo };
    
    // versions:
    // - 1: Standard
    // - 2: Feature unification changes
    // - 3: changes default fallback behaviour

    // Push dependencies from the root package
    push_dependencies(root_manifest, state.inner.rv, repo, state.inner.queue);
    for(;;)
    {
        DepSpec dep_spec;
        // Pop the next dependency to resolve, returning success if we're done.
        if( !state.policy.pick_next_dep(dep_spec, state.inner.queue) ) {
            break ;
        }
        TRACE_FUNCTION_F("[FOR " << dep_spec.source << "] " << dep_spec.package_name << " " << dep_spec.version_spec);
        // Ensure that this package is in the output too
        state.inner.rv.package_deps[dep_spec.source];

        // Could this dependency be unified with an existing dependency? If yes, return now
        if( state.policy.try_unify_version(state.inner.rv, dep_spec) ) {
            DEBUG("Unifies");
            continue ;
        }

        // Get possible versions matching this spec
        auto avail_versions = repo.enum_matching_versions(dep_spec.package_name, dep_spec.version_spec);
        if( avail_versions.empty() ) {
            // libstd 1.90 depends on miniz_oxide, which has an optional dep on simd-adler32, which isn't vendor
            DEBUG("No versions: soft-fail");
            continue ;
        }
        auto dep_versions = state.policy.filter_versions(dep_spec, std::move(avail_versions), state.inner.rv);
        DEBUG(dep_spec.source << ": " << dep_spec.package_name << " " << dep_spec.version_spec << " - [" << dep_versions << "]");
        // TODO: Filter out versions that would need to be unified (avoiding later backtracks)

        // Grab the first version, and prepare
        const auto* dep_package = state.policy.pick_next_version(repo, dep_spec, dep_versions);
        if( !dep_package ) {
            DEBUG("FAIL: No versions matching " << dep_spec.package_name << " " << dep_spec.version_spec << " [for " << dep_spec.source << "]");
            // TODO: Roll back to whoever selected the source package?
            if( !state.backtrack(dep_spec.source, nullptr) ) {
                throw ::std::runtime_error("Dependency resolution failed");
            }
            continue ;
        }

        // If there are any remaining versions, then push a backtrack entry
        state.inner.push_depths1[std::make_pair(dep_spec.source, dep_spec.package_name)] = state.backtrack_stack.size()+1;
        state.push_backtrack(dep_spec, std::move(dep_versions));

        // Record the stack depth after potential save
        DEBUG(" - " << state.backtrack_stack.size());

        if( const auto* v = state.try_push_package(dep_spec, *dep_package) ) {
            if( !state.backtrack(dep_spec.source, v) ) {
                throw ::std::runtime_error("Dependency resolution failed");
            }
            continue ;
        }
        DEBUG("Good");
    }
    assert( state.inner.queue.stack.empty() );
    return std::move(state.inner.rv);
}