# Genna — complete engine + genna-curate curation tool
CC      = cc
CFLAGS  = -O2 -g -D_GNU_SOURCE -Iinclude
# genna_persist.c joins the engine set: the engine's mutators call into it to
# log an edit before applying it, so every binary that can edit must have it.
ENGINE  = src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c
LM      = -lm
# GetProcessMemoryInfo for the RSS readout in vbench (Windows only)
PSAPI   = $(if $(filter Windows_NT,$(OS)),-lpsapi,)

# Sanitizer build. mingw-w64 gcc ships no libasan/libubsan, so the sanitized
# builds use clang, which does. See tools/run_sanitized.sh.
SAN_CC     = clang
SAN_FLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

.PHONY: all clean test fuzz persist bench sanitize

all: genna-curate embedtest simdtest comptest

# --- the curation tool (engine + working embedding near-dedup) ---
genna-curate: genna_curate.c $(ENGINE)
	$(CC) $(CFLAGS) genna_curate.c $(ENGINE) -o genna-curate $(LM)

# --- standalone embedding (near-dedup vs exact match head-to-head) ---
embedtest: genna_embed.c
	$(CC) $(CFLAGS) genna_embed.c -o embedtest $(LM)

# --- AVX2 SIMD substring scan (beats memmem) ---
simdtest: genna_simd.c
	$(CC) -O2 -mavx2 -D_GNU_SOURCE genna_simd.c -o simdtest

# --- zlib chunk compression layer (git parity) ---
comptest: genna_compress.c
	$(CC) $(CFLAGS) genna_compress.c -o comptest -lz

# --- benchmarks vs real tools (need corpora present) ---
# genna_vdict.c REPLACES genna_dict2.c (it defines the same gn_dict_* symbols
# for video); listing both is a multiple-definition link error.
ENGINE_V = src/genna_engine3.c src/genna_ext.c src/genna_vdict.c src/genna_persist.c
bench: $(ENGINE) src/genna_net.c src/genna_vdict.c
	$(CC) $(CFLAGS) tests/realbench.c  $(ENGINE)                       -o realbench
	$(CC) $(CFLAGS) tests/gitcmp.c     $(ENGINE)                       -o gitcmp
	$(CC) $(CFLAGS) tests/rsynccmp.c   $(ENGINE) src/genna_net.c       -o rsynccmp
	$(CC) $(CFLAGS) -DGN_CHUNK_TARGET_TOKENS=30 tests/vbench.c $(ENGINE_V) -o vbench $(PSAPI)

# --- correctness tests ---
test: $(ENGINE)
	$(CC) $(CFLAGS) tests/test_genna.c $(ENGINE) -o test_genna && ./test_genna
	$(CC) $(CFLAGS) tests/edge.c       $(ENGINE) -o edge        && ./edge

# --- persistence: byte-exact round trip + crash recovery ---
persist: $(ENGINE)
	$(CC) $(CFLAGS) tests/persist_test.c $(ENGINE) -o persist_test
	$(CC) $(CFLAGS) tests/crash_test.c   $(ENGINE) -o crash_test
	./persist_test write sample_ml_dataset.jsonl /tmp/gn_store.gn /tmp/gn_ref.bin
	./persist_test verify /tmp/gn_store.gn /tmp/gn_ref.bin
	./crash_test parent /tmp/gn_crash.gn /tmp/gn_witness.txt sample_ml_dataset.jsonl 6 350

# --- fuzz: randomized splices, search, refcount GC, persistence ---
fuzz: $(ENGINE)
	$(CC) $(CFLAGS) tests/fuzz_test.c $(ENGINE) -o fuzz_test && ./fuzz_test

# --- portable CI build ------------------------------------------------
# Everything that needs no corpora and no MSYS2, so it runs unchanged on
# Linux, macOS and Windows runners. tools/*.sh are MSYS2-specific (they set
# absolute /d/... paths and mingw toolchains); this target is what CI uses to
# prove the engine builds and passes on a non-Windows machine.
#
# Compression is passed in rather than detected: CI installs zlib and zstd, so
# a missing library should fail loudly here instead of silently producing a
# store several times larger than the one the benchmarks describe.
CI_DEFS ?= -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD
CI_LIBS ?= -lz -lzstd
CI_ENGINE = $(ENGINE) src/genna_bin.c src/genna_merge.c
CI_CFLAGS = $(CFLAGS) $(CI_DEFS) -Wall -Wextra

.PHONY: ci
ci: ci-build ci-run

# Split so a failure names itself. As one recipe, any of 8 compiles and 9
# test runs failing printed only "Build and test failed", and GitHub
# returns 403 for workflow logs without a token -- so on macOS there was
# no public signal saying WHICH of seventeen commands broke.
ci-build:
	@echo "== $$($(CC) --version | head -1) =="
	$(CC) $(CI_CFLAGS) tests/test_genna.c   $(ENGINE)              -o test_genna   $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/edge.c         $(ENGINE)              -o edge         $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/fuzz_test.c    $(ENGINE)              -o fuzz_test    $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/persist_test.c $(ENGINE)              -o persist_test $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/crash_test.c   $(ENGINE)              -o crash_test   $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/merge_test.c   $(ENGINE) src/genna_merge.c -o merge_test $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) -DGN_NODE_AGG tests/agg_test.c $(ENGINE) src/genna_bin.c src/genna_agg.c -o agg_test $(CI_LIBS) $(LM)
	$(CC) $(CI_CFLAGS) tests/oocore_test.c  $(CI_ENGINE) src/genna_agg.c -DGN_NODE_AGG -o oocore_test $(CI_LIBS) $(LM)

ci-run:
	./test_genna
	./edge
	./fuzz_test
	./merge_test
	./agg_test 4
	./persist_test write sample_ml_dataset.jsonl gn_store.gn gn_ref.bin
	./persist_test verify gn_store.gn gn_ref.bin
	./crash_test parent gn_crash.gn gn_witness.txt sample_ml_dataset.jsonl 3 200
	./oocore_test . 8 keep
	@rm -f gn_store.gn* gn_ref.bin gn_crash.gn* gn_witness.txt oo_*.gn*
	@echo "== CI build+test OK on $$(uname -s 2>/dev/null || echo Windows) =="

clean:
	rm -f genna-curate embedtest simdtest comptest realbench gitcmp rsynccmp vbench \
	      test_genna edge persist_test crash_test fuzz_test merge_test agg_test \
	      oocore_test *.exe
