/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/target_version.hpp
 * - mrustc target lanuage version definitions
 */
#pragma once

enum class TargetVersion {
#define X_TARGET_VERSION(name, str)	name,
#include "target_versions.def"
#undef X_TARGET_VERSION
};

// Defined in main.cpp
extern TargetVersion	gTargetVersion;

#define TARGETVER_MOST_1_19  (gTargetVersion <= TargetVersion::Rustc1_19)
#define TARGETVER_MOST_1_29  (gTargetVersion <= TargetVersion::Rustc1_29)
#define TARGETVER_MOST_1_39  (gTargetVersion <= TargetVersion::Rustc1_39)
#define TARGETVER_MOST_1_54  (gTargetVersion <= TargetVersion::Rustc1_54)
#define TARGETVER_MOST_1_74  (gTargetVersion <= TargetVersion::Rustc1_74)
#define TARGETVER_MOST_1_90  (gTargetVersion <= TargetVersion::Rustc1_90)
#define TARGETVER_LEAST_1_29  (gTargetVersion >= TargetVersion::Rustc1_29)
#define TARGETVER_LEAST_1_39  (gTargetVersion >= TargetVersion::Rustc1_39)
#define TARGETVER_LEAST_1_54  (gTargetVersion >= TargetVersion::Rustc1_54)
#define TARGETVER_LEAST_1_74  (gTargetVersion >= TargetVersion::Rustc1_74)
#define TARGETVER_LEAST_1_90  (gTargetVersion >= TargetVersion::Rustc1_90)
#define TARGETVER_LEAST_1_97  (gTargetVersion >= TargetVersion::Rustc1_97)
