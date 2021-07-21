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
	Rustc1_39,
	Rustc1_54,
};

// Defined in main.cpp
extern TargetVersion	gTargetVersion;

#define TARGETVER_MOST_1_19  (gTargetVersion <= TargetVersion::Rustc1_19)
#define TARGETVER_MOST_1_29  (gTargetVersion <= TargetVersion::Rustc1_29)
#define TARGETVER_MOST_1_39  (gTargetVersion <= TargetVersion::Rustc1_39)
//#define TARGETVER_MOST_1_54  (gTargetVersion <= TargetVersion::Rustc1_54)
#define TARGETVER_LEAST_1_29  (gTargetVersion >= TargetVersion::Rustc1_29)
#define TARGETVER_LEAST_1_39  (gTargetVersion >= TargetVersion::Rustc1_39)
#define TARGETVER_LEAST_1_54  (gTargetVersion >= TargetVersion::Rustc1_54)
