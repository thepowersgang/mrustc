/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * build.cpp
 * - Logic related to invoking the compiler
 */
#ifdef _MSC_VER
# define _CRT_SECURE_NO_WARNINGS    // Allows use of getenv (this program doesn't set env vars)
#endif

#if defined(__MINGW32__)
# define DISABLE_MULTITHREAD    // Mingw32 doesn't have c++11 threads
// Mingw doesn't define putenv()
extern "C" {
extern int _putenv_s(const char*, const char*);
}
#endif

#include "manifest.h"
#include "build.h"
#include "debug.h"
#include "stringlist.h"
#include <vector>
#include <algorithm>
#include <sstream>  // stringstream
#include <cstdlib>  // setenv
#ifndef DISABLE_MULTITHREAD
# include <thread>
# include <mutex>
# include <condition_variable>
#endif
#include <climits>
#include <cassert>
#ifdef _WIN32
# include <Windows.h>
#else
# include <unistd.h>    // getcwd/chdir
# include <spawn.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/wait.h>
# include <fcntl.h>
#endif

#ifdef _WIN32
# define EXESUF ".exe"
# ifdef _MSC_VER
#  define HOST_TARGET "x86_64-windows-msvc"
# elif defined(__MINGW32__)
#  define HOST_TARGET "x86_64-windows-gnu"
# else
# endif
#elif defined(__NetBSD__)
# define EXESUF ""
# define HOST_TARGET "x86_64-unknown-netbsd"
#else
# define EXESUF ""
# define HOST_TARGET "x86_64-unknown-linux-gnu"
#endif

/// Class abstracting access to the compiler
class Builder
{
    BuildOptions    m_opts;
    ::helpers::path m_compiler_path;

public:
    Builder(BuildOptions opts);

    bool build_target(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host) const;
    bool build_library(const PackageManifest& manifest, bool is_for_host) const;
    ::helpers::path build_build_script(const PackageManifest& manifest, bool is_for_host, bool* out_is_rebuilt) const;

private:
    ::helpers::path get_crate_path(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host, const char** crate_type, ::std::string* out_crate_suffix) const;
    bool spawn_process_mrustc(const StringList& args, StringListKV env, const ::helpers::path& logfile) const;
    bool spawn_process(const char* exe_name, const StringList& args, const StringListKV& env, const ::helpers::path& logfile) const;

    ::helpers::path build_and_run_script(const PackageManifest& manifest, bool is_for_host) const;

    // If `is_for_host` and cross compiling, use a different directory
    // - TODO: Include the target arch in the output dir too?
    ::helpers::path get_output_dir(bool is_for_host) const {
        if(is_for_host && m_opts.target_name != nullptr)
            return m_opts.output_dir / "host";
        else
            return m_opts.output_dir;
    }
};

class Timestamp
{
#if _WIN32
    uint64_t m_val;

    Timestamp(FILETIME ft):
        m_val( (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime) )
    {
    }
#else
    time_t  m_val;
    Timestamp(time_t t):
        m_val(t)
    {
    }
#endif

public:
    static Timestamp for_file(const ::helpers::path& p);
    static Timestamp infinite_past() {
#if _WIN32
        return Timestamp { FILETIME { 0, 0 } };
#else
        return Timestamp { 0 };
#endif
    }

    bool operator==(const Timestamp& x) const {
        return m_val == x.m_val;
    }
    bool operator<(const Timestamp& x) const {
        return m_val < x.m_val;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Timestamp& x) {
#if _WIN32
        os << ::std::hex << x.m_val << ::std::dec;
#else
        os << x.m_val;
#endif
        return os;
    }
};

