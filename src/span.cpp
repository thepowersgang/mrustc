/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * span.cpp
 * - Spans and error handling
 */
#include <functional>
#include <iostream>
#include <span.hpp>
#include <parse/lex.hpp>
#include <common.hpp>

Span::Span(const Span& x):
    outer_span(x.outer_span),
    filename(x.filename),
    start_line(x.start_line),
    start_ofs(x.start_ofs),
    end_line(x.end_line),
    end_ofs(x.end_ofs)
{
}
Span::Span(const Position& pos):
    outer_span(),
    filename(pos.filename),
    start_line(pos.line),
    start_ofs(pos.ofs),
    end_line(pos.line),
    end_ofs(pos.ofs)
{
}
Span::Span():
    outer_span(),
    filename("")/*,
    start_line(0), start_ofs(0),
    end_line(0), end_ofs(0) // */
{
    DEBUG("Empty span");
    //filename = FMT(":" << __builtin_return_address(0));
}

void Span::bug(::std::function<void(::std::ostream&)> msg) const
{
    ::std::cerr << this->filename << ":" << this->start_line << ": BUG:";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    abort();
}

void Span::error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const {
    ::std::cerr << this->filename << ":" << this->start_line << ": error:" << tag <<":";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    abort();
}
void Span::warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const {
    ::std::cerr << this->filename << ":" << this->start_line << ": warning:" << tag << ":";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    //abort();
}
void Span::note(::std::function<void(::std::ostream&)> msg) const {
    ::std::cerr << this->filename << ":" << this->start_line << ": note:";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    //abort();
}

::std::ostream& operator<<(::std::ostream& os, const Span& sp)
{
    os << sp.filename << ":" << sp.start_line;
    return os;
}
