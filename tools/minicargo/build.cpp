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
    // Generate sorted dependency list
    for (const auto& dep : manifest.dependencies())
    {
        list.add_package(dep.get_package(), 1);
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
}

bool Builder::build_target(const PackageManifest& manifest, const PackageTarget& target) const
{
    auto outfile = ::helpers::path("output") / ::format("lib", target.m_name, ".hir");

    StringList  args;
    args.push_back(::helpers::path(manifest.manifest_path()).parent() / ::helpers::path(target.m_path));
    args.push_back("--crate-name"); args.push_back(target.m_name.c_str());
    args.push_back("--crate-type"); args.push_back("rlib");
    args.push_back("-o"); args.push_back(outfile);

    return this->spawn_process(args, outfile + "_dbg.txt");
}
bool Builder::build_library(const PackageManifest& manifest) const
{
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
    CreateProcessA("x64\\Release\\mrustc.exe", (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, "MRUSTC_DEBUG=\0", NULL, &si, &pi);
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
