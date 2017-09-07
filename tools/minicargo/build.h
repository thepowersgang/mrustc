#pragma once

#include "manifest.h"
#include "path.h"

class StringList;
class StringListKV;
struct Timestamp;

class Builder
{
    ::helpers::path m_output_dir;
    ::helpers::path m_build_script_overrides;
    ::helpers::path m_compiler_path;

public:
    Builder(::helpers::path output_dir, ::helpers::path override_dir);

    bool build_target(const PackageManifest& manifest, const PackageTarget& target) const;
    bool build_library(const PackageManifest& manifest) const;
    ::std::string build_build_script(const PackageManifest& manifest) const;

private:
    bool spawn_process_mrustc(const StringList& args, StringListKV env, const ::helpers::path& logfile) const;
    bool spawn_process(const char* exe_name, const StringList& args, const StringListKV& env, const ::helpers::path& logfile) const;


    Timestamp get_timestamp(const ::helpers::path& path) const;
};

extern bool MiniCargo_Build(const PackageManifest& manifest, ::helpers::path override_path);
