#ifndef _VALUE_SET_H
#define _VALUE_SET_H

#include "main.h"

int set_value(char *file, char *numb);
void set_array_value(char **file, int num, char *value);
void set_temp(char *temp_sensor, char **temp_file, int temp_file_num, uchar tempwall);
int probe_unit_factor(const char *path, int *raw_value);
int set_current_array(char **file, int *unit, int num, int target_ua);
//读取真实电池温度（0.1℃制），优先thermal子系统的电池传感器，伪装生效后仍可用
int read_battery_real_temp(int *tenth_c);
//返回探测到的电池温度传感器节点路径，未找到返回NULL（仅供启动日志打印）
const char *battery_zone_path(void);
//返回该节点的 thermal type 名称，未找到返回NULL
const char *battery_zone_type(void);
//返回1表示只能用 type=battery 的镜像节点（伪装生效后它也会变成28℃，观测价值有限）
int battery_zone_is_mirror(void);

#endif
