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

Span::Span(Span parent, RcString filename, unsigned int start_line, unsigned int start_ofs,  unsigned int end_line, unsigned int end_ofs):
    m_ptr(SpanInner_Source::alloc( parent, ::std::move(filename), start_line, start_ofs, end_line, end_ofs ))
{}
Span::Span(Span parent, const Position& pos):
    m_ptr(SpanInner_Source::alloc( parent, pos.filename, pos.line,pos.ofs, pos.line,pos.ofs ))
{
}
Span::Span(Span parent, RcString source_crate, RcString macro_name)
    : m_ptr(SpanInner_Macro::alloc(parent, source_crate, macro_name))
{
}
Span::Span(const Span& x):
    m_ptr(x.m_ptr)
{
    if( m_ptr ) {
        m_ptr->reference_count += 1;
    }
}
Span::~Span()
{
    if(m_ptr)
    {
        m_ptr->reference_count --;
        if( m_ptr->reference_count == 0 )
        {
            delete m_ptr;
        }
        m_ptr = nullptr;
    }
}
const SpanInner_Source& Span::get_top_file_span() const {
    auto* top_span = this;
    while(top_span->get() && (*top_span)->parent_span != Span())
    {
        top_span = &(*top_span)->parent_span;
    }
    if( const auto* ts = dynamic_cast<const SpanInner_Source*>( top_span->get() ) ) {
        return *ts;
    }
    TODO(*this, "Top span isn't source?");
}

void Span::print_span_message(::std::function<void(::std::ostream&)> tag, ::std::function<void(::std::ostream&)> msg) const
{
    const Span& sp = *this;
    auto& sink = ::std::cerr;
    sink << sp << " ";
    //sink << sp->filename << ":" << sp->start_line << ": ";
    tag(sink);
    sink << ":";
    msg(sink);
    sink << ::std::endl;
    
    if( sp.get() )
    {
        for(auto parent = sp->parent_span; parent != Span(); parent = parent->parent_span)
        {
            sink << parent << ": note: From here" << ::std::endl;
        }
    }

    sink << ::std::flush;
}
void Span::bug(::std::function<void(::std::ostream&)> msg) const
{
    print_span_message([](auto& os){os << "BUG";}, msg);
#ifndef _WIN32
    abort();
#else
    exit(1);
#endif
}

void Span::error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const {
    print_span_message([&](auto& os){os << "error:" << tag;}, msg);
#ifndef _WIN32
    abort();
#else
    exit(1);
#endif
}
void Span::warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const {
    print_span_message([&](auto& os){os << "warn:" << tag;}, msg);
}
void Span::note(::std::function<void(::std::ostream&)> msg) const {
    print_span_message([](auto& os){os << "note";}, msg);
}

SpanInner::~SpanInner()
{
}

SpanInner_Source::~SpanInner_Source()
{
}
void SpanInner_Source::fmt(::std::ostream& os) const
{
    os << this->filename;
    if( this->start_line != this->end_line ) {
        os << ":" << this->start_line << "-" << this->end_line;
    }
    else if( this->start_ofs != this->end_ofs ) {
        os << ":" << this->start_line << ":" << this->start_ofs << "-" << this->end_ofs;
    }
    else {
        os << ":" << this->start_line << ":" << this->start_ofs;
    }
}

SpanInner_Macro::~SpanInner_Macro()
{
}
void SpanInner_Macro::fmt(::std::ostream& os) const
{
    os << "MACRO<::\"" << this->crate << "\"::" << this->macro << ">";
}
/*static*/ SpanInner* SpanInner_Macro::alloc(Span parent, RcString crate, RcString macro)
{
    auto rv = new SpanInner_Macro;
    rv->reference_count = 1;
    rv->parent_span = std::move(parent);
    rv->crate = std::move(crate);
    rv->macro = std::move(macro);
    return rv;
}

::std::ostream& operator<<(::std::ostream& os, const Span& sp)
{
    if( sp.m_ptr ) {
        sp.m_ptr->fmt(os);
    }
    else {
        os << "<null>";
    }
    return os;
}
