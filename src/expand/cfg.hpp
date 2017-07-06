
#pragma once

#include <ast/attrs.hpp>

extern void Cfg_SetFlag(::std::string name);
extern void Cfg_SetValue(::std::string name, ::std::string val);
extern void Cfg_SetValueCb(::std::string name, ::std::function<bool(const ::std::string&)> cb);
extern bool check_cfg(Span sp, const ::AST::MetaItem& mi);
