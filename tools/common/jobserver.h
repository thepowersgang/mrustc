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
    virtual ~JobServer() {}
    /// <summary>
    /// Create a jobserver instance (client, or server if `server_jobs` is non-zero and there isn't already a server)
    /// </summary>
    /// <param name="server_jobs">Number of downstream job slots to expose</param>
    /// <returns></returns>
    static ::std::unique_ptr<JobServer> create(size_t server_jobs);

    virtual bool take_one(unsigned long timeout_ms) = 0;
    virtual void return_one() = 0;
};
