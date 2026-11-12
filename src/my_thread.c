#include "my_thread.h"

//创建两个互斥锁，供多线程使用
//必须用PTHREAD_MUTEX_INITIALIZER初始化：仅靠BSS清零只是碰巧在glibc/musl上等价于默认属性，
//换C库或改变链接方式就会变成未定义行为（表现为偶发死锁或崩溃）
pthread_mutex_t mutex_foreground_app=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_options=PTHREAD_MUTEX_INITIALIZER;
