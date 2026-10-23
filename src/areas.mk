# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query.h"): SIFT_INC has -I$(SRC). SIFT_DIRS is the list of
# the folders. They are not on the vpath: a file name is not unique across them (am.c, kern.c, ...), so a rule names the path of its source.
SIFT_DIRS    := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INCDIRS := $(SRC)
SIFT_INC     := $(addprefix -I,$(SIFT_INCDIRS))
# The code generation baseline of every build of src/: portable x86-64 (the vector kernels opt in per function with target attributes); the compiler's own
# default elsewhere, where -march=x86-64 is an error.
SIFT_BASE_ISA := $(if $(filter x86_64,$(shell uname -m)),-march=x86-64 -mtune=generic)
# The include directories of the server headers' own dependencies (libintl.h from Homebrew's gettext), for the builds outside PGXS that include postgres.h.
SIFT_PGCPP := $(shell pg_config --cppflags)
# The matching link flags: the library directories (lz4, zstd) and libintl, which the server's libpgport needs where gettext is separate (not the whole of --libs: -lnuma and
# -lxml2 are not needed, and --ldflags may carry -flto).
SIFT_PGLIBS := $(filter -L% -lintl,$(shell pg_config --ldflags --libs))
