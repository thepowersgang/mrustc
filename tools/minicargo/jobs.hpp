/*
 */
#pragma once
#include <string>
#include <memory>
#include <vector>
#include <deque>
#include "stringlist.h"
#include <path.h>

struct RunnableJob
{
    virtual ~RunnableJob() {}
    virtual const char* exe_name() const = 0;
    virtual const StringList& args() const = 0;
    virtual const StringListKV& env() const = 0;
    virtual const ::helpers::path& logfile() const = 0;
    virtual const ::helpers::path& working_directory() const = 0;
};
class Job
{
public:
    virtual ~Job() {}

    virtual const std::string& name() const = 0;
    virtual const std::vector<std::string>& dependencies() const = 0;
    virtual bool is_runnable() const = 0;
    virtual std::unique_ptr<RunnableJob> start() = 0;
};
class JobList
{
    typedef std::unique_ptr<Job>    job_t;
    typedef std::unique_ptr<RunnableJob>    rjob_t;
    #ifdef WIN32
    typedef void*   child_t
    #else
    typedef pid_t   child_t;
    #endif

    size_t  num_jobs;
    ::std::vector<job_t>    waiting_jobs;
    ::std::deque<job_t>    runnable_jobs;
    ::std::vector<::std::pair<child_t,rjob_t>>   running_jobs;
    ::std::vector<std::string>  completed_jobs;
public:
    JobList(size_t num_jobs)
        : num_jobs(num_jobs)
    {}
    void add_job(::std::unique_ptr<Job> job);
    void run_all();
private:
    child_t spawn(const RunnableJob& j);
    bool wait_one();
};
