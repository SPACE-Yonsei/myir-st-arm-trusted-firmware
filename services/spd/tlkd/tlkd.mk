#
# Copyright (c) 2015, ARM Limited and Contributors. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

ifeq (${ERROR_DEPRECATED},0)
SPD_INCLUDES		:=	-Iinclude/bl32/payloads
endif

SPD_SOURCES		:=	services/spd/tlkd/tlkd_common.c		\
				services/spd/tlkd/tlkd_helpers.S	\
				services/spd/tlkd/tlkd_main.c		\
				services/spd/tlkd/tlkd_pm.c
