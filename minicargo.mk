
MRUSTC := bin/mrustc
MINICARGO := tools/bin/minicargo
RUSTCSRC := rustc-nightly/

LLVM_CONFIG := rustc-nightly/build/bin/llvm-config

.PHONY: bin/mrustc tools/bin/minicargo output/libsrc.hir output/libtest.hir output/libpanic_unwind.hir output/rustc

all: output/rustc

$(MRUSTC):
	$(MAKE) -f Makefile all
	test -e $@

$(MINICARGO):
	$(MAKE) -C tools/minicargo/
	test -e $@

output/libstd.hir: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) rustc-nightly/src/libstd --script-overrides script-overrides/nightly-2017-07-08/
	test -e $@
output/libpanic_unwind.hir: $(MRUSTC) $(MINICARGO) output/libstd.hir
	$(MINICARGO) rustc-nightly/src/libpanic_unwind --script-overrides script-overrides/nightly-2017-07-08/
	test -e $@
output/libtest.hir: $(MRUSTC) $(MINICARGO) output/libstd.hir output/libpanic_unwind.hir
	$(MINICARGO) rustc-nightly/src/libtest --vendor-dir rustc-nightly/src/vendor
	test -e $@

output/rustc: $(MRUSTC) $(MINICARGO) output/libstd.hir output/libtest.hir $(LLVM_CONFIG)
	LLVM_CONFIG=$(abspath $(LLVM_CONFIG)) $(MINICARGO) rustc-nightly/src/rustc --vendor-dir rustc-nightly/src/vendor

LLVM_CMAKE_OPTS := LLVM_TARGET_ARCH=$(firstword $(subst -, ,$(RUSTC_TARGET))) LLVM_DEFAULT_TARGET_TRIPLE=$(RUSTC_TARGET)
LLVM_CMAKE_OPTS += LLVM_TARGETS_TO_BUILD=X86#;ARM;AArch64;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX
LLVM_CMAKE_OPTS += LLVM_ENABLE_ASSERTIONS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_EXAMPLES=OFF LLVM_INCLUDE_TESTS=OFF LLVM_INCLUDE_DOCS=OFF
LLVM_CMAKE_OPTS += LLVM_ENABLE_ZLIB=OFF LLVM_ENABLE_TERMINFO=OFF LLVM_ENABLE_LIBEDIT=OFF WITH_POLLY=OFF
LLVM_CMAKE_OPTS += CMAKE_CXX_COMPILER="g++" CMAKE_C_COMPILER="gcc"


$(LLVM_CONFIG): $(RUSTCSRC)build/Makefile
	$Vcd rustc-nightly/build && $(MAKE)
$(RUSTCSRC)build/Makefile: $(RUSTCSRC)src/llvm/CMakeLists.txt
	@mkdir -p $(RUSTCSRC)build
	$Vcd rustc-nightly/build && cmake $(addprefix -D , $(LLVM_CMAKE_OPTS)) ../src/llvm

