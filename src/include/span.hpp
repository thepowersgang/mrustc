/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/span.hpp
 * - Spans and error handling
 */
#pragma once

#include <rc_string.hpp>
#include <functional>
#include <memory>

enum ErrorType
{
    E0000,
};
enum WarningType
{
    W0000,
};

class Position;
struct SpanInner;
struct SpanInner_Source;

struct Span
{
private:
    SpanInner*  m_ptr;
    //static SpanInner    s_empty_span;
public:
    Span()
        //: m_ptr(&s_empty_span)
        : m_ptr(nullptr)
    {}
    Span(Span parent, RcString filename, unsigned int start_line, unsigned int start_ofs,  unsigned int end_line, unsigned int end_ofs);
    Span(Span parent, const Position& position);
    Span(Span parent, RcString source_crate, RcString macro_name);
    ~Span();

    Span(const Span& x);
    Span(Span&& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr = nullptr;
    }

    Span& operator=(const Span& x)
    {
        this->~Span();
        new (this) Span(x);
        return *this;
    }
    Span& operator=(Span&& x)
    {
        this->~Span();
        new (this) Span(std::move(x));
        return *this;
    }

    operator bool() const { return m_ptr != nullptr; }
    bool operator==(const Span& x) const { return m_ptr == x.m_ptr; }
    bool operator!=(const Span& x) const { return !(*this == x); }

    const SpanInner* get() const { return m_ptr; }
    //const SpanInner& operator*() const { return *m_ptr; }
    const SpanInner* operator->() const { return m_ptr; }

    const SpanInner_Source& get_top_file_span() const;

    void bug(::std::function<void(::std::ostream&)> msg) const;
    void error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const;
    void warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const;
    void note(::std::function<void(::std::ostream&)> msg) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Span& sp);
private:
    void print_span_message(::std::function<void(::std::ostream&)> tag, ::std::function<void(::std::ostream&)> msg) const;
};
struct ProtoSpan
{
    // If `span` is populated, then this `ProtoSpan` was from a macro expansion
    Span    span;
    RcString   filename;

    unsigned int start_line;
    unsigned int start_ofs;
};
struct SpanInner
{
    friend struct Span;
protected:
    size_t  reference_count;
public:
    Span    parent_span;

    virtual ~SpanInner() = 0;
    virtual void fmt(::std::ostream& os) const = 0;
    virtual RcString crate_name() const = 0;
};
struct SpanInner_Source:
    public SpanInner
{
    friend struct Span;
public:
    RcString    filename;

    unsigned int start_line;
    unsigned int start_ofs;
    unsigned int end_line;
    unsigned int end_ofs;

    ~SpanInner_Source() override;
    void fmt(::std::ostream& os) const override;
    RcString crate_name() const override { return RcString(); }

private:
    static SpanInner* alloc(Span parent, RcString filename, unsigned int start_line, unsigned int start_ofs,  unsigned int end_line, unsigned int end_ofs) {
        auto* rv = new SpanInner_Source();
        rv->reference_count = 1;
        rv->parent_span = parent;
        rv->filename = ::std::move(filename);
        rv->start_line = start_line;
        rv->start_ofs = start_ofs;
        rv->end_line = end_line;
        rv->end_ofs = end_ofs;
        return rv;
    }
};
struct SpanInner_Macro:
    public SpanInner
{
    friend struct Span;
    RcString    crate;
    RcString    macro;

    ~SpanInner_Macro() override;
    void fmt(::std::ostream& os) const override;
    RcString crate_name() const override { return crate; }

private:
    static SpanInner* alloc(Span parent, RcString crate, RcString macro);
};

template<typename T>
struct Spanned
{
    Span    sp;
    T   ent;
};
template<typename T>
Spanned<T> make_spanned(Span sp, T val) {
    return Spanned<T> { ::std::move(sp), ::std::move(val) };
}

#define ERROR(span, code, msg)  do { ::Span(span).error(code, [&](::std::ostream& os) { os << msg; }); throw ::std::runtime_error("Error fell through" #code); } while(0)
#define WARNING(span, code, msg)  do { ::Span(span).warning(code, [&](::std::ostream& os) { os << msg; }); } while(0)
#define NOTE(span, msg)  do { ::Span(span).note([&](::std::ostream& os) { os << msg; }); } while(0)
#define BUG(span, msg)  do { ::Span(span).bug([&](::std::ostream& os) { os << __FILE__ << ":" << __LINE__ << ": " << msg; }); throw ::std::runtime_error("Bug fell through"); } while(0)
#define TODO(span, msg)  do { const char* __TODO_func = __func__; ::Span(span).bug([&](::std::ostream& os) { os << __FILE__ << ":" << __LINE__ << ": TODO: " << __TODO_func << " - " << msg; }); throw ::std::runtime_error("Bug (todo) fell through"); } while(0)

#define ASSERT_BUG(span, cnd, msg)  do { if( !(cnd) ) { ::Span(span).bug([&](::std::ostream& os) { os << "ASSERT FAIL: " << __FILE__ << ":" << __LINE__ << ":" #cnd << ": " << msg; }); throw ::std::runtime_error("Bug fell through"); } } while(0)
