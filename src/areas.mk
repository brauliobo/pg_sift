# The area folders of src/. Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its bare name (the file names are unique across the folders): SIFT_INC puts every folder on the
# include path, SIFT_DIRS is the list of them and is on the vpath, so that a pattern rule finds a source by its name.
SIFT_DIRS := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INC  := $(addprefix -I,$(SIFT_DIRS))
vpath %.c $(SIFT_DIRS)
