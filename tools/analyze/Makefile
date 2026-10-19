# Standalone analyzer harness. `make` builds an optimized binary, `make
# SANITIZE=1` an ASan/UBSan one; `make fetch` downloads the Unicode 16.0 and
# Snowball reference data into $(DATA); `make check` runs the stemmer
# vocabulary, the exhaustive analyze(t) == [t] check, the invariant fuzzer
# and the differential reference test;
# `./harness bench FILE [MODE [STEM]]` measures throughput (one doc per line).
SRC = ../../src
include $(SRC)/areas.mk
PGINC = $(shell pg_config --includedir-server)
PGLIB = -L$(shell pg_config --libdir) -L$(shell pg_config --pkglibdir)
CFLAGS = -O2 -g -march=x86-64 -mtune=generic -Wall -Wno-unused-function $(SIFT_INC) -I$(PGINC)
ifdef SANITIZE
CFLAGS += -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
endif
LIBS = $(PGLIB) -lpgcommon -lpgport -lm
DATA ?= data
# LeakSanitizer is on: the stub frees its palloc arena at exit (../pgshim.c), so what it reports is a real malloc leak

harness: harness.c ../pgshim.c ../pgshim.h $(SRC)/bm25/sift_analyze.c $(SRC)/bm25/sift_analyze_stem.c $(SRC)/bm25/sift_analyze.h \
		$(SRC)/bm25/sift_analyze_tables.h
	$(CC) $(CFLAGS) -o $@ harness.c ../pgshim.c $(SRC)/bm25/sift_analyze.c $(SRC)/bm25/sift_analyze_stem.c $(LIBS)

check: harness
	./harness stem $(DATA)/en_voc.txt $(DATA)/en_output.txt
	./harness invariant $(DATA)/en_voc.txt
	./harness fuzz 200000 1
	python3 -I reference.py $(DATA)/ucd ./harness 3000

UCD = https://www.unicode.org/Public/16.0.0/ucd
SNOWBALL = https://raw.githubusercontent.com/snowballstem/snowball-data/master/english
fetch:
	mkdir -p $(DATA)/ucd/emoji
	curl -sSf -o $(DATA)/ucd/Scripts.txt $(UCD)/Scripts.txt
	curl -sSf -o $(DATA)/ucd/DerivedCoreProperties.txt $(UCD)/DerivedCoreProperties.txt
	curl -sSf -o $(DATA)/ucd/emoji/emoji-data.txt $(UCD)/emoji/emoji-data.txt
	curl -sSf -o $(DATA)/en_voc.txt $(SNOWBALL)/voc.txt
	curl -sSf -o $(DATA)/en_output.txt $(SNOWBALL)/output.txt

clean:
	rm -f harness

.PHONY: check fetch clean
