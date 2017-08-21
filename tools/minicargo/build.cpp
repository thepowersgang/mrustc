/*
 */
#include "manifest.h"
#include "debug.h"
#include <vector>
#include <algorithm>
#include <sstream>  // stringstream
#include "helpers.h"    // path
#ifdef _WIN32
# include <Windows.h>
#else
# include <spawn.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/wait.h>
# include <fcntl.h>
#endif

struct BuildList
{
    struct BuildEnt {
        const PackageManifest*  package;
        unsigned level;
    };
    ::std::vector<BuildEnt>  m_list;

    void add_dependencies(const PackageManifest& p, unsigned level);
    void add_package(const PackageManifest& p, unsigned level);
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

class Builder
{
    class StringList
    {
        ::std::vector<::std::string>    m_cached;
        ::std::vector<const char*>  m_strings;
    public:
        StringList()
        {
        }

        const ::std::vector<const char*>& get_vec() const
        {
            return m_strings;
        }

        void push_back(::std::string s)
        {
            m_cached.push_back(::std::move(s));
            m_strings.push_back(m_cached.back().c_str());
        }
        void push_back(const char* s)
        {
            m_strings.push_back(s);
        }
    };

    //::helpers::path m_build_script_overrides;

public:
    bool build_target(const PackageManifest& manifest, const PackageTarget& target) const;
    bool build_library(const PackageManifest& manifest) const;

private:
    bool spawn_process(const StringList& args, const ::helpers::path& logfile) const;
};

void MiniCargo_Build(const PackageManifest& manifest)
{
    BuildList   list;

    list.add_dependencies(manifest, 0);

    list.sort_list();
    // dedup?
    for(const auto& p : list.iter())
    {
        DEBUG("WILL BUILD " << p.name() << " from " << p.manifest_path());
    }

    // Build dependencies
    Builder builder;
    for(const auto& p : list.iter())
    {
        if( ! builder.build_library(p) )
        {
            return;
        }
    }

    // TODO: If the manifest doesn't have a library, build the binary
    builder.build_library(manifest);
}

void BuildList::add_dependencies(const PackageManifest& p, unsigned level)
{
    for (const auto& dep : p.dependencies())
    {
        if( dep.is_optional() )
        {
            continue ;
        }
        add_package(dep.get_package(), level+1);
    }
}
void BuildList::add_package(const PackageManifest& p, unsigned level)
{
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
    for (const auto& dep : p.dependencies())
    {
        add_package(dep.get_package(), level+1);
    }
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

bool Builder::build_target(const PackageManifest& manifest, const PackageTarget& target) const
{
    auto outdir = ::helpers::path("output");
    auto outfile = outdir / ::format("lib", target.m_name, ".hir");

    // TODO: Determine if it needs re-running
    // Rerun if:
    // > `outfile` is missing
    // > mrustc/minicargo is newer than `outfile`
    // > build script has changed
    // > any input file has changed (requires depfile from mrustc)
    //auto ts_result = this->get_timestamp(outfile);
    //if( force_rebuild ) {
    //}
    //else if( ts_result == Timestamp::infinite_past() ) {
    //    // Rebuild (missing)
    //}
    //else if( ts_result < this->get_timestamp("../bin/mrustc") || ts_result < this->get_timestamp("bin/minicargo") ) {
    //    // Rebuild (older than mrustc/minicargo)
    //}
    // TODO: Check dependencies.
    //else {
    //    // Don't rebuild (no need to)
    //    return true;
    //}
    

    for(const auto& cmd : manifest.build_script_output().pre_build_commands)
    {
        // TODO: Run commands specified by build script (override)
    }

    StringList  args;
    args.push_back(::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(target.m_path));
    args.push_back("--crate-name"); args.push_back(target.m_name.c_str());
    args.push_back("--crate-type"); args.push_back("rlib");
    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(outdir);
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
    // TODO: Environment variables (rustc_env)

    return this->spawn_process(args, outfile + "_dbg.txt");
}
bool Builder::build_library(const PackageManifest& manifest) const
{
    if( manifest.build_script() != "" )
    {
        // Locate a build script override file
        //if(this->m_build_script_overrides.is_valid())
        //{
        //    auto override_file = this->m_build_script_overrides / "build_" + manifest.name + ".txt";
        //
        //    // > Note, override file can specify a list of commands to run.
        //    manifest.load_build_script( override_file );
        //}
        //else
        //{
        //    // Otherwise, compile and run build script
        //    // - Load dependencies for the build script
        //    // - Build the script itself
        //    this->build_build_script( manifest );
        //    // - Run the script and put output in the right dir
        //    manifest.load_build_script( ::helpers::path("output") / "build_" + manifest.name + ".txt" );
        //}
        throw ::std::runtime_error("TODO: Build script for " + manifest.name());
    }

    return this->build_target(manifest, manifest.get_library());
}
bool Builder::spawn_process(const StringList& args, const ::helpers::path& logfile) const
{
#ifdef _WIN32
    ::std::stringstream cmdline;
    cmdline << "mrustc.exe";
    for (const auto& arg : args.get_vec())
        cmdline << " " << arg;
    auto cmdline_str = cmdline.str();
    DEBUG("Calling " << cmdline_str);

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
        WriteFile(si.hStdOutput, cmdline_str.data(), cmdline_str.size(), &tmp, NULL);
        WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    PROCESS_INFORMATION pi = { 0 };
    char env[] =
        "MRUSTC_DEBUG=""\0"
        ;
    CreateProcessA("x64\\Release\\mrustc.exe", (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
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
    argv.insert(argv.begin(), "mrustc");
    DEBUG("Calling " << argv);
    argv.push_back(nullptr);

    // Generate `envp`
    ::std::vector<const char*> envp;
    extern char **environ;
    for(auto p = environ; *p; p++)
    {
        envp.push_back(*p);
    }
    envp.push_back(nullptr);

    if( posix_spawn(&pid, "../bin/mrustc", &fa, /*attr=*/nullptr, (char* const*)argv.data(), (char* const*)envp.data()) != 0 )
    {
        perror("posix_spawn");
        DEBUG("Unable to spawn compiler");
        posix_spawn_file_actions_destroy(&fa);
        return false;
    }
    posix_spawn_file_actions_destroy(&fa);
    int status = -1;
    waitpid(pid, &status, 0);
    if( WEXITSTATUS(status) != 0 )
    {
        DEBUG("Compiler exited with non-zero exit status " << WEXITSTATUS(status));
        return false;
    }
#endif
    return true;
}
