#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "printf_with_time.h"
#include "options_linkedlist.h"
#include "foreground_app.h"

//全局变量，用于存储当前前台应用的包名
volatile char ForegroundAppName[FOREGROUND_APP_STRING_MAX_SIZE]={0};
//子线程是否在运行的标志位，只有通过它才能保证同一时刻最多只有一个子线程
static volatile int fg_thread_alive=0;

//子线程被pthread_cancel取消时不会执行函数末尾的代码，需要靠清理函数复位标志位，
//否则标志位永远为1，此后再也不会重新创建子线程（“伪”旁路供电从此彻底失效）
static void fg_thread_cleanup(void *arg)
{
    (void)arg;
    fg_thread_alive=0;
}

/*
获取前台应用的包名以配合“伪”旁路供电功能使用
我是干嵌入式系统开发的，对AndroidNDK不熟悉，无法使用纯C语言实现，所以只能使用popen管道执行Shell命令来获取
但这就导致了执行效率的大幅下降，我无能为力，只能等待其他大佬的PR
*/
void *get_foreground_appname(void *android_version)
{
    pthread_cleanup_push(fg_thread_cleanup, NULL);
    while(read_one_option("BYPASS_CHARGE") == 1)
    {
        //在循环体内定义变量，这样变量仅存在于单次循环，每次循环结束后变量自动释放，循环开始时变量重新定义
        char result[2 * APP_PACKAGE_NAME_MAX_SIZE]={0},*tmpchar1,*tmpchar2;
        FILE *fp;
        //判断是否为锁屏状态，如果是则无法获取应用包名，直接将全局变量ForegroundAppName赋值为screen_is_off
        //"re"：关闭子进程继承的文件描述符，避免每次popen都泄漏一个fd
        fp=popen("dumpsys deviceidle | grep 'mScreenOn'", "re");
        if(fp == NULL)
        {
            printf_with_time("无法创建管道通信，跳过本次循环！");
            continue_no_print:
            sleep(5);
            //添加进程取消点，否则无法通过调用pthread_exit()函数取消进程
            //下面所有pthread_testcancel()函数的作用都同上
            pthread_testcancel();
            continue;
        }
        if(fgets(result, sizeof(result), fp) == NULL) result[0]='\0';
        pclose(fp);
        fp=NULL;
        line_feed(result);
        //此Shell命令的返回值格式为  mScreenOn=true
        //若为true则没有锁屏，若为false则处于锁屏状态
        tmpchar1=strstr(result, "=");
        if(tmpchar1 == NULL)
        {
            printf_with_time("无法获取锁屏状态，跳过本次循环！");
            goto continue_no_print;
        }
        if(strcmp(tmpchar1+1, "true"))
        {
            pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
            memset((void *)ForegroundAppName, 0, sizeof(ForegroundAppName));
            strncpy((char *)ForegroundAppName, "screen_is_off", APP_PACKAGE_NAME_MAX_SIZE);
            pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
            goto continue_no_print;
        }
        /*
        如果非锁屏状态，则获取前台应用包名
        安卓10及以上可使用dumpsys activity lru获取应用状态
        安卓10以下就只能用dumpsys activity o获取，执行效率慢
        然后再使用grep提取出包含关键字的行，此行即为前台应用的详情
        */
        fp=(*((int *)android_version) < 10)?popen("dumpsys activity o | grep ' (top-activity)'", "re"):popen("dumpsys activity lru | grep ' TOP'", "re");
        if(fp == NULL)
        {
            printf_with_time("无法创建管道通信，跳过本次循环！");
            goto continue_no_print;
        }
        if(fgets(result, sizeof(result), fp) == NULL) result[0]='\0';
        pclose(fp);
        fp=NULL;
        line_feed(result);
        /*
        从上一步获取到的前台应用的详情中截取应用包名
        dumpsys activity lru获取到的详情格式为  #98: fg     TOP  LCMN 4493:com.termux/u0a351 act:activities|recents
        dumpsys activity o获取到的详情格式为  Proc # 0: fg     T/A/TOP  LCMN  t: 0 4493:com.termux/u0a351 (top-activity)
        */
        tmpchar1=strstr(result, "TOP ");
        if(tmpchar1 == NULL)
        {
            can_not_get_package:
            printf_with_time("无法获取前台应用包名，跳过本次循环！");
            goto continue_no_print;
        }
        //旧版本直接对strstr的返回值+1，某些机型的输出缺少分隔符时会解引用空指针，导致整个程序崩溃
        if(*((int *)android_version) < 10)
        {
            tmpchar2=strstr(tmpchar1, ":");
            if(tmpchar2 == NULL) goto can_not_get_package;
            tmpchar2=strstr(tmpchar2+1, ":");
        }
        else tmpchar2=strstr(tmpchar1, ":");
        if(tmpchar2 == NULL) goto can_not_get_package;
        tmpchar1=strstr(tmpchar2, "/");
        if(tmpchar1 == NULL) goto can_not_get_package;
        *tmpchar1='\0';
        tmpchar1=tmpchar2+1;
        pthread_testcancel();
        //将前台应用包名赋值给ForegroundAppName
        pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
        memset((void *)ForegroundAppName, 0, sizeof(ForegroundAppName));
        strncpy((char *)ForegroundAppName, tmpchar1, APP_PACKAGE_NAME_MAX_SIZE);
        pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
        sleep(5);
        pthread_testcancel();
    }
    //如果配置文件的BYPASS_CHARGE值不为1，则退出循环并将ForegroundAppName清空
    pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
    memset((void *)ForegroundAppName, 0, sizeof(ForegroundAppName));
    pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
    pthread_cleanup_pop(1);
    return NULL;
}

//获取安卓版本，因为只有安卓7及以上才能使用Shell命令获取安卓版本
int check_android_version()
{
    char android_version_char[8]={0};
    int android_version=0;
    FILE *fp=NULL;
    fp=popen("getprop ro.build.version.release", "re");
    if(fp == NULL)
    {
        printf_with_time("无法创建管道通信，故无法获取安卓版本，“伪”旁路供电功能失效！");
        return 0;
    }
    if(fgets(android_version_char, sizeof(android_version_char), fp) == NULL) android_version_char[0]='\0';
    line_feed(android_version_char);
    pclose(fp);
    fp=NULL;
    if(!strlen(android_version_char))
    {
        printf_with_time("无法获取安卓版本，而只有安卓7及以上能够通过Shell命令获取前台应用，所以必须要获取安卓版本进行判断，“伪”旁路供电功能失效！");
        return 0;
    }
    android_version=atoi(android_version_char);
    if(android_version < 7)
    {
        printf_with_time("安卓7及以下无法通过Shell命令获取前台应用，当前版本为安卓%d，“伪”旁路供电功能失效！", android_version);
        return 0;
    }
    return android_version;
}

/*
保证同一时刻最多只有一个获取前台应用包名的子线程
返回1代表子线程正在运行（本次创建成功或本来就在跑），返回0代表创建失败
*/
int start_foreground_appname_thread(pthread_t *thread, int *android_version)
{
    if(thread == NULL) return 0;
    if(fg_thread_alive) return 1;
    fg_thread_alive=1;
    if(pthread_create(thread, NULL, get_foreground_appname, (void *)android_version) != 0)
    {
        fg_thread_alive=0;
        printf_with_time("无法创建获取前台应用包名的子线程，5秒后重试");
        return 0;
    }
    pthread_detach(*thread);
    return 1;
}

int foreground_thread_alive()
{
    return fg_thread_alive;
}
