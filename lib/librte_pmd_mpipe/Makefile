# FIXME: License
# referred librte_pmd_mpipe/Makefile

include $(RTE_SDK)/mk/rte.vars.mk

#
# library name
#
LIB = librte_pmd_mpipe.a

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)

#
# all source are stored in SRCS-y
#
SRCS-$(CONFIG_RTE_LIBRTE_MPIPE_PMD) += pmd_mpipe.c

# this lib depends upon:
DEPDIRS-$(CONFIG_RTE_LIBRTE_MPIPE_PMD) += lib/librte_eal lib/librte_ether
DEPDIRS-$(CONFIG_RTE_LIBRTE_MPIPE_PMD) += lib/librte_mempool lib/librte_mbuf
DEPDIRS-$(CONFIG_RTE_LIBRTE_MPIPE_PMD) += lib/librte_net lib/librte_malloc

include $(RTE_SDK)/mk/rte.lib.mk