BuildList::BuildList(const PackageManifest& manifest, const BuildOptions& opts):
    m_root_manifest(manifest)
{
    struct ListBuilder {
        struct Ent {
            const PackageManifest* package;
            bool    native;
            unsigned level;
            };
        ::std::vector<Ent>  m_list;

        void add_package(const PackageManifest& p, unsigned level, bool include_build, bool is_native)
        {
            TRACE_FUNCTION_F(p.name());
            // TODO: If this is a proc macro, set `is_native`
            // If the package is already loaded
            for(auto& ent : m_list)
            {
                if(ent.package == &p && ent.native == is_native && ent.level >= level)
                {
                    // NOTE: Only skip if this package will be built before we needed (i.e. the level is greater)
                    return ;
                }
                // Keep searching (might already have a higher entry)
            }
            m_list.push_back({ &p, is_native, level });
            add_dependencies(p, level, include_build, is_native);
        }
        void add_dependencies(const PackageManifest& p, unsigned level, bool include_build, bool is_native)
        {
            for (const auto& dep : p.dependencies())
            {
                if( dep.is_disabled() )
                {
                    continue ;
                }
                DEBUG(p.name() << ": Dependency " << dep.name());
                add_package(dep.get_package(), level+1, include_build, is_native);
            }

            if( p.build_script() != "" && include_build )
            {
                for(const auto& dep : p.build_dependencies())
                {
                    if( dep.is_disabled() )
                    {
                        continue ;
                    }
                    DEBUG(p.name() << ": Build Dependency " << dep.name());
                    add_package(dep.get_package(), level+1, true, true);
                }
            }
        }
        void sort_list()
        {
            ::std::sort(m_list.begin(), m_list.end(), [](const auto& a, const auto& b){ return a.level > b.level; });

            // Needed to deduplicate after sorting (`add_package` doesn't fully dedup)
            for(auto it = m_list.begin(); it != m_list.end(); )
            {
                auto it2 = ::std::find_if(m_list.begin(), it, [&](const auto& x){ return x.package == it->package; });
                if( it2 != it )
                {
                    DEBUG((it - m_list.begin()) << ": Duplicate " << it->package->name() << " - Already at pos " << (it2 - m_list.begin()));
                    it = m_list.erase(it);
                }
                else
                {
                    DEBUG((it - m_list.begin()) << ": Keep " << it->package->name() << ", level = " << it->level);
                    ++it;
                }
            }
        }
    };

    bool cross_compiling = (opts.target_name != nullptr);
    ListBuilder b;
    b.add_dependencies(manifest, 0, !opts.build_script_overrides.is_valid(), !cross_compiling);
    if( manifest.has_library() )
    {
        b.m_list.push_back({ &manifest, !cross_compiling, 0 });
    }

    // TODO: Add the binaries too?
    // - They need slightly different treatment.

    b.sort_list();


    // Move the contents of the above list to this class's list
    m_list.reserve(b.m_list.size());
    for(const auto& e : b.m_list)
    {
        m_list.push_back({ e.package, e.native, {} });
    }
    // Fill in all of the dependents (i.e. packages that will be closer to being buildable when the package is built)
    for(size_t i = 0; i < m_list.size(); i++)
    {
        const auto* cur = m_list[i].package;
        for(size_t j = i+1; j < m_list.size(); j ++)
        {
            const auto& p = *m_list[j].package;
            for( const auto& dep : p.dependencies() )
            {
                if( !dep.is_disabled() && &dep.get_package() == cur )
                {
                    m_list[i].dependents.push_back(static_cast<unsigned>(j));
                }
            }
            if( p.build_script() != "" && !opts.build_script_overrides.is_valid() )
            {
                for(const auto& dep : p.build_dependencies())
                {
                    if( !dep.is_disabled() && &dep.get_package() == cur )
                    {
                        m_list[i].dependents.push_back(static_cast<unsigned>(j));
                    }
                }
            }
        }
    }
}
bool BuildList::build(BuildOptions opts, unsigned num_jobs)
{
    bool include_build = !opts.build_script_overrides.is_valid();
    Builder builder { ::std::move(opts) };

    // Pre-count how many dependencies are remaining for each package
    struct BuildState
    {
        ::std::vector<unsigned> num_deps_remaining;
        ::std::vector<unsigned> build_queue;

        int complete_package(unsigned index, const ::std::vector<Entry>& list)
        {
            int rv = 0;
            DEBUG("Completed " << list[index].package->name() << " (" << list[index].dependents.size() << " dependents)");

            for(auto d : list[index].dependents)
            {
                assert(this->num_deps_remaining[d] > 0);
                this->num_deps_remaining[d] --;
                DEBUG("- " << list[d].package->name() << " has " << this->num_deps_remaining[d] << " deps remaining");
                if( this->num_deps_remaining[d] == 0 )
                {
                    rv ++;
                    this->build_queue.push_back(d);
                }
            }

            return rv;
        }

        unsigned get_next()
        {
            assert(!this->build_queue.empty());
            unsigned rv = this->build_queue.back();
            this->build_queue.pop_back();
            return rv;
        }
    };
    BuildState  state;
    state.num_deps_remaining.reserve(m_list.size());
    for(const auto& e : m_list)
    {
        auto idx = static_cast<unsigned>(state.num_deps_remaining.size());
        const auto& p = *e.package;
        unsigned n_deps = 0;
        for (const auto& dep : p.dependencies())
        {
            if( dep.is_disabled() )
            {
                continue ;
            }
            n_deps ++;
        }

        if( p.build_script() != "" && include_build )
        {
            for(const auto& dep : p.build_dependencies())
            {
                if( dep.is_disabled() )
                {
                    continue ;
                }
                n_deps ++;
            }
        }
        // If there's no dependencies for this package, add it to the build queue
        if( n_deps == 0 )
        {
            state.build_queue.push_back(idx);
        }
        DEBUG("Package '" << p.name() << "' has " << n_deps << " dependencies and " << m_list[idx].dependents.size() << " dependents");
        state.num_deps_remaining.push_back( n_deps );
    }

    // Actually do the build
    if( num_jobs > 1 )
    {
#ifndef DISABLE_MULTITHREAD
        class Semaphore
        {
            ::std::mutex    mutex;
            ::std::condition_variable   condvar;
            unsigned    count = 0;
        public:
            void notify_max() {
                ::std::lock_guard<::std::mutex> lh { this->mutex };
                count = UINT_MAX;
                condvar.notify_all();
            }
            void notify() {
                ::std::lock_guard<::std::mutex> lh { this->mutex };
                if( count == UINT_MAX )
                    return;
                ++ count;
                condvar.notify_one();
            }
            void wait() {
                ::std::unique_lock<::std::mutex> lh { this->mutex };
                while( count == 0 )
                {
                    condvar.wait(lh);
                }
                if( count == UINT_MAX )
                    return;
                -- count;
            }
        };
        struct Queue
        {
            Semaphore   dead_threads;
            Semaphore   avaliable_tasks;

            ::std::mutex    mutex;
            BuildState  state;

            unsigned    num_active;
            bool    failure;
            bool    complete;   // Set if num_active==0 and tasks.empty()

            Queue(BuildState x):
                state(::std::move(x)),
                num_active(0),
                failure(false),
                complete(false)
            {
            }

            void signal_all() {
                avaliable_tasks.notify_max();
            }
        };
        struct H {
            static void thread_body(unsigned my_idx, const ::std::vector<Entry>* list_p, Queue* queue_p, const Builder* builder)
            {
                const auto& list = *list_p;
                auto& queue = *queue_p;
                for(;;)
                {
                    DEBUG("Thread " << my_idx << ": waiting");
                    queue.avaliable_tasks.wait();

                    if( queue.complete || queue.failure )
                    {
                        DEBUG("Thread " << my_idx << ": Terminating");
                        break;
                    }

                    unsigned cur;
                    {
                        ::std::lock_guard<::std::mutex> sl { queue.mutex };
                        cur = queue.state.get_next();
                        queue.num_active ++;
                    }

                    DEBUG("Thread " << my_idx << ": Starting " << cur << " - " << list[cur].package->name());
                    if( ! builder->build_library(*list[cur].package, list[cur].is_host) )
                    {
                        queue.failure = true;
                        queue.signal_all();
                    }
                    else
                    {
                        ::std::lock_guard<::std::mutex> sl { queue.mutex };
                        queue.num_active --;
                        int v = queue.state.complete_package(cur, list);
                        while(v--)
                        {
                            queue.avaliable_tasks.notify();
                        }

                        // If the queue is empty, and there's no active jobs, stop.
                        if( queue.state.build_queue.empty() && queue.num_active == 0 )
                        {
                            queue.complete = true;
                            queue.signal_all();
                        }
                    }
                }

                queue.dead_threads.notify();
            }
        };
        Queue   queue { state };


        ::std::vector<::std::thread>    threads;
        threads.reserve(num_jobs);
        DEBUG("Spawning " << num_jobs << " worker threads");
        for(unsigned i = 0; i < num_jobs; i++)
        {
            threads.push_back(::std::thread(H::thread_body, i, &this->m_list, &queue, &builder));
        }

        DEBUG("Poking jobs");
        for(auto i = queue.state.build_queue.size(); i --;)
        {
            queue.avaliable_tasks.notify();
        }

        // All jobs are done, wait for each thread to complete
        for(unsigned i = 0; i < num_jobs; i++)
        {
            threads[i].join();
            DEBUG("> Thread " << i << " complete");
        }

        if( queue.failure )
        {
            return false;
        }
        state = ::std::move(queue.state);
#else
        while( !state.build_queue.empty() )
        {
            auto cur = state.get_next();

            if( ! builder.build_library(*m_list[cur].package, m_list[cur].is_host) )
            {
                return false;
            }
            state.complete_package(cur, m_list);
        }
#endif
    }
    else if( num_jobs == 1 )
    {
        while( !state.build_queue.empty() )
        {
            auto cur = state.get_next();

            if( ! builder.build_library(*m_list[cur].package, m_list[cur].is_host) )
            {
                return false;
            }
            state.complete_package(cur, m_list);
        }
    }
    else
    {
        ::std::cout << "DRY RUN BUILD" << ::std::endl;
        unsigned pass = 0;
        while( !state.build_queue.empty() )
        {
            auto queue = ::std::move(state.build_queue);
            for(auto idx : queue)
            {
                ::std::cout << pass << ": " << m_list[idx].package->name() << ::std::endl;
            }
            for(auto idx : queue)
            {
                state.complete_package(idx, m_list);
            }
            pass ++;
        }
        // TODO: Binaries?
        return false;
    }

    // DEBUG ASSERT
    {
        bool any_incomplete = false;
        for(size_t i = 0; i < state.num_deps_remaining.size(); i ++)
        {
            if( state.num_deps_remaining[i] != 0 ) {
                ::std::cerr << "BUG: Package '" << m_list[i].package->name() << "' still had " << state.num_deps_remaining[i] << " dependecies still to be built" << ::std::endl;
                any_incomplete = true;
            }
        }
        if( any_incomplete ) {
            throw ::std::runtime_error("Incomplete packages (still have dependencies remaining)");
        }
    }

    // Now that all libraries are done, build the binaries (if present)
    return this->m_root_manifest.foreach_binaries([&](const auto& bin_target) {
        return builder.build_target(this->m_root_manifest, bin_target, /*is_for_host=*/false);
        });
}


