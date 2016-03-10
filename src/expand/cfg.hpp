
#pragma once

extern void Cfg_SetFlag(::std::string name);
extern void Cfg_SetValue(::std::string name, ::std::string val);
extern bool check_cfg(Span sp, const ::AST::MetaItem& mi);
