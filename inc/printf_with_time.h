#ifndef _PRINTF_WITH_TIME_H
#define _PRINTF_WITH_TIME_H

#include <time.h>

#include "main.h"

/*
把当前时间转换为北京时间(UTC+8)并写入 *ptm
注意返回的 tm_year 已加 1900、tm_mon 已加 1（即 1~12），不能直接交给 strftime 用 %Y/%m ——
strftime 要的是"距 1900 的年数"和"0~11 的月"，所以调用方必须用 %04d-%02d-%02d 自己格式化
*/
void get_utc8_time(struct tm *ptm);

void printf_with_time(const char *format, ...);

#endif
