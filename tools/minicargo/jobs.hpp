/*
 */
#pragma once
#include <string>
#include <memory>
#include <vector>
#include <deque>
#include <unordered_set>
#include "stringlist.h"
#include <path.h>
#include "os.hpp"

struct RunnableJob
{
    RunnableJob(const char* exe_name, StringList args, StringListKV env, ::helpers::path logfile, ::helpers::path working_directory={})
        : exe_name(exe_name)
        , args(std::move(args))
        , env(std::move(env))
        , logfile(std::move(logfile))
        , working_directory(std::move(working_directory))
    {
    }

    RunnableJob(RunnableJob&& ) = default;
    RunnableJob& operator=(RunnableJob&& ) = default;

    const char* exe_name;
    StringList args;
    StringListKV env;
    ::helpers::path logfile;
    ::helpers::path working_directory;
};
class Job
{
public:
    virtual ~Job() {}

    virtual const char* verb() const { return "BUILDING"; }
    virtual const std::string& name() const = 0;
    virtual const std::vector<std::string>& dependencies() const = 0;
    virtual bool is_runnable() const = 0;
    virtual RunnableJob start() = 0;
    virtual bool complete(bool was_successful) = 0;
};
class JobList
{
    typedef std::unique_ptr<Job>    job_t;
    struct RunningJob {
        os_support::Process handle;
        job_t   job;
        RunnableJob desc;
        RunningJob(RunningJob&& ) = default;
        RunningJob& operator=(RunningJob&& ) = default;
    };

    size_t  num_jobs;
    ::std::vector<job_t>    waiting_jobs;
    ::std::deque<job_t>    runnable_jobs;
    ::std::vector<RunningJob>   running_jobs;
    ::std::unordered_set<std::string>  completed_jobs;
public:
    JobList(size_t num_jobs)
        : num_jobs(num_jobs)
    {}
    void add_job(::std::unique_ptr<Job> job);
    bool run_all();

private:
    os_support::Process spawn(const RunnableJob& j);
    bool wait_one(bool block=true);
};
