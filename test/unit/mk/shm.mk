# The tests of the shared-memory write layer (core/shm/, col/leafshm.c) that include the sources they cover: the server symbols they do not run link as stubs that trap (AUTOSTUB).
SHM_TESTS = core/shm/lease core/shm/combiner core/shm/manifest col/leafshm

TESTS    += $(SHM_TESTS)
AUTOSTUB += $(SHM_TESTS)
$(foreach t,$(SHM_TESTS),$(eval XFLAGS_$(t) = -D_GNU_SOURCE -DSIFT_TESTING -Wno-unused-function))
