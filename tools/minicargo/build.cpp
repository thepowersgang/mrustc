/*
 */
#ifdef _WIN32
# define _CRT_SECURE_NO_WARNINGS    // Allows use of getenv (this program doesn't set env vars)
#endif
#include "manifest.h"
#include "build.h"
#include "debug.h"
#include <vector>
#include <algorithm>
#include <sstream>  // stringstream
#include <cstdlib>  // setenv
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
# define TARGET "x86_64-windows-msvc"
#else
# define EXESUF ""
# define TARGET "x86_64-unknown-linux-gnu"
#endif

struct BuildList
{
    struct BuildEnt {
        const PackageManifest*  package;
        unsigned level;
    };
    ::std::vector<BuildEnt>  m_list;

    void add_dependencies(const PackageManifest& p, unsigned level, bool include_build);
    void add_package(const PackageManifest& p, unsigned level, bool include_build);
    void sort_list();

    struct Iter {
        const BuildList& l;
        size_t  i;

        const PackageManifest& operator*() const {
            return *this->l.m_list[this->i].package;
        }
        void operator++() {
            this->i++;
        }
        bool operator!=(const Iter& x) const {
            return this->i != x.i;
        }
        Iter begin() const {
            return *this;
        }
        Iter end() {
            return Iter{ this->l, this->l.m_list.size() };
        }
    };

    Iter iter() const {
        return Iter { *this, 0 };
    }
};

class StringList
{
    ::std::vector<::std::string>    m_cached;
    ::std::vector<const char*>  m_strings;
public:
    StringList()
    {
    }
    StringList(const StringList&) = delete;
    StringList(StringList&&) = default;

    const ::std::vector<const char*>& get_vec() const
    {
        return m_strings;
    }

    void push_back(::std::string s)
    {
        // If the cache list is about to move, update the pointers
        if(m_cached.capacity() == m_cached.size())
        {
            // Make a bitmap of entries in `m_strings` that are pointers into `m_cached`
            ::std::vector<bool> b;
            b.reserve(m_strings.size());
            size_t j = 0;
            for(const auto* s : m_strings)
            {
                if(j == m_cached.size())
                    break;
                if(s == m_cached[j].c_str())
                {
                    j ++;
                    b.push_back(true);
                }
                else
                {
                    b.push_back(false);
                }
            }

            // Add the new one
            m_cached.push_back(::std::move(s));
            // Update pointers
            j = 0;
            for(size_t i = 0; i < b.size(); i ++)
            {
                if(b[i])
                {
                    m_strings[i] = m_cached.at(j++).c_str();
                }
            }
        }
        else
        {
            m_cached.push_back(::std::move(s));
        }
        m_strings.push_back(m_cached.back().c_str());
    }
    void push_back(const char* s)
    {
        m_strings.push_back(s);
    }
};
class StringListKV: private StringList
{
    ::std::vector<const char*>  m_keys;
public:
    StringListKV()
    {
    }
    StringListKV(StringListKV&& x):
        StringList(::std::move(x)),
        m_keys(::std::move(x.m_keys))
    {
    }

    void push_back(const char* k, ::std::string v)
    {
        m_keys.push_back(k);
        StringList::push_back(v);
    }
    void push_back(const char* k, const char* v)
    {
        m_keys.push_back(k);
        StringList::push_back(v);
    }

    struct Iter {
        const StringListKV&   v;
        size_t  i;
        
        void operator++() {
            this->i++;
        }
        ::std::pair<const char*,const char*> operator*() {
            return ::std::make_pair(this->v.m_keys[this->i], this->v.get_vec()[this->i]);
        }
        bool operator!=(const Iter& x) const {
            return this->i != x.i;
        }
    };
    Iter begin() const {
        return Iter { *this, 0 };
    }
    Iter end() const {
        return Iter { *this, m_keys.size() };
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const StringListKV& x) {
        os << "{ ";
        for(auto kv : x)
            os << kv.first << "=" << kv.second << " ";
        os << "}";
        return os;
    }
};

struct Timestamp
{
#if _WIN32
    uint64_t m_val;

