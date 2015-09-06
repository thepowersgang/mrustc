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

void Span::bug(::std::function<void(::std::ostream&)> msg) const
{
    ::std::cerr << this->filename << ":" << this->start_line << ": BUG:";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    abort();
}

void Span::error(ErrorType tag, ::std::function<void(::std::ostream&)> msg) const {
    ::std::cerr << this->filename << ":" << this->start_line << ": error:";
    msg(::std::cerr);
    ::std::cerr << ::std::endl;
    abort();
}
void Span::warning(WarningType tag, ::std::function<void(::std::ostream&)> msg) const {
    ::std::cerr << this->filename << ":" << this->start_line << ": warning:";
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
