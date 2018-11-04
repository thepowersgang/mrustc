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

struct ProtoSpan
{
    RcString   filename;

    unsigned int start_line;
    unsigned int start_ofs;
};
struct Span
{
    ::std::shared_ptr<Span> outer_span; // Expansion target for macros
    RcString    filename;

    unsigned int start_line;
    unsigned int start_ofs;
    unsigned int end_line;
    unsigned int end_ofs;

    Span(RcString filename, unsigned int start_line, unsigned int start_ofs,  unsigned int end_line, unsigned int end_ofs):
        filename( ::std::move(filename) ),
        start_line(start_line),
        start_ofs(start_ofs),
        end_line(end_line),
        end_ofs(end_ofs)
    {}
    Span(const Span& x) = default;
    Span(Span&& x) = default;
    Span(const Position& position);
    Span();

    Span& operator=(const Span& x) = default;
    Span& operator=(Span&& x) = default;

    void bug(::std::function<void(::std::ostream&)> msg) const;
    void error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const;
    void warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const;
    void note(::std::function<void(::std::ostream&)> msg) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Span& sp);
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
#define BUG(span, msg)  do { ::Span(span).bug([&](::std::ostream& os) { os << __FILE__ << ":" << __LINE__ << ": " << msg; }); throw ::std::runtime_error("Bug fell through"); } while(0)
#define TODO(span, msg)  do { const char* __TODO_func = __func__; ::Span(span).bug([&](::std::ostream& os) { os << __FILE__ << ":" << __LINE__ << ": TODO: " << __TODO_func << " - " << msg; }); throw ::std::runtime_error("Bug (todo) fell through"); } while(0)

#define ASSERT_BUG(span, cnd, msg)  do { if( !(cnd) ) { ::Span(span).bug([&](::std::ostream& os) { os << "ASSERT FAIL: " << __FILE__ << ":" << __LINE__ << ":" #cnd << ": " << msg; }); throw ::std::runtime_error("Bug fell through"); } } while(0)
