
MRUSTC := bin/mrustc
MINICARGO := tools/bin/minicargo

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

output/rustc: $(MRUSTC) $(MINICARGO) output/libstd.hir output/libtest.hir
	$(MINICARGO) rustc-nightly/src/rustc --vendor-dir rustc-nightly/src/vendor
