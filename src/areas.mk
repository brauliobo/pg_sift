# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query.h"): SIFT_INC has -I$(SRC). SIFT_DIRS is the list of
# the folders. They are not on the vpath: a file name is not unique across them (am.c, kern.c, ...), so a rule names the path of its source.
SIFT_DIRS    := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INCDIRS := $(SRC)
SIFT_INC     := $(addprefix -I,$(SIFT_INCDIRS))
