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

SpanInner Span::s_empty_span;

Span::Span(Span parent, RcString filename, unsigned int start_line, unsigned int start_ofs,  unsigned int end_line, unsigned int end_ofs):
    m_ptr(SpanInner::alloc( parent, ::std::move(filename), start_line, start_ofs, end_line, end_ofs ))
{}
Span::Span(Span parent, const Position& pos):
    m_ptr(SpanInner::alloc( parent, pos.filename, pos.line,pos.ofs, pos.line,pos.ofs ))
{
}
Span::Span(const Span& x):
    m_ptr(x.m_ptr)
{
    m_ptr->reference_count += 1;
}
Span::~Span()
{
    if(m_ptr && m_ptr != &s_empty_span)
    {
        m_ptr->reference_count --;
        if( m_ptr->reference_count == 0 )
        {
            delete m_ptr;
        }
        m_ptr = nullptr;
    }
}

namespace {
    void print_span_message(const Span& sp, ::std::function<void(::std::ostream&)> tag, ::std::function<void(::std::ostream&)> msg)
    {
        auto& sink = ::std::cerr;
        sink << sp << " ";
        //sink << sp->filename << ":" << sp->start_line << ": ";
        tag(sink);
        sink << ":";
        msg(sink);
        sink << ::std::endl;
        
        for(auto parent = sp->parent_span; parent != Span(); parent = parent->parent_span)
        {
            sink << parent->filename << ":" << parent->start_line << ": note: From here" << ::std::endl;
        }

        sink << ::std::flush;
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
    print_span_message(*this, [&](auto& os){os << "warn:" << tag;}, msg);
}
void Span::note(::std::function<void(::std::ostream&)> msg) const {
    print_span_message(*this, [](auto& os){os << "note";}, msg);
}

::std::ostream& operator<<(::std::ostream& os, const Span& sp)
{
    os << sp->filename;
    if( sp->start_line != sp->end_line ) {
        os << ":" << sp->start_line << "-" << sp->end_line;
    }
    else if( sp->start_ofs != sp->end_ofs ) {
        os << ":" << sp->start_line << ":" << sp->start_ofs << "-" << sp->end_ofs;
    }
    else {
        os << ":" << sp->start_line << ":" << sp->start_ofs;
    }
    return os;
}
