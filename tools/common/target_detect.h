/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * common/target_detect.h
 * - Auto-magical host target detection
 */
#pragma once

// - Windows (MSVC)
#ifdef _MSC_VER
# if defined(_WIN64)
#  define DEFAULT_TARGET_NAME "x86_64-pc-windows-msvc"
# else
#  define DEFAULT_TARGET_NAME "x86-pc-windows-msvc"
# endif
// - Linux
#elif defined(__linux__)
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif
# include <features.h>
# if defined(__amd64__)
#  if defined(_ILP32)
#   define DEFAULT_TARGET_NAME "x86_64-unknown-linux-gnux32"
#  elif defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "x86_64-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "x86_64-unknown-linux-musl"
#  endif
# elif defined(__aarch64__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "aarch64-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "aarch64-unknown-linux-musl"
#  endif
# elif defined(__arm__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "arm-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "arm-unknown-linux-musl"
#  endif
# elif defined(__i386__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "i586-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "i586-unknown-linux-musl"
#  endif
# elif defined(__m68k__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "m68k-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "m68k-unknown-linux-musl"
#  endif
# elif defined(__powerpc64__) && defined(__BIG_ENDIAN__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "powerpc64-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "powerpc64-unknown-linux-musl"
#  endif
# elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "powerpc64le-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "powerpc64le-unknown-linux-musl"
#  endif
# elif defined(__riscv) && __riscv_xlen == 64
#  if defined(__USE_GNU)
#   define DEFAULT_TARGET_NAME "riscv64-unknown-linux-gnu"
#  else
#   define DEFAULT_TARGET_NAME "riscv64-unknown-linux-musl"
#  endif
# else
#  warning "Unable to detect a suitable default target (linux-gnu)"
# endif
// - msys/cygwin
#elif defined(__CYGWIN__)
# if defined(__x86_64__)
#  define DEFAULT_TARGET_NAME "x86_64-pc-windows-gnu"
# else
#  define DEFAULT_TARGET_NAME "i586-pc-windows-gnu"
# endif
// - MinGW
#elif defined(__MINGW32__)
# if defined(_WIN64)
#  define DEFAULT_TARGET_NAME "x86_64-pc-windows-gnu"
# else
#  define DEFAULT_TARGET_NAME "i586-pc-windows-gnu"
# endif
// - FreeBSD
#elif defined(__FreeBSD__)
# if defined(__amd64__)
#  define DEFAULT_TARGET_NAME "x86_64-unknown-freebsd"
# elif defined(__aarch64__)
#  define DEFAULT_TARGET_NAME "aarch64-unknown-freebsd"
# elif defined(__arm__)
#  define DEFAULT_TARGET_NAME "arm-unknown-freebsd"
# elif defined(__i386__)
#  define DEFAULT_TARGET_NAME "i686-unknown-freebsd"
# else
#  warning "Unable to detect a suitable default target (FreeBSD)"
# endif
// - NetBSD
#elif defined(__NetBSD__)
# if defined(__amd64__)
#  define DEFAULT_TARGET_NAME "x86_64-unknown-netbsd"
# else
#  warning "Unable to detect a suitable default target (NetBSD)"
# endif
// - OpenBSD
#elif defined(__OpenBSD__)
# if defined(__amd64__)
#  define DEFAULT_TARGET_NAME "x86_64-unknown-openbsd"
# elif defined(__aarch64__)
#  define DEFAULT_TARGET_NAME "aarch64-unknown-openbsd"
# elif defined(__arm__)
#  define DEFAULT_TARGET_NAME "arm-unknown-openbsd"
# elif defined(__i386__)
#  define DEFAULT_TARGET_NAME "i686-unknown-openbsd"
# else
#  warning "Unable to detect a suitable default target (OpenBSD)"
# endif
// - DragonFly
#elif defined(__DragonFly__)
# define DEFAULT_TARGET_NAME "x86_64-unknown-dragonfly"
// - Apple devices
#elif defined(__APPLE__)
# if defined(__aarch64__)
#  define DEFAULT_TARGET_NAME "aarch64-apple-darwin"
# elif defined(__ppc64__)
#  define DEFAULT_TARGET_NAME "powerpc64-apple-darwin"
# elif defined(__ppc__)
#  define DEFAULT_TARGET_NAME "powerpc-apple-darwin"
# else
#  define DEFAULT_TARGET_NAME "x86_64-apple-darwin"
#endif
// - Haiku
#elif defined(__HAIKU__)
# if defined(__x86_64__)
# define DEFAULT_TARGET_NAME "x86_64-unknown-haiku"
# elif defined(__arm__)
# define DEFAULT_TARGET_NAME "arm-unknown-haiku"
# else
#  warning "Unable to detect a suitable default target (Haiku)"
# endif
// - Unknown
#else
# warning "Unable to detect a suitable default target"
#endif
#ifndef DEFAULT_TARGET_NAME
# define DEFAULT_TARGET_NAME	""
#endif
