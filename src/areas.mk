# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query.h"): SIFT_INC has -I$(SRC) first. SIFT_DIRS is the list
# of the folders and is on the vpath, so that a pattern rule finds a source by its file name; the folders stay on the include path for the files
# that are not renamed yet (the include paths of src/agg and src/compat, which forwards the old names of renamed headers).
SIFT_DIRS := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INC  := -I$(SRC) $(addprefix -I,$(SIFT_DIRS))
vpath %.c $(SIFT_DIRS)
