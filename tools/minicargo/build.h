#pragma once

#include "manifest.h"
#include "path.h"

class StringList;
class Timestamp;

class Builder
{
    static const char* const MRUSTC_PATH;

    ::helpers::path m_output_dir;
    ::helpers::path m_build_script_overrides;

public:
    Builder(::helpers::path output_dir, ::helpers::path override_dir):
        m_output_dir(output_dir),
        m_build_script_overrides(override_dir)
    {
    }

    bool build_target(const PackageManifest& manifest, const PackageTarget& target) const;
    bool build_library(const PackageManifest& manifest) const;
    ::std::string build_build_script(const PackageManifest& manifest) const;

private:
    bool spawn_process(const char* exe_name, const StringList& args, const ::helpers::path& logfile) const;
    bool spawn_process_mrustc(const StringList& args, const ::helpers::path& logfile) const;


    Timestamp get_timestamp(const ::helpers::path& path) const;
};

extern void MiniCargo_Build(const PackageManifest& manifest, ::helpers::path override_path);