    Timestamp(FILETIME ft):
        m_val( (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime) )
    {
    }
#else
    time_t  m_val;
#endif
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

bool MiniCargo_Build(const PackageManifest& manifest, BuildOptions opts)
{
    BuildList   list;

    list.add_dependencies(manifest, 0, !opts.build_script_overrides.is_valid());

    list.sort_list();
    // dedup?
    for(const auto& p : list.iter())
    {
        DEBUG("WILL BUILD " << p.name() << " from " << p.manifest_path());
    }

    // Build dependencies
    Builder builder { ::std::move(opts) };
    for(const auto& p : list.iter())
    {
        if( ! builder.build_library(p) )
        {
            return false;
        }
    }

    // TODO: If the manifest doesn't have a library, build the binary
    if( manifest.has_library() )
    {
        if( ! builder.build_library(manifest) )
        {
            return false;
        }
    }

    return manifest.foreach_binaries([&](const auto& bin_target) {
        return builder.build_target(manifest, bin_target);
        });
}

void BuildList::add_dependencies(const PackageManifest& p, unsigned level, bool include_build)
{
    for (const auto& dep : p.dependencies())
    {
        if( dep.is_disabled() )
        {
            continue ;
        }
        DEBUG(p.name() << ": Dependency " << dep.name());
        add_package(dep.get_package(), level+1, include_build);
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
            add_package(dep.get_package(), level+1, include_build);
        }
    }
}
void BuildList::add_package(const PackageManifest& p, unsigned level, bool include_build)
{
    TRACE_FUNCTION_F(p.name());
    // If the package is already loaded
    for(auto& ent : m_list)
    {
        if(ent.package == &p && ent.level >= level)
        {
            // NOTE: Only skip if this package will be built before we needed (i.e. the level is greater)
            return ;
        }
        // Keep searching (might already have a higher entry)
    }
    m_list.push_back({ &p, level });
    add_dependencies(p, level, include_build);
}
void BuildList::sort_list()
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

Builder::Builder(BuildOptions opts):
    m_opts(::std::move(opts))
{
#ifdef _WIN32
    char buf[1024];
    size_t s = GetModuleFileName(NULL, buf, sizeof(buf)-1);
    buf[s] = 0;

    ::helpers::path minicargo_path { buf };
    minicargo_path.pop_component();
    m_compiler_path = minicargo_path / "mrustc.exe";
#else
    char buf[1024];
    size_t s = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    buf[s] = 0;

    ::helpers::path minicargo_path { buf };
    minicargo_path.pop_component();
    m_compiler_path = (minicargo_path / "../../bin/mrustc").normalise();
#endif
}

