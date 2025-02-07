/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <inttypes.h>
#include <stdbool.h>

#include "sd-device.h"
#include "sd-id128.h"

int net_get_type_string(sd_device *device, uint16_t iftype, char **ret);
const char *net_get_persistent_name(sd_device *device);
int net_get_unique_predictable_data(sd_device *device, bool use_sysname, uint64_t *ret);
int net_get_unique_predictable_data_from_name(const char *name, const sd_id128_t *key, uint64_t *ret);
