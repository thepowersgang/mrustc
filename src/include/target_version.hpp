/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/target_version.hpp
 * - mrustc target lanuage version definitions
 */
#pragma once

enum class TargetVersion {
	Rustc1_19,
	Rustc1_29,
};

// Defined in main.cpp
extern TargetVersion	gTargetVersion;

#define TARGETVER_1_19  (gTargetVersion == TargetVersion::Rustc1_19)
#define TARGETVER_1_29  (gTargetVersion == TargetVersion::Rustc1_29)
