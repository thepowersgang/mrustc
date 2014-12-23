/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#define FOREACH(basetype, it, src)  for(basetype::const_iterator it = src.begin(); it != src.end(); ++ it)
#define FOREACH_M(basetype, it, src)  for(basetype::iterator it = src.begin(); it != src.end(); ++ it)

#endif