Builder::Builder(BuildOptions opts):
    m_opts(::std::move(opts))
{
#ifdef _WIN32
    char buf[1024];
    size_t s = GetModuleFileName(NULL, buf, sizeof(buf)-1);
    buf[s] = 0;

    ::helpers::path minicargo_path { buf };
    minicargo_path.pop_component();
#ifdef __MINGW32__
    m_compiler_path = (minicargo_path / "..\\..\\bin\\mrustc.exe").normalise();
#else
    m_compiler_path = minicargo_path / "mrustc.exe";
#endif
#else
    char buf[1024];
    size_t s = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    buf[s] = 0;

    ::helpers::path minicargo_path { buf };
    minicargo_path.pop_component();
    m_compiler_path = (minicargo_path / "../../bin/mrustc").normalise();
#endif
}

::helpers::path Builder::get_crate_path(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host, const char** crate_type, ::std::string* out_crate_suffix) const
{
    auto outfile = this->get_output_dir(is_for_host);
    // HACK: If there's no version, don't emit a version tag
    ::std::string   crate_suffix;
#if 1
    if( manifest.version() != PackageVersion() ) {
        crate_suffix = ::format("-", manifest.version());
        for(auto& v : crate_suffix)
            if(v == '.')
                v = '_';
    }
#endif

    switch(target.m_type)
    {
    case PackageTarget::Type::Lib:
        if(crate_type) {
            *crate_type = target.m_is_proc_macro ? "proc-macro" : "rlib";
        }
        outfile /= ::format("lib", target.m_name, crate_suffix, ".hir");
        break;
    case PackageTarget::Type::Bin:
        if(crate_type)
            *crate_type = "bin";
        outfile /= ::format(target.m_name, EXESUF);
        break;
    default:
        throw ::std::runtime_error("Unknown target type being built");
    }
    if(out_crate_suffix)
        *out_crate_suffix = crate_suffix;
    return outfile;
}

