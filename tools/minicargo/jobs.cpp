/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * jobs.cpp
 * - Logic related to running build tasks
 */
#include <iostream>
#include "jobs.hpp"
#include "debug.h"
#include "os.hpp"
#include <iomanip>

#include <unordered_set>
#include <cassert>
#include <algorithm>

#ifdef _WIN32
# include <Windows.h>
#else
extern "C" {
# include <sys/wait.h>  // waitpid
}
#endif

void JobList::add_job(::std::unique_ptr<Job> job)
{
    waiting_jobs.push_back(std::move(job));
}

bool JobList::run_all()
{
    auto total_job_count = this->waiting_jobs.size();
    bool failed = false;
    size_t num_complete = 0;
    while( !this->waiting_jobs.empty() || !this->runnable_jobs.empty() || !this->running_jobs.empty() )
    {
        // Wait until a running job stops
        while( this->running_jobs.size() >= this->num_jobs )
        {
            if( !wait_one() ) {
                failed = true;
                break;
            }
            num_complete += 1;
        }
        if(failed) {
            break;
        }
        
        // Update the runnable list.
        for(auto& slot : this->waiting_jobs)
        {
            if(!slot) {
                continue;
            }
            const auto& deps = slot->dependencies();
            if( std::all_of(deps.begin(), deps.end(), [&](const std::string& s){ return completed_jobs.count(s) > 0; }) && slot->is_runnable() )
            {
                this->runnable_jobs.push_back(std::move(slot));
            }
        }
        auto new_end = std::remove_if(waiting_jobs.begin(), waiting_jobs.end(), [](const job_t& j){ return !j; });
        waiting_jobs.erase(new_end, waiting_jobs.end());
        if( this->runnable_jobs.empty() ) {
            if( this->running_jobs.empty()) {
                if( !this->waiting_jobs.empty() ) {
                    ::std::cerr << "BUG: Nothing runnable or running, but jobs are still waiting\n";
                }
                break ;
            }
            else {
                continue ;
            }
        }
        
        auto job = ::std::move(this->runnable_jobs.front());
        this->runnable_jobs.pop_front();
        auto rjob = job->start();
        {
            ::std::cout << "--- ";
            os_support::set_console_colour(::std::cout, os_support::TerminalColour::Green);
            ::std::cout << job->verb() << " " << job->name();
            os_support::set_console_colour(::std::cout, os_support::TerminalColour::Default);
            ::std::cout << " ("
                << std::fixed << std::setprecision(1) << (100 * static_cast<double>(num_complete) / total_job_count) << "% " 
                << this->running_jobs.size()+1 << "r," << this->runnable_jobs.size() << "w," << this->waiting_jobs.size() << "b/" << total_job_count << "t)";
            ::std::cout << std::endl;
        }

        auto handle = this->spawn(rjob);
        this->running_jobs.push_back(RunningJob { handle, std::move(job), std::move(rjob) });
    }
    while( !this->running_jobs.empty() )
    {
        failed |= !wait_one();
        num_complete += 1;
    }
    return !failed;
}

os_support::Process JobList::spawn(const RunnableJob& rjob)
{
    try {
        return os_support::Process::spawn(rjob.exe_name, rjob.args, rjob.env, rjob.logfile, rjob.working_directory);
    }
    catch(...) {
        exit(1);
    }
}

bool JobList::wait_one()
{
    bool rv;
#ifdef _WIN32
    ::std::vector<HANDLE>   handles;
    for(const auto& j : this->running_jobs) {
        handles.push_back(j.handle.handle);
    }
    auto wait_rv = WaitForMultipleObjects(handles.size(), handles.data(), FALSE, 0);
    if( !(WAIT_OBJECT_0 <= wait_rv && wait_rv < WAIT_OBJECT_0 + handles.size()) ) {
        return false;
    }

    auto idx = wait_rv - WAIT_OBJECT_0;
    auto handle = this->running_jobs[idx].handle.handle;
    RunningJob  rjob = ::std::move(this->running_jobs[idx]);
    this->running_jobs.erase(this->running_jobs.begin() + idx);

    DWORD status = 1;
    GetExitCodeProcess(handle, &status);
    rv = os_support::Process::handle_status(status);
#else
    int status = -1;
    auto pid = waitpid(0, &status, 0);
    if(pid == (pid_t)-1)
    {
        ::std::cerr << "`waitpid` failed!" << std::endl;
        return false;
    }

    rv = os_support::Process::handle_status(status);

    auto i = ::std::find_if(this->running_jobs.begin(), this->running_jobs.end(), [&](const auto& e){ return e.handle.handle == pid; });
    assert(i != this->running_jobs.end());
    RunningJob rjob = ::std::move(*i);
    this->running_jobs.erase(i);
#endif

    if( !rv )
    {
        ::std::cerr << "FAILING COMMAND: " << rjob.desc.exe_name;
        for(const auto& p : rjob.desc.args.get_vec())
            ::std::cerr  << " " << p;
        ::std::cerr << ::std::endl;
        ::std::cerr << "Env: ";
        for(auto kv : rjob.desc.env)
        {
            ::std::cerr << " " << kv.first << "=" << kv.second;
        }
        ::std::cerr << ::std::endl;
        //::std::cerr << "See " << rjob.desc->logfile() << " for the compiler output" << ::std::endl;
    }
    else
    {
        this->completed_jobs.insert(rjob.job->name());
    }
    rv &= rjob.job->complete(rv);
    if(getenv("MINICARGO_RUN_ONCE") || getenv("MINICARGO_RUNONCE"))
    {
        if(rv) {
            std::cerr << "- Only running one job" << std::endl;
        }
        exit(1);
    }
    
    return rv;
}