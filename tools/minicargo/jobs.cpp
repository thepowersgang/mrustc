/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * jobs.cpp
 * - Logic related to running build tasks
 */
 //
#undef NDEBUG
#ifdef _MSC_VER
# define _CRT_SECURE_NO_WARNINGS    // Allows use of getenv (this program doesn't set env vars)
#endif

#include <iostream>
#include "jobs.hpp"
#include "debug.h"
#include "jobserver.h"
#include "os.hpp"
#include <iomanip>

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

bool JobList::run_all(size_t num_jobs, bool dry_run)
{
    // Sort jobs by name, to provide a consistent execution order
    ::std::sort(waiting_jobs.begin(), waiting_jobs.end(), [](const std::unique_ptr<Job>& a, const std::unique_ptr<Job>& b) {
        return a->name() < b->name();
    });

    auto jobserver = (dry_run || num_jobs == 1) ? ::std::unique_ptr<JobServer>()    // Dry run or being forced to a single build task? force no jobserver
            : num_jobs == 0 ? JobServer::create(0)  // If no `-j` option is passed, try and get a jobserver client but don't create a server
            : JobServer::create(num_jobs-1) // `-j` was passed with a non-1 count, allow `JobServer` to create a server (keeping one job for ourselves)
            ;
    if( !dry_run && num_jobs > 1 ) {
        assert(jobserver);
    }
    // If there is no jobserver client, and `-j` wasn't passed - force the job count to 1
    // - This simplifies the logic lower down.
    if(!jobserver && num_jobs == 0) {
        num_jobs = 1;
    }
    const auto total_job_count = this->waiting_jobs.size();

    bool failed = false;
    /// Number of jobserver tokens taken
    size_t  n_tokens = 0;
    // Force the main loop to wait until a task completes. Set when there is nothing waiting to run.
    bool force_wait = false;

    auto dump_state = [&]() {
        auto num_complete = total_job_count - this->runnable_jobs.size() - this->running_jobs.size() - this->waiting_jobs.size();
        ::std::cerr
            << " ("
            << std::fixed << std::setprecision(1) << (100 * static_cast<double>(num_complete) / total_job_count) << "% " 
            << this->running_jobs.size() << "r," << this->runnable_jobs.size() << "w," << this->waiting_jobs.size() << "b"
            << "," << num_complete << "c"
            << "/" << total_job_count << "t"
            << "):"
            ;
        for(const auto& rj : this->running_jobs) {
            if(&rj != &this->running_jobs.front())
                ::std::cerr << ",";
            ::std::cerr << " " << rj.job->name();
        }
        ::std::cerr << "\n";
    };

    #if 0
    // HACK: Consume jobs that don't need to be run
    while( !this->waiting_jobs.empty() || !this->runnable_jobs.empty() || !this->running_jobs.empty() )
    {
        // Update the runnable list.
        for(auto& slot : this->waiting_jobs)
        {
            assert(slot);
            const auto& deps = slot->dependencies();
            if( std::all_of(deps.begin(), deps.end(), [&](const std::string& s){ return completed_jobs.count(s) > 0; }) && slot->is_runnable() )
            {
                this->runnable_jobs.push_back(std::move(slot));
            }
        }
        auto new_end = std::remove_if(waiting_jobs.begin(), waiting_jobs.end(), [](const job_t& j){ return !j; });
        waiting_jobs.erase(new_end, waiting_jobs.end());

        ::std::sort(runnable_jobs.begin(), runnable_jobs.end(), [](const std::unique_ptr<Job>& a, const std::unique_ptr<Job>& b) {
            return a->name() < b->name();
        });

        if( this->runnable_jobs.empty() ) {
            break;
        }
        if( !this->runnable_jobs.front()->is_already_complete() ) {
            break;
        }
        this->runnable_jobs.pop_front();
    }
    #else
    // Remove previously completed jobs after sorting
    {
        auto new_end = std::remove_if(this->waiting_jobs.begin(), this->waiting_jobs.end(),
            [](const std::unique_ptr<Job>& a) { return a->is_already_complete(); });
        this->waiting_jobs.erase(new_end, this->waiting_jobs.end());
    }
    #endif

    while( !this->waiting_jobs.empty() || !this->runnable_jobs.empty() || !this->running_jobs.empty() )
    {
        // Wait until a running job stops
        if( !dry_run ) {
            // Keep looping while:
            // - There is at least one running job
            // - AND any of:
            //   - A wait is being forced (due to an empty runnable queue)
            //   - We're being limited by our own internal limits (either via `-j` or because there's no jobserver)
            while( this->running_jobs.size() > 0 && (force_wait || (num_jobs > 0 && this->running_jobs.size() >= num_jobs)) )
            {
                if( !wait_one() ) {
                    dump_state();
                    failed = true;
                    break;
                }
                dump_state();
                force_wait = false;
            }
            if(failed) {
                break;
            }
            // Release jobserver tokens
            if( jobserver ) {
                // Has to handle the case where there are now no jobs running, and NOT release the implicit token we already own
                while( n_tokens > 0 && 1+n_tokens > this->running_jobs.size()) {
                    n_tokens -= 1;
                    jobserver->return_one();
                }
            }
        }

        // Update the runnable list.
        for(auto& slot : this->waiting_jobs)
        {
            assert(slot);
            const auto& deps = slot->dependencies();
            if( std::all_of(deps.begin(), deps.end(), [&](const std::string& s){ return completed_jobs.count(s) > 0; }) && slot->is_runnable() )
            {
                this->runnable_jobs.push_back(std::move(slot));
            }
        }
        auto new_end = std::remove_if(waiting_jobs.begin(), waiting_jobs.end(), [](const job_t& j){ return !j; });
        waiting_jobs.erase(new_end, waiting_jobs.end());

        ::std::sort(runnable_jobs.begin(), runnable_jobs.end(), [](const std::unique_ptr<Job>& a, const std::unique_ptr<Job>& b) {
            return a->name() < b->name();
        });

        // Is nothing runnable?
        if( this->runnable_jobs.empty() ) {
            // Is nothing running?
            if( this->running_jobs.empty()) {
                // BUG if there are jobs on the queue
                if( !this->waiting_jobs.empty() ) {
                    ::std::cerr << "BUG: Nothing runnable or running, but jobs are still waiting\n";
                }
                break ;
            }
            else {
                force_wait = true;
                continue ;
            }
        }

        if( this->running_jobs.empty() )
        {
            // Nothing running, so we don't need to acquire a token (we're using our own token)
        }
        else if( jobserver )
        {
            // JobServer is present, need to request a token
            assert(num_jobs == 0 || num_jobs > 1);
            // - Wait 100ms, short enough to not slow things down too much but not too short to be busy waiting all the time
            if( !jobserver->take_one(100) ) {
                // Check if one of our own tasks is complete.
                if( !wait_one(false) ) {
                    dump_state();
                    failed = true;
                    break;
                }
                continue;
            }
            n_tokens += 1;
        }

        auto job = ::std::move(this->runnable_jobs.front());
        this->runnable_jobs.pop_front();
        auto rjob = job->start();
        if( dry_run )
        {
            this->completed_jobs.insert(job->name());
            ::std::cout << "> " << rjob.exe_name;
            for(const auto& a : rjob.args.get_vec()) {
                ::std::cout << " " << a;
            }
            ::std::cout << ::std::endl;
            continue;
        }
        {
            ::std::cout << "--- ";
            os_support::set_console_colour(::std::cout, os_support::TerminalColour::Green);
            ::std::cout << job->verb() << " " << job->name();
            os_support::set_console_colour(::std::cout, os_support::TerminalColour::Default);
            auto num_complete = total_job_count - this->runnable_jobs.size() - (1+this->running_jobs.size()) - this->waiting_jobs.size();
            ::std::cout << " ("
                << std::fixed << std::setprecision(1) << (100 * static_cast<double>(num_complete) / total_job_count) << "% " 
                << this->running_jobs.size()+1 << "r," << this->runnable_jobs.size() << "w," << this->waiting_jobs.size() << "b"
                << "," << num_complete << "c"
                << "/" << total_job_count << "t)";
            ::std::cout << std::endl;
        }

        auto handle = this->spawn(rjob);
        this->running_jobs.push_back(RunningJob { handle, std::move(job), std::move(rjob) });
        dump_state();
    }
    while( !this->running_jobs.empty() )
    {
        failed |= !wait_one();
        dump_state();
    }
    // Release jobserver tokens
    if(jobserver) {
        while( n_tokens > 0 && 1+n_tokens > this->running_jobs.size()) {
            n_tokens -= 1;
            jobserver->return_one();
        }
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

bool JobList::wait_one(bool block)
{
    bool rv;
#ifdef _WIN32
    ::std::vector<HANDLE>   handles;
    for(const auto& j : this->running_jobs) {
        handles.push_back(j.handle.m_handle);
    }
    if(handles.size() > MAXIMUM_WAIT_OBJECTS) {
        handles.resize(MAXIMUM_WAIT_OBJECTS);
        ::std::cerr << "WARNING! Win32's WaitForMultipleObjects only supports up to " << MAXIMUM_WAIT_OBJECTS << " handles, but have " << handles.size() << ::std::endl;
    }
    auto wait_rv = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, block ? INFINITE : 0);
    if( !(WAIT_OBJECT_0 <= wait_rv && wait_rv < WAIT_OBJECT_0 + handles.size()) ) {
        if( !block )
            return true;
        return false;
    }

    auto idx = wait_rv - WAIT_OBJECT_0;
    auto handle = this->running_jobs[idx].handle.m_handle;
    RunningJob  rjob = ::std::move(this->running_jobs[idx]);
    this->running_jobs.erase(this->running_jobs.begin() + idx);

    DWORD status = 1;
    GetExitCodeProcess(handle, &status);
    rv = os_support::Process::handle_status(status);
#else
    int status = -1;
    auto pid = waitpid(0, &status, block ? 0 : WNOHANG);
    if(pid == 0) {
        if(block) {
            ::std::cerr << "`waitpid` returned zero when blocking?!" << std::endl;
            return false;
        }
        return true;
    }
    if(pid == (pid_t)-1)
    {
        ::std::cerr << "`waitpid` failed!" << std::endl;
        return false;
    }

    rv = os_support::Process::handle_status(status);

    auto i = ::std::find_if(this->running_jobs.begin(), this->running_jobs.end(), [&](const auto& e){ return e.handle.m_handle == pid; });
    if( i == this->running_jobs.end() ) {
        ::std::cerr << "`waitpid` returned an unknown pid (" << pid << ")" << std::endl;
        return false;
    }
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
        ::std::cout << "Completed " << rjob.job->name() << std::endl;
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