bool Builder::build_target(const PackageManifest& manifest, const PackageTarget& target, bool is_for_host) const
{
    const char* crate_type;
    ::std::string   crate_suffix;
    auto outfile = this->get_crate_path(manifest, target, is_for_host,  &crate_type, &crate_suffix);

    // TODO: Determine if it needs re-running
    // Rerun if:
    // > `outfile` is missing
    // > mrustc/minicargo is newer than `outfile`
    // > build script has changed
    // > any input file has changed (requires depfile from mrustc)
    bool force_rebuild = false;
    auto ts_result = Timestamp::for_file(outfile);
    if( force_rebuild ) {
        DEBUG("Building " << outfile << " - Force");
    }
    else if( ts_result == Timestamp::infinite_past() ) {
        // Rebuild (missing)
        DEBUG("Building " << outfile << " - Missing");
    }
    else if( !getenv("MINICARGO_IGNTOOLS") && ( ts_result < Timestamp::for_file(m_compiler_path) /*|| ts_result < Timestamp::for_file("bin/minicargo")*/ ) ) {
        // Rebuild (older than mrustc/minicargo)
        DEBUG("Building " << outfile << " - Older than mrustc ( " << ts_result << " < " << Timestamp::for_file(m_compiler_path) << ")");
    }
    else {
        // TODO: Check dependencies. (from depfile)
        // Don't rebuild (no need to)
        DEBUG("Not building " << outfile << " - not out of date");
        return true;
    }

    for(const auto& cmd : manifest.build_script_output().pre_build_commands)
    {
        // TODO: Run commands specified by build script (override)
    }

    ::std::cout << "BUILDING " << target.m_name << " from " << manifest.name() << " v" << manifest.version() << " with features [" << manifest.active_features() << "]" << ::std::endl;
    StringList  args;
    args.push_back(::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(target.m_path));
    args.push_back("--crate-name"); args.push_back(target.m_name.c_str());
    args.push_back("--crate-type"); args.push_back(crate_type);
    if( !crate_suffix.empty() ) {
        args.push_back("--crate-tag"); args.push_back(crate_suffix.c_str() + 1);
    }
    if( true /*this->enable_debug*/ ) {
        args.push_back("-g");
        //args.push_back("--cfg"); args.push_back("debug_assertions");
    }
    if( true /*this->enable_optimise*/ ) {
        args.push_back("-O");
    }
    if( m_opts.target_name )
    {
        if( is_for_host ) {
            //args.push_back("--target"); args.push_back(HOST_TARGET);
        }
        else {
            args.push_back("--target"); args.push_back(m_opts.target_name);
            args.push_back("-C"); args.push_back(format("emit-build-command=",outfile,".sh"));
        }
    }
    if( m_opts.emit_mmir )
    {
        args.push_back("-C"); args.push_back("codegen-type=monomir");
    }

    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(this->get_output_dir(is_for_host).str());
    for(const auto& dir : manifest.build_script_output().rustc_link_search) {
        args.push_back("-L"); args.push_back(dir.second.c_str());
    }
    for(const auto& lib : manifest.build_script_output().rustc_link_lib) {
        args.push_back("-l"); args.push_back(lib.second.c_str());
    }
    for(const auto& cfg : manifest.build_script_output().rustc_cfg) {
        args.push_back("--cfg"); args.push_back(cfg.c_str());
    }
    for(const auto& flag : manifest.build_script_output().rustc_flags) {
        args.push_back(flag.c_str());
    }
    for(const auto& feat : manifest.active_features()) {
        args.push_back("--cfg"); args.push_back(::format("feature=", feat));
    }
    // If not building the package's library, but the package has a library
    if( target.m_type != PackageTarget::Type::Lib && manifest.has_library() )
    {
        // Add a --extern for it
        const auto& m = manifest;
        auto path = this->get_crate_path(m, m.get_library(), is_for_host, nullptr, nullptr);
        args.push_back("--extern");
        args.push_back(::format(m.get_library().m_name, "=", path));
    }
    for(const auto& dep : manifest.dependencies())
    {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            auto path = this->get_crate_path(m, m.get_library(), is_for_host, nullptr, nullptr);
            args.push_back("--extern");
            args.push_back(::format(m.get_library().m_name, "=", path));
        }
    }
    for(const auto& d : m_opts.lib_search_dirs)
    {
        args.push_back("-L");
        args.push_back(d.str().c_str());
    }

    // TODO: Environment variables (rustc_env)
    StringListKV    env;
    auto out_dir = this->get_output_dir(is_for_host).to_absolute() / "build_" + manifest.name().c_str();
    env.push_back("OUT_DIR", out_dir.str());
    env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
    env.push_back("CARGO_PKG_VERSION", ::format(manifest.version()));
    env.push_back("CARGO_PKG_VERSION_MAJOR", ::format(manifest.version().major));
    env.push_back("CARGO_PKG_VERSION_MINOR", ::format(manifest.version().minor));
    env.push_back("CARGO_PKG_VERSION_PATCH", ::format(manifest.version().patch));
    for(const auto& dep : manifest.dependencies())
    {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            for(const auto& p : m.build_script_output().downstream_env)
            {
                env.push_back(p.first.c_str(), p.second.c_str());
            }
        }
    }

    // TODO: If emitting command files (i.e. cross-compiling), concatenate the contents of `outfile + ".sh"` onto a
    // master file.
    // - Will probably want to do this as a final stage after building everything.
    return this->spawn_process_mrustc(args, ::std::move(env), outfile + "_dbg.txt");
}
::helpers::path Builder::build_build_script(const PackageManifest& manifest, bool is_for_host, bool* out_is_rebuilt) const
{
    // - Output dir is the same as the library.
    auto outfile = this->get_output_dir(is_for_host) / manifest.name() + "_build" EXESUF;

    auto ts_result = Timestamp::for_file(outfile);
    if( ts_result == Timestamp::infinite_past() ) {
        DEBUG("Building " << outfile << " - Missing");
    }
    else if( !getenv("MINICARGO_IGNTOOLS") && (ts_result < Timestamp::for_file(m_compiler_path)) ) {
        // Rebuild (older than mrustc/minicargo)
        DEBUG("Building " << outfile << " - Older than mrustc ( " << ts_result << " < " << Timestamp::for_file(m_compiler_path) << ")");
    }
    else
    {
        *out_is_rebuilt = false;
        return outfile;
    }

    // TODO: Load+check a depfile (requires mrustc to emit one)

    StringList  args;
    args.push_back( ::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(manifest.build_script()) );
    args.push_back("--crate-name"); args.push_back("build");
    args.push_back("--crate-type"); args.push_back("bin");
    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(this->get_output_dir(true).str()); // NOTE: Forces `is_for_host` to true here.
    for(const auto& d : m_opts.lib_search_dirs)
    {
        args.push_back("-L");
        args.push_back(d.str().c_str());
    }
    for(const auto& dep : manifest.build_dependencies())
    {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            auto path = this->get_crate_path(m, m.get_library(), true, nullptr, nullptr);   // Dependencies for build scripts are always for the host (because it is)
            args.push_back("--extern");
            args.push_back(::format(m.get_library().m_name, "=", path));
        }
    }
    // - Build scripts are built for the host (not the target)
    //args.push_back("--target"); args.push_back(HOST_TARGET);

    StringListKV    env;
    env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
    env.push_back("CARGO_PKG_VERSION", ::format(manifest.version()));
    env.push_back("CARGO_PKG_VERSION_MAJOR", ::format(manifest.version().major));
    env.push_back("CARGO_PKG_VERSION_MINOR", ::format(manifest.version().minor));
    env.push_back("CARGO_PKG_VERSION_PATCH", ::format(manifest.version().patch));
    // TODO: If there's any dependencies marked as `links = foo` then grab `DEP_FOO_<varname>` from its metadata
    // (build script output)

    if( this->spawn_process_mrustc(args, ::std::move(env), outfile + "_dbg.txt") )
    {
        *out_is_rebuilt = true;
        return outfile;
    }
    else
    {
        // Force the caller to check the above path
        return ::helpers::path();
    }
}
::helpers::path Builder::build_and_run_script(const PackageManifest& manifest, bool is_for_host) const
{
    auto output_dir_abs = this->get_output_dir(is_for_host).to_absolute();

    auto out_file = output_dir_abs / "build_" + manifest.name().c_str() + ".txt";
    auto out_dir = output_dir_abs / "build_" + manifest.name().c_str();
    
    bool run_build_script = false;
    // TODO: Handle a pre-existing script containing `cargo:rerun-if-changed`
    auto script_exe = this->build_build_script(manifest, is_for_host, &run_build_script);
    if( !script_exe.is_valid() )
    {
        // Build failed, return an invalid path too.
        return ::helpers::path();
    }

    if( run_build_script )
    {
        auto script_exe_abs = script_exe.to_absolute();

        // - Run the script and put output in the right dir
#if _WIN32
        CreateDirectoryA(out_dir.str().c_str(), NULL);
#else
        mkdir(out_dir.str().c_str(), 0755);
#endif
        // Environment variables (key-value list)
        StringListKV    env;
        env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
        //env.push_back("CARGO_MANIFEST_LINKS", manifest.m_links);
        //for(const auto& feat : manifest.m_active_features)
        //{
        //    ::std::string   fn = "CARGO_FEATURE_";
        //    for(char c : feat)
        //        fn += c == '-' ? '_' : tolower(c);
        //    env.push_back(fn, manifest.m_links);
        //}
        //env.push_back("CARGO_CFG_RELEASE", "");
        env.push_back("OUT_DIR", out_dir);
        env.push_back("TARGET", m_opts.target_name ? m_opts.target_name : HOST_TARGET);
        env.push_back("HOST", HOST_TARGET);
        env.push_back("NUM_JOBS", "1");
        env.push_back("OPT_LEVEL", "2");
        env.push_back("DEBUG", "0");
        env.push_back("PROFILE", "release");
        for(const auto& dep : manifest.dependencies())
        {
            if( ! dep.is_disabled() )
            {
                const auto& m = dep.get_package();
                for(const auto& p : m.build_script_output().downstream_env)
                {
                    env.push_back(p.first.c_str(), p.second.c_str());
                }
            }
        }

        //auto _ = ScopedChdir { manifest.directory() };
        #if _WIN32
        #else
        auto fd_cwd = open(".", O_DIRECTORY);
        chdir(manifest.directory().str().c_str());
        #endif
        if( !this->spawn_process(script_exe_abs.str().c_str(), {}, env, out_file) )
        {
            rename(out_file.str().c_str(), (out_file+"_failed").str().c_str());
            // Build failed, return an invalid path
            return ::helpers::path();;
        }
        #if _WIN32
        #else
        fchdir(fd_cwd);
        #endif
    }
    
    return out_file;
}
bool Builder::build_library(const PackageManifest& manifest, bool is_for_host) const
{
    if( manifest.build_script() != "" )
    {
        // Locate a build script override file
        if(this->m_opts.build_script_overrides.is_valid())
        {
            auto override_file = this->m_opts.build_script_overrides / "build_" + manifest.name().c_str() + ".txt";
            // TODO: Should this test if it exists? or just assume and let it error?

            // > Note, override file can specify a list of commands to run.
            const_cast<PackageManifest&>(manifest).load_build_script( override_file.str() );
        }
        else
        {
            // - Build+Run
            auto script_file = this->build_and_run_script(manifest, is_for_host);
            if( !script_file.is_valid() )
            {
                return false;
            }
            // - Load
            const_cast<PackageManifest&>(manifest).load_build_script( script_file.str() );
        }
    }

    return this->build_target(manifest, manifest.get_library(), is_for_host);
}
bool Builder::spawn_process_mrustc(const StringList& args, StringListKV env, const ::helpers::path& logfile) const
{
    //env.push_back("MRUSTC_DEBUG", "");
    return spawn_process(m_compiler_path.str().c_str(), args, env, logfile);
}
bool Builder::spawn_process(const char* exe_name, const StringList& args, const StringListKV& env, const ::helpers::path& logfile) const
{
#ifdef _WIN32
    ::std::stringstream cmdline;
    cmdline << exe_name;
    for (const auto& arg : args.get_vec())
        cmdline << " " << arg;
    auto cmdline_str = cmdline.str();
    DEBUG("Calling " << cmdline_str);
    
#if 0
    // TODO: Determine required minimal environment, to avoid importing the entire caller environment
    ::std::stringstream environ_str;
    environ_str << "TEMP=" << getenv("TEMP") << '\0';
    environ_str << "TMP=" << getenv("TMP") << '\0';
    for(auto kv : env)
    {
        environ_str << kv.first << "=" << kv.second << '\0';
    }
    environ_str << '\0';
#else
    for(auto kv : env)
    {
        _putenv_s(kv.first, kv.second);
    }
#endif

    CreateDirectory(static_cast<::std::string>(logfile.parent()).c_str(), NULL);

    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.hStdOutput = CreateFile( static_cast<::std::string>(logfile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        DWORD   tmp;
        WriteFile(si.hStdOutput, cmdline_str.data(), static_cast<DWORD>(cmdline_str.size()), &tmp, NULL);
        WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    PROCESS_INFORMATION pi = { 0 };
    CreateProcessA(exe_name, (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(si.hStdOutput);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(pi.hProcess, &status);
    if (status != 0)
    {
        DEBUG("Compiler exited with non-zero exit status " << status);
        return false;
    }
#else

    // Create logfile output directory
    mkdir(static_cast<::std::string>(logfile.parent()).c_str(), 0755);

    // Create handles such that the log file is on stdout
    ::std::string logfile_str = logfile;
    pid_t pid;
    posix_spawn_file_actions_t  fa;
    {
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logfile_str.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
    }

    // Generate `argv`
    auto argv = args.get_vec();
    argv.insert(argv.begin(), exe_name);
    //DEBUG("Calling " << argv);
    Debug_Print([&](auto& os){
        os << "Calling";
        for(const auto& p : argv)
            os << " " << p;
        });
    DEBUG("Environment " << env);
    argv.push_back(nullptr);

    // Generate `envp`
    StringList  envp;
    extern char **environ;
    for(auto p = environ; *p; p++)
    {
        envp.push_back(*p);
    }
    for(auto kv : env)
    {
        envp.push_back(::format(kv.first, "=", kv.second));
    }
    //Debug_Print([&](auto& os){
    //    os << "ENVP=";
    //    for(const auto& p : envp.get_vec())
    //        os << "\n " << p;
    //    });
    envp.push_back(nullptr);

    if( posix_spawn(&pid, exe_name, &fa, /*attr=*/nullptr, (char* const*)argv.data(), (char* const*)envp.get_vec().data()) != 0 )
    {
        perror("posix_spawn");
        DEBUG("Unable to spawn compiler");
        posix_spawn_file_actions_destroy(&fa);
        return false;
    }
    posix_spawn_file_actions_destroy(&fa);
    int status = -1;
    waitpid(pid, &status, 0);
    if( status != 0 )
    {
        if( WIFEXITED(status) )
            DEBUG("Compiler exited with non-zero exit status " << WEXITSTATUS(status));
        else if( WIFSIGNALED(status) )
            DEBUG("Compiler was terminated with signal " << WTERMSIG(status));
        else
            DEBUG("Compiler terminated for unknown reason, status=" << status);
        DEBUG("See " << logfile << " for the compiler output");
        return false;
    }
#endif
    return true;
}

Timestamp Timestamp::for_file(const ::helpers::path& path)
{
#if _WIN32
    FILETIME    out;
    auto handle = CreateFile(path.str().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(handle == INVALID_HANDLE_VALUE) {
        //DEBUG("Can't find " << path);
        return Timestamp::infinite_past();
    }
    if( GetFileTime(handle, NULL, NULL, &out) == FALSE ) {
        //DEBUG("Can't GetFileTime on " << path);
        CloseHandle(handle);
        return Timestamp::infinite_past();
    }
    CloseHandle(handle);
    //DEBUG(Timestamp{out} << " " << path);
    return Timestamp { out };
#else
    struct stat  s;
    if( stat(path.str().c_str(), &s) == 0 )
    {
        return Timestamp { s.st_mtime };
    }
    else
    {
        return Timestamp::infinite_past();
    }
#endif
}

