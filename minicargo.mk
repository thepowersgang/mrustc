
RUSTC_CHANNEL ?= nightly
RUSTC_VERSION ?= 2017-07-08
OVERRIDE_SUFFIX ?= # -linux
OUTDIR := output/

MRUSTC := bin/mrustc
MINICARGO := tools/bin/minicargo
ifeq ($(RUSTC_CHANNEL),nightly)
	RUSTCSRC := rustc-nightly/
else
	RUSTCSRC := rustc-$(RUSTC_VERSION)-src/
endif

LLVM_CONFIG := $(RUSTCSRC)build/bin/llvm-config
RUSTC_TARGET := x86_64-unknown-linux-gnu
OVERRIDE_DIR := script-overrides/$(RUSTC_CHANNEL)-$(RUSTC_VERSION)$(OVERRIDE_SUFFIX)/

.PHONY: bin/mrustc tools/bin/minicargo $(OUTDIR)libsrc.hir $(OUTDIR)libtest.hir $(OUTDIR)libpanic_unwind.hir $(OUTDIR)rustc $(OUTDIR)cargo

all: $(OUTDIR)rustc

mini: $(OUTDIR)libpanic_unwind.hir $(OUTDIR)libtest.hit

$(MRUSTC):
	$(MAKE) -f Makefile all
	test -e $@

$(MINICARGO):
	$(MAKE) -C tools/minicargo/
	test -e $@

$(OUTDIR)libstd.hir: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) $(RUSTCSRC)src/libstd --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR)
	test -e $@
$(OUTDIR)libpanic_unwind.hir: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.hir
	$(MINICARGO) $(RUSTCSRC)src/libpanic_unwind --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR)
	test -e $@
$(OUTDIR)libtest.hir: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.hir $(OUTDIR)libpanic_unwind.hir
	$(MINICARGO) $(RUSTCSRC)src/libtest --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)
	test -e $@

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-$(RUSTC_CHANNEL)-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib

$(OUTDIR)rustc: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.hir $(OUTDIR)libtest.hir $(LLVM_CONFIG)
	$(RUSTC_ENV_VARS) $(MINICARGO) $(RUSTCSRC)src/rustc --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)
$(OUTDIR)cargo: $(MRUSTC) $(OUTDIR)libstd.hir
	$(MINICARGO) $(RUSTCSRC)src/tools/cargo --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)

# Reference $(RUSTCSRC)src/bootstrap/native.rs for these values
LLVM_CMAKE_OPTS := LLVM_TARGET_ARCH=$(firstword $(subst -, ,$(RUSTC_TARGET))) LLVM_DEFAULT_TARGET_TRIPLE=$(RUSTC_TARGET)
LLVM_CMAKE_OPTS += LLVM_TARGETS_TO_BUILD=X86#;ARM;AArch64;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX
LLVM_CMAKE_OPTS += LLVM_ENABLE_ASSERTIONS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_EXAMPLES=OFF LLVM_INCLUDE_TESTS=OFF LLVM_INCLUDE_DOCS=OFF
LLVM_CMAKE_OPTS += LLVM_ENABLE_ZLIB=OFF LLVM_ENABLE_TERMINFO=OFF LLVM_ENABLE_LIBEDIT=OFF WITH_POLLY=OFF
LLVM_CMAKE_OPTS += CMAKE_CXX_COMPILER="g++" CMAKE_C_COMPILER="gcc"


$(LLVM_CONFIG): $(RUSTCSRC)build/Makefile
	$Vcd $(RUSTCSRC)build && $(MAKE)
$(RUSTCSRC)build/Makefile: $(RUSTCSRC)src/llvm/CMakeLists.txt
	@mkdir -p $(RUSTCSRC)build
	$Vcd $(RUSTCSRC)build && cmake $(addprefix -D , $(LLVM_CMAKE_OPTS)) ../src/llvm


#
# Developement-only targets
#
$(OUTDIR)libnum.hir: $(MRUSTC) $(OUTDIR)libstd.hir
	$(MINICARGO) $(RUSTCSRC)src/vendor/num --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)

