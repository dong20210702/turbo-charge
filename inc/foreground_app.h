#ifndef _FOREGROUND_APP_H
#define _FOREGROUND_APP_H

#include "main.h"

extern volatile char ForegroundAppName[FOREGROUND_APP_STRING_MAX_SIZE];

void *get_foreground_appname(void *android_version);
int check_android_version();
int start_foreground_appname_thread(pthread_t *thread, int *android_version);
int foreground_thread_alive();

#endif
