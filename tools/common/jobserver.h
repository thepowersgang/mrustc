/*
* mrustc common tools
* - by John Hodge (Mutabah)
*
* tools/common/jobserver.h
* - An interface to (or emulation of) make's jobserver
*/
#pragma once
#include <memory>

class JobServer
{
public:
    virtual ~JobServer();
    static ::std::unique_ptr<JobServer> create(size_t max_jobs);

    virtual bool take_one(unsigned long timeout_ms) = 0;
    virtual void return_one() = 0;
};
