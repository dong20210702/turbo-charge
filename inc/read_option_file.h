#ifndef _READ_OPTION_FILE_H
#define _READ_OPTION_FILE_H

#include "main.h"

void *read_option_file(void *arg);

//首次解析完成标志，1表示配置文件已经读过一遍（主线程启动摘要要等它）
extern volatile int option_load_ready;

#endif