::helpers::path Builder::get_crate_path(const PackageManifest& manifest, const PackageTarget& target, const char** crate_type, ::std::string* out_crate_suffix) const
{
    auto outfile = m_opts.output_dir;
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

bool Builder::build_target(const PackageManifest& manifest, const PackageTarget& target) const
{
    const char* crate_type;
    ::std::string   crate_suffix;
    auto outfile = this->get_crate_path(manifest, target,  &crate_type, &crate_suffix);

    // TODO: Determine if it needs re-running
    // Rerun if:
    // > `outfile` is missing
    // > mrustc/minicargo is newer than `outfile`
    // > build script has changed
    // > any input file has changed (requires depfile from mrustc)
    bool force_rebuild = false;
    auto ts_result = this->get_timestamp(outfile);
    if( force_rebuild ) {
        DEBUG("Building " << outfile << " - Force");
    }
    else if( ts_result == Timestamp::infinite_past() ) {
        // Rebuild (missing)
        DEBUG("Building " << outfile << " - Missing");
    }
    else if( ts_result < this->get_timestamp(m_compiler_path) /*|| ts_result < this->get_timestamp("bin/minicargo")*/ ) {
        // Rebuild (older than mrustc/minicargo)
        DEBUG("Building " << outfile << " - Older than mrustc ( " << ts_result << " < " << this->get_timestamp(m_compiler_path) << ")");
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
    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(m_opts.output_dir.str().c_str());
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
    for(const auto& dep : manifest.dependencies())
    {
        if( ! dep.is_disabled() )
        {
            const auto& m = dep.get_package();
            auto path = this->get_crate_path(m, m.get_library(), nullptr, nullptr);
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
    auto out_dir = m_opts.output_dir.to_absolute() / "build_" + manifest.name().c_str();
    env.push_back("OUT_DIR", out_dir.str());
    env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
    env.push_back("CARGO_PKG_VERSION", ::format(manifest.version()));

    return this->spawn_process_mrustc(args, ::std::move(env), outfile + "_dbg.txt");
}
::std::string Builder::build_build_script(const PackageManifest& manifest) const
{
    auto outfile = m_opts.output_dir / manifest.name() + "_build" EXESUF;

    StringList  args;
    args.push_back( ::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(manifest.build_script()) );
    args.push_back("--crate-name"); args.push_back("build");
    args.push_back("--crate-type"); args.push_back("bin");
    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(m_opts.output_dir.str().c_str());
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
            auto path = this->get_crate_path(m, m.get_library(), nullptr, nullptr);
            args.push_back("--extern");
            args.push_back(::format(m.get_library().m_name, "=", path));
        }
    }

    StringListKV    env;
    env.push_back("CARGO_MANIFEST_DIR", manifest.directory().to_absolute());
    env.push_back("CARGO_PKG_VERSION", ::format(manifest.version()));

    if( this->spawn_process_mrustc(args, ::std::move(env), outfile + "_dbg.txt") )
        return outfile;
    else
        return "";
}
bool Builder::build_library(const PackageManifest& manifest) const
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
            auto out_file = m_opts.output_dir / "build_" + manifest.name().c_str() + ".txt";
            // If the build script output doesn't exist (TODO: Or is older than ...)
            bool run_build_script = true;
            auto ts_result = this->get_timestamp(out_file);
            if( ts_result == Timestamp::infinite_past() ) {
                DEBUG("Building " << out_file << " - Missing");
            }
            else if( ts_result < this->get_timestamp(m_compiler_path) /*|| ts_result < this->get_timestamp("bin/minicargo")*/ ) {
                // Rebuild (older than mrustc/minicargo)
                DEBUG("Building " << out_file << " - Older than mrustc ( " << ts_result << " < " << this->get_timestamp(m_compiler_path) << ")");
            }
            else
            {
                run_build_script = false;
            }
            if( run_build_script )
            {
                // Compile and run build script
                // - Load dependencies for the build script
                //  - TODO: Should this have already been done
                // - Build the script itself
                auto script_exe = this->build_build_script( manifest );
                if( script_exe == "" )
                    return false;
                auto script_exe_abs = ::helpers::path(script_exe).to_absolute();

                auto output_dir_abs = m_opts.output_dir.to_absolute();

                // - Run the script and put output in the right dir
                auto out_file = output_dir_abs / "build_" + manifest.name().c_str() + ".txt";
                auto out_dir = output_dir_abs / "build_" + manifest.name().c_str();
#if _WIN32
                CreateDirectoryA(out_dir.str().c_str(), NULL);
#else
                mkdir(out_dir.str().c_str(), 0755);
#endif
                // TODO: Environment variables (key-value list)
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
                env.push_back("TARGET", TARGET);
                env.push_back("HOST", TARGET);
                env.push_back("NUM_JOBS", "1");
                env.push_back("OPT_LEVEL", "2");
                env.push_back("DEBUG", "0");
                env.push_back("PROFILE", "release");

                #if _WIN32
                #else
                auto fd_cwd = open(".", O_DIRECTORY);
                chdir(manifest.directory().str().c_str());
                #endif
                if( !this->spawn_process(script_exe_abs.str().c_str(), {}, env, out_file) )
                {
                    rename(out_file.str().c_str(), (out_file+"_failed").str().c_str());
                    return false;
                }
                #if _WIN32
                #else
                fchdir(fd_cwd);
                #endif
            }
            // - Load
            const_cast<PackageManifest&>(manifest).load_build_script( out_file.str() );
        }
    }

    return this->build_target(manifest, manifest.get_library());
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
    // TODO: Determine required minimal environment.
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
        return false;
    }
#endif
    return true;
}

Timestamp Builder::get_timestamp(const ::helpers::path& path) const
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

