# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its path from src/ (#include "bm25/query.h"): SIFT_INC has -I$(SRC). SIFT_DIRS is the list of
# the folders and is on the vpath, so that a pattern rule finds a source by its file name.
SIFT_DIRS    := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INCDIRS := $(SRC)
SIFT_INC     := $(addprefix -I,$(SIFT_INCDIRS))
vpath %.c $(SIFT_DIRS)
