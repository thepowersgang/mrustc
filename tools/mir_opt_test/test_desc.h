/*
 * test_desc.h
 * - Description of a MIR optimisation test
 */
#pragma once

#include <hir/crate_ptr.hpp>
#include <hir/type.hpp>
#include <hir/path.hpp>
#include <path.h>

struct MirOptTestFile
{
    ::std::string   m_filename;
    ::HIR::CratePtr m_crate;

    struct Test
    {
        ::HIR::SimplePath input_function;
        ::HIR::SimplePath output_template_function;
    };
    ::std::vector<Test>  m_tests;

    static MirOptTestFile  load_from_file(const helpers::path& p);
};
