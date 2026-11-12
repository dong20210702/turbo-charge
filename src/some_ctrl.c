#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "some_ctrl.h"
#include "options_linkedlist.h"
#include "value_set.h"
#include "my_malloc.h"
#include "foreground_app.h"
#include "printf_with_time.h"

//阶梯充电的控制，1为开启0为关闭
//不再在运行时调用check_read_file，节点暂时不存在时由set_value静默跳过，
//旧版本这里一旦读不到文件就会exit，导致拔插充电器后模块直接消失
void step_charge_ctl(char *value)
{
    set_value("/sys/class/power_supply/battery/step_charging_enabled", value);
    set_value("/sys/class/power_supply/battery/sw_jeita_enabled", value);
}

//控制能否进行充电，1为充电0为暂停充电
void charge_ctl(char *i)
{
    set_value("/sys/class/power_supply/battery/charging_enabled", i);
    set_value("/sys/class/power_supply/battery/battery_charging_enabled", i);
    if(atoi(i))
    {
        set_value("/sys/class/power_supply/battery/input_suspend", "0");
        set_value("/sys/class/qcom-battery/restricted_charging", "0");
    }
    else
    {
        set_value("/sys/class/power_supply/battery/input_suspend", "1");
        set_value("/sys/class/qcom-battery/restricted_charging", "1");
    }
}

//电量控制
void powel_ctl(int *last_charge_stop, int *charge_is_stop)
{
    int charge_stop=read_one_option("CHARGE_STOP"),power_int=0;
    char power[10]={0};
    if(*last_charge_stop == -1) *last_charge_stop=charge_stop;
    if(read_one_option("POWER_CTRL") == 1)
    {
        if(!read_file("/sys/class/power_supply/battery/capacity", power, sizeof(power))) return;
        power_int=atoi(power);
        if(*last_charge_stop != charge_stop && *charge_is_stop)
        {
            if(power_int < charge_stop)
            {
                printf_with_time("手机当前电量为%d%%，小于新的电量阈值，恢复充电", power_int);
                *charge_is_stop=0;
                charge_ctl("1");
            }
            else
            {
                printf_with_time("手机当前电量为%d%%，大于等于新的电量阈值，保持停止充电状态", power_int);
                charge_ctl("0");
            }
            *last_charge_stop=charge_stop;
        }
        if(power_int >= charge_stop && !(*charge_is_stop))
        {
            printf_with_time("当前电量为%d%%，大于等于停止充电的电量阈值，停止充电", power_int);
            *charge_is_stop=1;
            charge_ctl("0");
        }
        if(power_int <= read_one_option("CHARGE_START") && *charge_is_stop)
        {
            printf_with_time("当前电量为%d%%，小于等于恢复充电的电量阈值，恢复充电", power_int);
            *charge_is_stop=0;
            charge_ctl("1");
        }
    }
    else
    {
        if(*charge_is_stop)
        {
            printf_with_time("电量控制关闭，恢复充电");
            *charge_is_stop=0;
        }
        if(*last_charge_stop != charge_stop) *last_charge_stop=charge_stop;
        charge_ctl("1");
    }
}

