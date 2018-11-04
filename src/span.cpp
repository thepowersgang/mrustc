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
    //DEBUG("Empty span");
    //filename = FMT(":" << __builtin_return_address(0));
}

namespace {
    void print_span_message(const Span& sp, ::std::function<void(::std::ostream&)> tag, ::std::function<void(::std::ostream&)> msg)
    {
        auto& sink = ::std::cerr;
        sink << sp.filename << ":" << sp.start_line << ": ";
        tag(sink);
        sink << ":";
        msg(sink);
        sink << ::std::endl;
        const auto* parent = sp.outer_span.get();
        while(parent)
        {
            sink << parent->filename << ":" << parent->start_line << ": note: From here" << ::std::endl;
            parent = parent->outer_span.get();
        }
    }
}
void Span::bug(::std::function<void(::std::ostream&)> msg) const
{
    print_span_message(*this, [](auto& os){os << "BUG";}, msg);
#ifndef _WIN32
    abort();
#else
    exit(1);
#endif
}

void Span::error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const {
    print_span_message(*this, [&](auto& os){os << "error:" << tag;}, msg);
#ifndef _WIN32
    abort();
#else
    exit(1);
#endif
}
void Span::warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const {
    print_span_message(*this, [&](auto& os){os << "warning" << tag;}, msg);
}
void Span::note(::std::function<void(::std::ostream&)> msg) const {
    print_span_message(*this, [](auto& os){os << "note";}, msg);
}

::std::ostream& operator<<(::std::ostream& os, const Span& sp)
{
    os << sp.filename << ":" << sp.start_line;
    return os;
}
