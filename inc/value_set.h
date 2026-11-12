#ifndef _VALUE_SET_H
#define _VALUE_SET_H

#include "main.h"

int set_value(char *file, char *numb);
void set_array_value(char **file, int num, char *value);
void set_temp(char *temp_sensor, char **temp_file, int temp_file_num, uchar tempwall);
int probe_unit_factor(const char *path, int *raw_value);
int set_current_array(char **file, int *unit, int num, int target_ua);

#endif
