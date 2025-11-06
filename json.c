/*
 * json.c
 *
 * Copyright (c) 2015 Aurelien Chartier <chartier.aurelien@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <json.h>
#include <bits.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "pci.h"

static inline char*
strdup_or_null(const char *default_value)
{
    return default_value ? strdup(default_value) : NULL;
}

static char*
json_parse_string_field(json_object *jvalue,
                        const char *field_name,
                        const char *field_default_value)
{
    json_object *jfield = NULL;
    json_bool jres = json_object_object_get_ex(jvalue, field_name, &jfield);

    if (NULL == jfield || is_error(jfield) || !jres)
        return strdup_or_null(field_default_value);

    if (!json_object_is_type(jfield, json_type_string))
        return strdup_or_null(field_default_value);

    return strdup(json_object_get_string(jfield));
}


static pci_device_field*
json_parse_config_space_field(json_object *jvalue,
                              pci_device_field *fields_list)
{
    char *reg = json_parse_string_field(jvalue, "register", NULL);
    char *size = json_parse_string_field(jvalue, "size", NULL);
    char *mask = json_parse_string_field(jvalue, "mask", "00000000");

    if (NULL == reg || NULL == size) // those fields are mandatory
        return NULL;

    if (strlen(reg) != 8)
    {
        fprintf(stderr, "register field %s has an invalid size (expecting 8)\n", reg);
        return NULL;
    }

    if (strlen(size) != 1 ||
        (size[0] != '1' && size[0] != '2' && size[0] != '4'))
    {
        fprintf(stderr, "size field %s is invalid (expecting 1, 2 or 4)\n", size);
        return NULL;
    }

    if (strlen(mask) != 8)
    {
        fprintf(stderr, "mask field %s has an invalid size (expecting 8)\n", mask);
        return NULL;
    }

    return pci_device_field_add(fields_list, reg, size, mask);
}


static pci_device_field*
json_parse_quirk_config_space_fields(json_object *jquirk)
{
    json_object *jarray = NULL;
    pci_device_field *result = NULL;

    json_bool jres = json_object_object_get_ex(jquirk, "config_space_fields", &jarray);

    if (!jres)
        return NULL;

    if (NULL == jarray)
        return NULL;

    if (!json_object_is_type(jarray, json_type_array))
        return NULL;

    int len = json_object_array_length(jarray);

    for (int i = 0; i < len; ++i)
    {
        json_object *jvalue = json_object_array_get_idx(jarray, i);
        enum json_type type = json_object_get_type(jvalue);

        // If this is not the expected type, skip it
        if (json_type_object != type)
            continue;

        result = json_parse_config_space_field(jvalue, result);
    }

    return result;
}

static pci_device_quirk*
json_parse_quirk(json_object *jquirk, pci_device_quirk *quirks_list)
{
    char *name = json_parse_string_field(jquirk, "name", "");
    char *vendor = json_parse_string_field(jquirk, "vendor", "ffff");
    char *device = json_parse_string_field(jquirk, "device", "ffff");
    char *subvendor = json_parse_string_field(jquirk, "subvendor", "ffff");
    char *subdevice = json_parse_string_field(jquirk, "subdevice", "ffff");

    if (strlen(vendor) != 4)
    {
        fprintf(stderr, "vendor field %s has an invalid size (expecting 4)", vendor);
        return NULL;
    }

    if (strlen(device) != 4)
    {
        fprintf(stderr, "device field %s has an invalid size (expecting 4)", device);
        return NULL;
    }

    if (strlen(subvendor) != 4)
    {
        fprintf(stderr, "subvendor field %s has an invalid size (expecting 4)", subvendor);
        return NULL;
    }

    if (strlen(subdevice) != 4)
    {
        fprintf(stderr, "subdevice field %s has an invalid size (expecting 4)", subdevice);
        return NULL;
    }

    printf("Quirk - vendor: %s subvendor: %s\n", vendor, subvendor);
    printf("Quirk - device %s subdevice: %s\n", device, subdevice);

    pci_device_field *config_space_fields = json_parse_quirk_config_space_fields(jquirk);

    // no space fields found, skipping that quirk
    if (NULL == config_space_fields)
        return quirks_list;

    return pci_device_quirk_add(quirks_list, name, device, vendor, subvendor,
                                subdevice, config_space_fields);
}

static pci_device_quirk*
json_parse_quirks_array(json_object *quirks_array)
{
    int len = json_object_array_length(quirks_array);
    pci_device_quirk *quirks = NULL;

    for (int i = 0; i < len; ++i)
    {
        json_object *jvalue = json_object_array_get_idx(quirks_array, i);
        enum json_type type = json_object_get_type(jvalue);

        // if quirk is not an object as expected, skip it
        if (type != json_type_object)
            continue;

        quirks = json_parse_quirk(jvalue, quirks);
    }

    return quirks;
}

pci_device_quirk*
parse_json_file(const char *json_file)
{
    struct stat st;
    int res = stat(json_file, &st);
    if (-1 == res)
    {
        perror(json_file);
        return NULL;
    }

    char *json_str = calloc(st.st_size + 1, sizeof (char));
    if (NULL == json_str)
    {
        fprintf(stderr, "calloc for json file failed\n");
        return NULL;
    }

    FILE *f = fopen(json_file, "r");
    if (NULL == f)
    {
        perror(json_file);
        return NULL;
    }

    if (fread(json_str, st.st_size, sizeof (char), f) <= 0)
    {
        fprintf(stderr, "fread for json file failed\n");
        return NULL;
    }

    fclose(f);

    json_object *jobj = json_tokener_parse(json_str);
    if (is_error(jobj))
    {
        fprintf(stderr, "%s is not a valid json file\n", json_file);
        return NULL;
    }

    enum json_type type = json_object_get_type(jobj);
    if (json_type_array != type)
    {
        fprintf(stderr, "%s root node should be an array\n", json_file);
        return NULL;
    }

    pci_device_quirk *result = json_parse_quirks_array(jobj);

    json_object_put(jobj);
    free(json_str);

    return result;
}
