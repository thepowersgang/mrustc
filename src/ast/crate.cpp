/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/crate.cpp
 * - Helper functions for the AST::Crate type (includes loading `extern crate`s)
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"
#include "../expand/cfg.hpp"
#include <hir/hir.hpp>  // HIR::Crate
#include <hir/main_bindings.hpp>    // HIR_Deserialise
#include <fstream>
#ifdef _WIN32
# define NOGDI  // prevent ERROR from being defined
# include <Windows.h>
#else
# include <dirent.h>
#endif

::std::vector<::std::string>    AST::g_crate_load_dirs = { };
::std::map<::std::string, ::std::string>    AST::g_crate_overrides;
::std::map<RcString, RcString>    AST::g_implicit_crates;

namespace {
    bool check_item_cfg(const ::AST::AttributeList& attrs)
    {
        for(const auto& at : attrs.m_items) {
            if( at.name() == "cfg" && !check_cfg(at.span(), at) ) {
                return false;
            }
        }
        return true;
    }
    void iterate_module(::AST::Module& mod, ::std::function<void(::AST::Module& mod)> fcn)
    {
        fcn(mod);
        for( auto& sm : mod.m_items )
        {
            if( auto* e = sm->data.opt_Module() )
            {
                if( check_item_cfg(sm->attrs) )
                {
                    iterate_module(*e, fcn);
                }
            }
        }
        // TODO: What about if an anon mod has been #[cfg]-d out?
        // - For now, disable
        //for(const auto& anon : mod.anon_mods() ) {
        //    iterate_module(*anon, fcn);
        //}
    }
}