/*
“伪”旁路充电控制
thread1为用来存储进程信息的pthread_t结构体
android_version为安卓版本
last_appname为上一次获取到的前台应用包名
is_bypass为上一次获取到的前台应用是否位于配置列表中，与主函数进行通信
screen_is_off为手机是否处于锁屏状态，与主函数进行通信
current_max_file为存储电流文件的二级指针变量，与current_max_file_num配套使用
current_max_file_num为电流文件的个数
*/
void bypass_charge_ctl(pthread_t *thread1, int *android_version, char *last_appname, int *is_bypass, int *screen_is_off, char **current_max_file, int current_max_file_num)
{
    char name[FOREGROUND_APP_STRING_MAX_SIZE]={0};
    uchar in_list=0;
    int i=0,bypass_enabled=read_one_option("BYPASS_CHARGE");
    FILE *fp=NULL;
    static char **bypass_app_package_name=NULL;
    static int bypass_app_num=0;
    //-1代表“还没读过”，0是合法的修改时间，不能用0做哨兵
    static time_t bypass_file_last_modify_time=-1;
    struct stat statbuf={0};
    /*
    为了不使获取前台应用包名拖累主程序的执行效率，所以使用了子线程方案
    如果配置文件的BYPASS_CHARGE值为1且子线程没有在运行，则创建子线程
    子线程是否在运行由foreground_app.c里的标志位统一维护，不再用占位字符串来判断，
    否则取消子线程与创建子线程的竞态会导致同时跑出多个子线程，反复fork shell进程拖垮系统
    */
    pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
    if(bypass_enabled == 1 && !foreground_thread_alive()) start_foreground_appname_thread(thread1, android_version);
    if(bypass_enabled == 1 && strlen((char *)ForegroundAppName))
    {
        //如果手机从锁屏状态恢复且在锁屏前处于“伪”旁路供电模式，则恢复此模式
        if(strcmp((char *)ForegroundAppName, "screen_is_off"))
        {
            if(*screen_is_off)
            {
                if(*is_bypass) printf_with_time("手机屏幕开启，恢复“伪”旁路供电模式");
                *screen_is_off=0;
            }
            //读取“伪”旁路供电的配置文件
            //旧版本这里误用了option_file的修改时间，且从不更新已记录的时间戳，
            //导致每一轮循环都要重新打开并解析一次配置文件，同时漏掉除第一条以外的所有字符串的内存释放
            if(check_read_file(bypass_charge_file) && !stat(bypass_charge_file, &statbuf) && statbuf.st_mtime != bypass_file_last_modify_time)
            {
                bypass_file_last_modify_time=statbuf.st_mtime;
                //必须先按旧的个数释放，再清零计数
                free_malloc_memory(&bypass_app_package_name, bypass_app_num);
                bypass_app_num=0;
                fp=fopen(bypass_charge_file, "rt");
                if(fp != NULL)
                {
                    while(fgets(name, sizeof(name), fp) != NULL)
                    {
                        line_feed(name);
                        //跳过以英文井号开头的行及空行
                        if((strlen(name) == 0) || (name[0] == '#')) continue;
                        bypass_app_num++;
                        bypass_app_package_name=(char **)my_realloc(bypass_app_package_name, sizeof(char *)*bypass_app_num);
                        bypass_app_package_name[bypass_app_num-1]=(char *)my_calloc(1, sizeof(char)*FOREGROUND_APP_STRING_MAX_SIZE);
                        strncpy(bypass_app_package_name[bypass_app_num-1], name, APP_PACKAGE_NAME_MAX_SIZE-1);
                    }
                    fclose(fp);
                    fp=NULL;
                }
            }
            //判断前台应用包名是否在配置文件中
            for(i=0;i < bypass_app_num && bypass_app_package_name != NULL;i++)
            {
                if(!strcmp((char *)ForegroundAppName, bypass_app_package_name[i]))
                {
                    in_list=1;
                    break;
                }
            }
            //如果前台应用包名在配置文件中
            if(in_list)
            {
                //如果之前不在“伪”旁路供电模式而切换应用后在了，就打印相关信息
                if(!(*is_bypass))
                {
                    printf_with_time("当前前台应用为%s，位于“伪”旁路供电配置列表中，进入“伪”旁路供电模式", ForegroundAppName);
                    *is_bypass=1;
                }
                //如果之前在“伪”旁路供电模式而切换应用后还在，就打印相关信息
                else if(strcmp(last_appname, (char *)ForegroundAppName)) printf_with_time("前台应用切换为%s，位于“伪”旁路供电配置列表中，保持“伪”旁路供电模式", ForegroundAppName);
                //限制电流，“伪”旁路供电
                set_array_value(current_max_file, current_max_file_num, BYPASS_CHARGE_CURRENT);
            }
            else
            {
                //如果之前在“伪”旁路供电模式而切换应用后不在，就打印相关信息
                if(*is_bypass)
                {
                    //分为两种情况，一种为未切换前台应用但应用包名从“伪”旁路供电配置列表中移除，一种为切换到了“伪”旁路供电配置列表中没有的应用
                    if(strcmp(last_appname, (char *)ForegroundAppName)) printf_with_time("前台应用切换为%s，不在“伪”旁路供电配置列表中，恢复正常充电模式", ForegroundAppName);
                    else printf_with_time("%s已从“伪”旁路供电配置列表中移除，恢复正常充电模式", ForegroundAppName);
                    *is_bypass=0;
                }
            }
            memcpy(last_appname, (char *)ForegroundAppName, sizeof(char)*FOREGROUND_APP_STRING_MAX_SIZE);
        }
        else
        {
            //如果手机进入锁屏状态且在锁屏前处于“伪”旁路供电模式，则暂时恢复正常充电模式
            if(!(*screen_is_off))
            {
                if(*is_bypass) printf_with_time("手机屏幕关闭，暂时进入正常充电模式");
                *screen_is_off=1;
            }
        }
    }
    //子线程结束运行或者获取不到前台应用包名，则清空上一次获取到的前台应用包名并恢复正常充电模式
    else
    {
        if(strlen(last_appname)) memset(last_appname, 0, sizeof(char)*FOREGROUND_APP_STRING_MAX_SIZE);
        if(*is_bypass) *is_bypass=0;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
}
