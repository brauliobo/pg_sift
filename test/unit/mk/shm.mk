# The tests of the shared-memory write layer (core/shm/, core/progress.c, col/dsmreg.c, col/leafshm.c, col/dictshm/) that include the sources they cover: the server symbols they do not run link as stubs that trap (AUTOSTUB).
SHM_TESTS = core/shm/lease core/shm/combiner core/shm/manifest core/progress col/dsmreg col/leafshm col/dictshm

TESTS    += $(SHM_TESTS)
AUTOSTUB += $(SHM_TESTS)
$(foreach t,$(SHM_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -DSIFT_TESTING -Wno-unused-function))
# col/dsmreg.c names its library by the code id the build writes
XFLAGS_col/dsmreg += -I$(B)/gen
$(B)/col/dsmreg.o: $(B)/gen/core/code_id.h
$(B)/gen/core/code_id.h:
	@mkdir -p $(@D) && echo '#define SIFT_CODE_ID "unit"' >$@
