/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * build.h
 * - Definitions relating to building the crate (and dependencies)
 */
#pragma once

#include "manifest.h"
#include <path.h>

class StringList;
class StringListKV;
class Timestamp;

struct BuildOptions
{
    ::helpers::path output_dir;
    ::helpers::path build_script_overrides;
    ::std::vector<::helpers::path>  lib_search_dirs;
    bool emit_mmir = false;
    bool enable_debug = false;
    const char* target_name = nullptr;  // if null, host is used
    enum class Mode {
        /// Build the binary/library
        Normal,
        /// Build tests
        Test,
        ///// Build examples
        //Examples,
    } mode = Mode::Normal;
};

class BuildList
{
    struct Entry
    {
        const PackageManifest*  package;
        bool    is_host;
        ::std::vector<unsigned> dependents;   // Indexes into the list
    };
    const PackageManifest&  m_root_manifest;
    // List is sorted by build order
    ::std::vector<Entry>    m_list;
public:
    BuildList(const PackageManifest& manifest, const BuildOptions& opts);
    bool build(BuildOptions opts, unsigned num_jobs, bool dry_run);  // 0 = either 1 job, or as many as we can take from the jobserver
};
