#ifndef _CHARGE_LOG_H
#define _CHARGE_LOG_H

/*
充电采样日志（charge_log.csv）

存在的原因：
模块把电池温度伪造为28℃、把充电电流上限写到50A之后，系统原本的温控和限流信息
全部被盖掉了，出问题（功率上不去、掉到10W、100%还在灌电）时只能靠猜。
本模块把「我们写了什么」和「电池/充电器实际在做什么」同时记进CSV，
导出来看曲线就能定位是哪一个环节把功率掐掉的。

采样间隔由本模块内部控制（充电中5秒一行、未充电30秒一行），
调用方每次都调用即可，不需要自己算时间。
*/

//初始化（目前只做一次清理，fopen是惰性的，文件在第一次采样时才创建）
void charge_log_init(void);

/*
写一行采样
capacity          当前电量（%）
status            充电状态字符串（Charging/Full/Discharging/...）
applied_current   本模块当前实际生效的电流上限（单位μA），用于和内核实际电流对照
stage             当前策略档位字符串（如"0-80"、"80-90"、"FULL"），用于对照降流时机
temp_sensor       温度传感器节点路径（thermal_zoneX/temp），可为NULL
*/
void charge_log_sample(int capacity, const char *status, int applied_current, const char *stage, const char *temp_sensor);

//关闭文件（进程退出时调用，非必需，内核会自行刷新）
void charge_log_close(void);

#endif
