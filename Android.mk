#Enables the listed display HAL modules
#libs to be built for QCOM targets only

ifeq ($(BOARD_USES_LEGACY_QCOM),true)

display-hals := libgralloc libgenlock libcopybit
display-hals += libhwcomposer liboverlay libqdutils

ifneq ($(TARGET_PROVIDES_LIBLIGHT),true)
display-hals += liblight
endif

include $(call all-named-subdir-makefiles,$(display-hals))

endif
