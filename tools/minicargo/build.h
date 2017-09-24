#pragma once

#include "manifest.h"
#include "path.h"

class StringList;
class StringListKV;
struct Timestamp;

struct BuildOptions
{
    ::helpers::path output_dir;
    ::helpers::path build_script_overrides;
    ::std::vector<::helpers::path>  lib_search_dirs;
};

class Builder
{
    BuildOptions    m_opts;
    ::helpers::path m_compiler_path;

public:
    Builder(BuildOptions opts);

    bool build_target(const PackageManifest& manifest, const PackageTarget& target) const;
    bool build_library(const PackageManifest& manifest) const;
    ::std::string build_build_script(const PackageManifest& manifest) const;

private:
    ::helpers::path get_crate_path(const PackageManifest& manifest, const PackageTarget& target, const char** crate_type, ::std::string* out_crate_suffix) const;
    bool spawn_process_mrustc(const StringList& args, StringListKV env, const ::helpers::path& logfile) const;
    bool spawn_process(const char* exe_name, const StringList& args, const StringListKV& env, const ::helpers::path& logfile) const;


    Timestamp get_timestamp(const ::helpers::path& path) const;
};

extern bool MiniCargo_Build(const PackageManifest& manifest, BuildOptions opts);