namespace AST {

Crate::Crate():
    m_root_module(AST::AbsolutePath()),
    m_load_std(LOAD_STD)
{
}

void Crate::load_externs()
{
    auto cb = [this](Module& mod) {
        for( /*const*/ auto& it : mod.m_items )
        {
            if( auto* c = it->data.opt_Crate() )
            {
                if( check_item_cfg(it->attrs) )
                {
                    if( c->name == "" ) {
                        // Leave for now
                    }
                    else {
                        c->name = load_extern_crate( it->span, c->name.c_str() );
                    }
                }
            }
        }
        };
    iterate_module(m_root_module, cb);

    // Check for no_std or no_core, and load libstd/libcore
    // - Duplicates some of the logic in "Expand", but also helps keep crate loading separate to most of expand
    // NOTE: Not all crates are loaded here, any crates loaded by macro invocations will be done during expand.
    bool no_std  = false;
    bool no_core = false;

    for( const auto& a : this->m_attrs.m_items )
    {
        if( a.name() == "no_std" )
            no_std = true;
        if( a.name() == "no_core" )
            no_core = true;
        if( a.name() == "cfg_attr" && a.items().size() == 2 ) {
            if( check_cfg(a.span(), a.items().at(0)) )
            {
                const auto& a2 = a.items().at(1);
                if( a2.name() == "no_std" )
                    no_std = true;
                if( a2.name() == "no_core" )
                    no_core = true;
            }
        }
    }

    if( no_core ) {
        // Don't load anything
    }
    else if( no_std ) {
        auto n = this->load_extern_crate(Span(), "core");
        //if( n != "core" ) {
        //    WARNING(Span(), W0000, "libcore wasn't loaded as `core`, instead `" << n << "`");
        //}
    }
    else {
        auto n = this->load_extern_crate(Span(), "std");
        //if( n != "std" ) {
        //    WARNING(Span(), W0000, "libstd wasn't loaded as `std`, instead `" << n << "`");
        //}
    }

    // Ensure that all crates passed on the command line are loaded
    //if( this->m_edition >= Edition::Rust2018 )
    if( TARGETVER_LEAST_1_29 )
    {
        DEBUG("Load from --crate");
        for(const auto& c : g_crate_overrides)
        {
            auto real_name = this->load_extern_crate(Span(), c.first.c_str());
            g_implicit_crates.insert( std::make_pair(RcString::new_interned(c.first), real_name) );
        }
        // 
        if(this->m_ext_cratename_core != "")
        {
            g_implicit_crates.insert( std::make_pair( RcString::new_interned("core"), this->m_ext_cratename_core) );
        }
    }
}
// TODO: Handle disambiguating crates with the same name (e.g. libc in std and crates.io libc)
// - Crates recorded in rlibs should specify a hash/tag that's passed in to this function.
RcString Crate::load_extern_crate(Span sp, const RcString& name, const ::std::string& basename/*=""*/)
{
    TRACE_FUNCTION_F("Loading crate '" << name << "' (basename='" << basename << "')");

    ::std::string   path;
    auto it = g_crate_overrides.find(name.c_str());
    // If there's no filename, and this crate name is in the override list - use an the explicit path
    if(basename == "" && it != g_crate_overrides.end())
    {
        path = it->second;
        if( !::std::ifstream(path).good() ) {
            ERROR(sp, E0000, "Unable to open crate '" << name << "' at path " << path);
        }
        DEBUG("path = " << path << " (--extern)");
    }
    // If the filename is known, then search for that in the search directories
    // - Checks the crate name of each to ensure a match
    else if( basename != "" )
    {
        // Search a list of load paths for the crate
        for(const auto& p : g_crate_load_dirs)
        {
            path = p + "/" + basename;

            if( ::std::ifstream(path).good() ) {
                // Ensure that if this is loaded, it yields the right name (otherwise skip)
                auto n = HIR_Deserialise_JustName(path);
                if( n == name ) {
                    break ;
                }
            }
        }
        if( !::std::ifstream(path).good() ) {
            ERROR(sp, E0000, "Unable to locate crate '" << name << "' with filename " << basename << " in search directories");
        }
        DEBUG("path = " << path << " (basename)");
    }
    else
    {
        ::std::vector<::std::string>    paths;
#define RLIB_SUFFIX ".rlib"
#define RDYLIB_SUFFIX ".so"
        auto direct_filename = FMT("lib" << name.c_str() << RLIB_SUFFIX);
        auto direct_filename_so = FMT("lib" << name.c_str() << RDYLIB_SUFFIX);
        auto name_prefix = FMT("lib" << name.c_str() << "-");
        // Search a list of load paths for the crate
        for(const auto& p : g_crate_load_dirs)
        {
            path = p + "/" + direct_filename;
            if( ::std::ifstream(path).good() ) {
                paths.push_back(path);
            }
            path = p + "/" + direct_filename_so;
            if( ::std::ifstream(path).good() ) {
                paths.push_back(path);
            }
            path = "";

            // Search for `p+"/lib"+name+"-*.rlib" (which would match e.g. libnum-0.11.rlib)
#ifdef _WIN32
            WIN32_FIND_DATA find_data;
            auto mask = p + "\\*";
            HANDLE find_handle = FindFirstFile( mask.c_str(), &find_data );
            if( find_handle == INVALID_HANDLE_VALUE ) {
                continue ;
            }
            do
            {
                const auto* fname = find_data.cFileName;
#else
            auto dp = opendir(p.c_str());
            if( !dp ) {
                continue ;
            }
            struct dirent *ent;
            while( (ent = readdir(dp)) != nullptr && path == "" )
            {
                const auto* fname = ent->d_name;
#endif

                // AND the start is "lib"+name
                size_t len = strlen(fname);
                if( len > (sizeof(RLIB_SUFFIX)-1) && strcmp(fname + len - (sizeof(RLIB_SUFFIX)-1), RLIB_SUFFIX) == 0 )
                {
                }
                else if( len > (sizeof(RDYLIB_SUFFIX)-1) && strcmp(fname + len - (sizeof(RDYLIB_SUFFIX)-1), RDYLIB_SUFFIX) == 0 )
                {
                }
                else
                {
                    continue ;
                }

                DEBUG(fname << " vs " << name_prefix);
                // Check if the entry ends with .rlib
                if( strncmp(name_prefix.c_str(), fname, name_prefix.size()) != 0 )
                    continue ;

                paths.push_back( p + "/" + fname );
#ifdef _WIN32
            } while( FindNextFile(find_handle, &find_data) );
            FindClose(find_handle);
#else
            }
            closedir(dp);
#endif
            if( paths.size() > 0 )
                break;
        }
        if( paths.size() > 1 ) {
            ERROR(sp, E0000, "Multiple options for crate '" << name << "' in search directories - " << paths);
        }
        if( paths.size() == 0 || !::std::ifstream(paths.front()).good() ) {
            ERROR(sp, E0000, "Unable to locate crate '" << name << "' in search directories");
        }
        path = paths.front();
        DEBUG("path = " << path << " (search)");
    }

    // NOTE: Creating `ExternCrate` loads the crate from the specified path
    auto ec = ExternCrate { name, path };
    auto real_name = ec.m_hir->m_crate_name;
    assert(real_name != "");
    auto res = m_extern_crates.insert(::std::make_pair( real_name, mv$(ec) ));
    if( !res.second ) {
        // Crate already loaded?
    }
    auto& ext_crate = res.first->second;
    // Move the external list out (doesn't need to be kept in the nested crate)
    //auto crate_ext_list = mv$( ext_crate.m_hir->m_ext_crates );
    const auto& crate_ext_list = ext_crate.m_hir->m_ext_crates;

    // Load referenced crates
    for( const auto& ext : crate_ext_list )
    {
        if( m_extern_crates.count(ext.first) == 0 )
        {
            const auto load_name = this->load_extern_crate(sp, ext.first, ext.second.m_basename);
            if( load_name != ext.first )
            {
                // ERROR - The crate loaded wasn't the one that was used when compiling this crate.
                ERROR(sp, E0000, "The crate file `" << ext.second.m_basename << "` didn't load the expected crate - have " << load_name << " != exp " << ext.first);
            }
        }
    }

    if( ext_crate.m_short_name == "core" ) {
        if( this->m_ext_cratename_core == "" ) {
            this->m_ext_cratename_core = ext_crate.m_name;
        }
    }
    if( ext_crate.m_short_name == "std" ) {
        if( this->m_ext_cratename_std == "" ) {
            this->m_ext_cratename_std = ext_crate.m_name;
        }
    }
    if( ext_crate.m_short_name == "proc_macro" ) {
        if( this->m_ext_cratename_procmacro == "" ) {
            this->m_ext_cratename_procmacro = ext_crate.m_name;
        }
    }
    if( ext_crate.m_short_name == "test" ) {
        if( this->m_ext_cratename_test == "" ) {
            this->m_ext_cratename_test = ext_crate.m_name;
        }
    }

    DEBUG("Loaded '" << name << "' from '" << basename << "' (actual name is '" << real_name << "' aka `" << ext_crate.m_short_name << "`)");
    return real_name;
}

ExternCrate::ExternCrate(const RcString& name, const ::std::string& path):
    m_name(name),
    m_short_name(name),
    m_filename(path)
{
    TRACE_FUNCTION_F("name=" << name << ", path='" << path << "'");
    m_hir = HIR_Deserialise(path);

    m_hir->post_load_update(name);
    m_name = m_hir->m_crate_name;
    if(const auto* e = strchr(m_name.c_str(), '-'))
    {
        m_short_name = RcString(m_name.c_str(), e - m_name.c_str());
    }
    else
    {
    }
}

}   // namespace AST

