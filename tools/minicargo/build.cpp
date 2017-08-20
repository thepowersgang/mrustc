/*
 */
#include "manifest.h"
#include "debug.h"
#include <vector>
#include <algorithm>
#include <sstream>  // stringstream
#include "helpers.h"    // path
#ifdef _WIN32
#include <Windows.h>
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
            return *this->l.m_list[this->l.m_list.size() - this->i - 1].package;
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
    for(auto& ent : m_list)
    {
        if(ent.package == &p)
        {
            ent.level = level;
            return ;
        }
    }
    m_list.push_back({ &p, level });
    for (const auto& dep : p.dependencies())
    {
        add_package(dep.get_package(), level+1);
    }
}
void BuildList::sort_list()
{
    ::std::sort(m_list.begin(), m_list.end(), [](const auto& a, const auto& b){ return a.level < b.level; });

    for(auto it = m_list.begin(); it != m_list.end(); )
    {
        auto it2 = ::std::find_if(m_list.begin(), it, [&](const auto& x){ return x.package == it->package; });
        if( it2 != it )
        {
            it = m_list.erase(it);
        }
        else
        {
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

    StringList  args;
    args.push_back(::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(target.m_path));
    args.push_back("--crate-name"); args.push_back(target.m_name.c_str());
    args.push_back("--crate-type"); args.push_back("rlib");
    args.push_back("-o"); args.push_back(outfile);
    args.push_back("-L"); args.push_back(outdir);
    //for(const auto& dir : manifest.build_script.rustc_link_search) {
    //    args.push_back("-L"); args.push_back(dir.second.c_str());
    //}
    //for(const auto& lib : manifest.build_script.rustc_link_lib) {
    //    args.push_back("-l"); args.push_back(lib.second.c_str());
    //}
    //for(const auto& cfg : manifest.build_script.rustc_cfg) {
    //    args.push_back("--cfg"); args.push_back(cfg.c_str());
    //}
    //for(const auto& flag : manifest.build_script.rustc_flags) {
    //    args.push_back(flag.c_str());
    //}
    // TODO: Environment variables (rustc_env)

    return this->spawn_process(args, outfile + "_dbg.txt");
}
bool Builder::build_library(const PackageManifest& manifest) const
{
    if( manifest.build_script() != "" )
    {
        // Locate a build script override file
        // > Note, override file can specify a list of commands to run.
        //manifest.script_output = BuildScript::load( override_file );
        // Otherwise, compile and run build script
        //manifest.script_output = BuildScript::load( ::helpers::path("output") / "build_" + manifest.name + ".txt" );
        // Parse build script output.
        throw ::std::runtime_error("TODO: Build script");
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
#if 1
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
#endif
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
    // TODO: posix_spawn
    return false;
#endif
    return true;
}
