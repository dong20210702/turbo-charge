#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "main.h"
#include "read_option_file.h"
#include "options_linkedlist.h"
#include "some_ctrl.h"
#include "my_malloc.h"
#include "printf_with_time.h"
#include "value_set.h"
#include "foreground_app.h"
#include "charge_log.h"
#include "probe.h"

//重新向内核节点施加各项“解锁”参数的周期（单位:s）
//系统（温控服务、充电HAL、PD重新协商）会在运行过程中把某些节点改回默认值，
//只在启动时写一次的话，拔插充电器或协议重协商后就可能失去快充，所以需要周期性复查
#define REAPPLY_INTERVAL 30
//电流类配置的下限保护（单位:μA），配置为0或过小时内核会直接停止充电
//100000μA=100mA，低于这个值时多数机型的充电IC会直接关断，表现为“配置了但快充不生效”
#define CURRENT_MIN_LIMIT 100000

/*
列出path目录下的所有文件夹并赋值给二级指针变量ppp
第二个参数是二级指针的地址
返回值是文件夹的个数
容量按需翻倍，避免目录条目过多时越界写坏堆
*/
int list_dir(char *path, char ***ppp)
{
    DIR *pDir;
    struct dirent *ent;
    int file_num=0,cap=64;
    pDir=opendir(path);
    if(pDir == NULL) return 0;
    *ppp=(char **)my_realloc(*ppp, sizeof(char *)*cap);
    while((ent=readdir(pDir)) != NULL)
    {
        if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if(file_num >= cap)
        {
            cap*=2;
            *ppp=(char **)my_realloc(*ppp, sizeof(char *)*cap);
        }
        //分配一个与路径字符串大小相等的内存，用来装路径
        (*ppp)[file_num]=(char *)my_calloc(1, sizeof(char)*(strlen(path)+strlen(ent->d_name)+2));
        //拼接路径及文件夹名
        if((size_t)sprintf((*ppp)[file_num], "%s/%s", path, ent->d_name) >= sizeof(char)*(strlen(path)+strlen(ent->d_name)+2))
        {
            my_free((*ppp)[file_num]);
            (*ppp)[file_num]=NULL;
            continue;
        }
        file_num++;
    }
    closedir(pDir);
    //收缩内存
    if(file_num) *ppp=(char **)my_realloc(*ppp, sizeof(char *)*file_num);
    else
    {
        my_free(*ppp);
        *ppp=NULL;
    }
    return file_num;
}

//将字符串中的回车符、换行符换为字符串结束符
void line_feed(char *line)
{
    char *p;
    if(line == NULL) return;
    p=strchr(line, '\r');
    if(p != NULL) *p='\0';
    p=strchr(line, '\n');
    if(p != NULL) *p='\0';
}

/*
检查文件是否可读，可读返回1，不可读返回0
若文件暂时不存在，延时0.25秒后重新检查，兼容驱动重新注册节点、编辑器原子保存等瞬时状态
注意：本函数不再结束进程。运行期间某个节点短暂消失是正常现象（例如拔插充电器、温控服务重启），
旧版本在这里直接exit，会导致模块在后台静默退出，从而出现“用着用着快充就没了”的现象
*/
int check_read_file(char *file)
{
    struct timespec req={0, 250000000L};
    if(file == NULL) return 0;
    if(access(file, F_OK))
    {
        nanosleep(&req, NULL);
        if(access(file, F_OK)) return 0;
    }
    if(!access(file, R_OK)) return 1;
    //权限0644不得简写成644
    chmod(file, 0644);
    return !access(file, R_OK);
}

//读取文件内容，成功返回1，失败返回0（失败时char_var保持原内容）
int read_file(char *file_path, char *char_var, int max_char_num)
{
    FILE *fp;
    if(!check_read_file(file_path)) return 0;
    fp=fopen(file_path, "rt");
    if(fp == NULL) return 0;
    if(fgets(char_var, max_char_num, fp) == NULL)
    {
        fclose(fp);
        fp=NULL;
        return 0;
    }
    fclose(fp);
    fp=NULL;
    line_feed(char_var);
    return 1;
}

//同一类错误信息最多每60秒输出一次，避免节点持续不可用时把日志刷爆
static int should_log()
{
    static time_t last_log_time=0;
    time_t now=time(NULL);
    if(now - last_log_time < 60) return 0;
    last_log_time=now;
    return 1;
}

/*
保证同一时刻只有一个实例在运行
重装模块不重启手机、守护脚本重启程序等场景都会让旧实例还没退出就启动了新实例，
两个实例同时抢写同一批内核节点，表现就是电流忽高忽低、断充，甚至互相把对方的设置改回去
锁文件句柄故意不关闭，进程退出时由内核自动释放
返回1代表可以继续运行，0代表已有实例在运行
*/
static int ensure_single_instance()
{
    static int lock_fd=-1;
    lock_fd=open("/data/adb/turbo-charge/lock", O_RDWR|O_CREAT, 0644);
    //锁文件打不开时不阻塞程序运行，保证模块仍然可用
    if(lock_fd < 0) return 1;
    if(flock(lock_fd, LOCK_EX|LOCK_NB))
    {
        close(lock_fd);
        lock_fd=-1;
        return 0;
    }
    return 1;
}

//读取温度传感器的温度值，成功返回1，读数无效返回0
//0、1、-1是内核在节点不可用时返回的占位值，必须按无效处理，否则会被当成“温度已经降到0度”而误恢复大电流
/*
读取温度传感器的原始值，统一换算成 0.001℃ 返回
thermal_zone 下的 temp 单位是 0.001℃（45000 = 45.0℃），
power_supply 下的 temp 单位是 0.1℃（450 = 45.0℃），两者差 10 倍，
旧版本一律按 0.001℃ 处理，一旦温度源换到 power_supply 就会把 45℃ 读成 4.5℃，
温度保护等于直接失效。这里按量级判定单位：
  raw < 2000  → 0.1℃ 制（最大对应 200℃，真实机型不会超过），乘 10 后返回
  raw >= 2000 → 已是 0.001℃ 制，原样返回
*/
static int read_temp(char *temp_sensor, int *temp_int)
{
    char thermal[15]={0};
    int value=0;
    if(temp_sensor == NULL) return 0;
    if(!read_file(temp_sensor, thermal, sizeof(thermal))) return 0;
    value=atoi(thermal);
    if(value <= 1 || value > 200000) return 0;
    if(value < 2000) value*=10;
    *temp_int=value;
    return 1;
}

/*
查找可用温度传感器，找到则把温度节点的完整路径写入*temp_sensor并返回1，找不到返回0
注意：温度节点的路径是要交回给主函数长期使用的，所以必须用二级指针输出，
旧版本用一级指针传参，函数内部无论怎么赋值都不会影响主函数里的变量，
主函数拿到的永远是NULL，一旦触发温度控制就会因为“找不到文件”而退出进程
*/
int find_temp_sensor(char **temp_sensor)
{
    FILE *fq;
    struct stat statbuf={0};
    char *temp_sensor_dir = NULL, *buffer = NULL, *temp_tmp = NULL, *msg = NULL;
    char **thermal_dir = NULL;
    int thermal_file_num = 0, i = 0, j = 0, temp_sensor_num = 100;

    if(temp_sensor == NULL) return 0;
    *temp_sensor=NULL;
    temp_sensor_dir=(char *)my_calloc(1, sizeof(char));
    buffer=(char *)my_calloc(1, sizeof(char));
    temp_tmp=(char *)my_calloc(1, sizeof(char)*15);
    msg=(char *)my_calloc(1, sizeof(char));
    thermal_dir=(char **)my_calloc(1, sizeof(char *));
    thermal_file_num=list_dir("/sys/class/thermal", &thermal_dir);
    //遍历/sys/class/thermal
    for(i=0;i < thermal_file_num;i++)
    {
        //判断/sys/class/thermal下各个文件夹的名是否包含thermal_zone
        //因为温度传感器的相关文件就在此类文件夹中
        if(strstr(thermal_dir[i], "thermal_zone") != NULL)
        {
            //判断该文件夹内是否有一个名为type的文件，此文件的内容为该温度传感器的名称
            buffer=(char *)my_realloc(buffer, sizeof(char)*(strlen(thermal_dir[i])+6));
            sprintf(buffer, "%s/type", thermal_dir[i]);
            //如果没有type文件或type文件不可读，则跳过此温度传感器的后续操作
            if(access(buffer, R_OK)) continue;
            //获取type文件内的字符个数
            if(stat(buffer, &statbuf)) continue;
            fq=fopen(buffer, "rt");
            if(fq == NULL) continue;
            //重新分配内存，使其刚好能够装入该温度传感器的名称，并获取该温度传感器的名称
            msg=(char *)my_realloc(msg, sizeof(char)*(statbuf.st_size+1));
            if(fgets(msg, statbuf.st_size+1, fq) == NULL) msg[0]='\0';
            fclose(fq);
            fq=NULL;
            line_feed(msg);
            //检查此文件夹下有无temp文件，此文件的内容是该温度传感器所获取的温度值
            sprintf(buffer, "%s/temp", thermal_dir[i]);
            //如果没有temp文件或temp文件不可读，则跳过此温度传感器的后续操作
            if(access(buffer, R_OK)) continue;
            fq=fopen(buffer, "rt");
            if(fq != NULL)
            {
                if(fgets(temp_tmp, 10, fq) == NULL) temp_tmp[0]='\0';
                fclose(fq);
                fq=NULL;
            }
            //如果无法通过管道打开temp文件，则跳过此温度传感器的后续操作
            else continue;
            line_feed(temp_tmp);
            //判断temp文件的值是否正常，若不正常则代表不能使用此温度传感器，跳过此温度传感器的后续操作
            if(atoi(temp_tmp) == 1 || atoi(temp_tmp) == 0 || atoi(temp_tmp) == -1) continue;
            //根据温度传感器的优先级顺序进行筛选，最终选择优先级最高的可用的温度传感器
            for(j=0;j < temp_sensor_quantity;j++)
            {
                if(!strcmp(msg, temp_sensors[j]) && temp_sensor_num > j)
                {
                    temp_sensor_num=j;
                    //重新分配内存保证了最小内存使用量，temp_sensor_dir只是温度传感器的所在路径，后续会进行拼接
                    temp_sensor_dir=(char *)my_realloc(temp_sensor_dir, sizeof(char)*(strlen(thermal_dir[i])+1));
                    strcpy(temp_sensor_dir, thermal_dir[i]);
                }
            }
        }
    }
    my_free(buffer);
    buffer=NULL;
    my_free(temp_tmp);
    temp_tmp=NULL;
    my_free(msg);
    msg=NULL;
    free_malloc_memory(&thermal_dir, thermal_file_num);
    //判断是否获取到了可用的温度传感器
    if(temp_sensor_num != 100 && strlen(temp_sensor_dir))
    {
        //重新分配内存，使其刚好能装下路径+“/temp”，因为temp文件内存储的是温度传感器所获取的温度值
        *temp_sensor=(char *)my_calloc(1, sizeof(char)*(strlen(temp_sensor_dir)+6));
        sprintf(*temp_sensor, "%s/temp", temp_sensor_dir);
        printf_with_time("将使用%s温度传感器作为手机温度的获取源。由于每个传感器所处位置不同以及每个手机发热区不同，很可能导致获取到的温度与实际体感温度不同", temp_sensors[temp_sensor_num]);
        my_free(temp_sensor_dir);
        temp_sensor_dir=NULL;
        if(!check_read_file(*temp_sensor))
        {
            printf_with_time("无法读取%s，温度控制及充电时强制显示28℃功能失效！", *temp_sensor);
            my_free(*temp_sensor);
            *temp_sensor=NULL;
            return 0;
        }
        return 1;
    }
    //如果没有获取到可用的温度传感器，则返回0
    my_free(temp_sensor_dir);
    temp_sensor_dir=NULL;
    return 0;
}

//把匹配到的文件路径追加到动态数组里，容量不足时翻倍
static void add_path(char ***file, int *num, int *cap, char *path, int path_len)
{
    if(*num >= *cap)
    {
        *cap*=2;
        *file=(char **)my_realloc(*file, sizeof(char *)*(*cap));
    }
    (*file)[*num]=(char *)my_calloc(1, sizeof(char)*(path_len+1));
    strcpy((*file)[*num], path);
    (*num)++;
}

/*
判断路径是否以指定后缀结尾
原代码用 path+path_len-固定数字 的方式做后缀比较，长度全靠手数，而
/constant_charge_current_max 是28字符却写成27、/fast_charge_current 是20字符却写成19，
差一个字符导致 strcmp 首字符是 'c' 对 '/'，这两个最常用的电流节点从来没被识别成功过
（只有 thermal_input_current 的长度是对的），所以大量机型表现为“有关电流的所有功能失效”
*/
static int has_suffix(const char *path, const char *suffix)
{
    size_t path_len=strlen(path), suffix_len=strlen(suffix);
    if(path_len < suffix_len) return 0;
    return !strcmp(path+path_len-suffix_len, suffix);
}

/*
判断各个所需文件是否存在
检查电池状态、电量、充电控制、阶梯充电、温度等相关文件
并在/sys/class/power_supply中搜索电流文件和温度文件
根据文件的存在情况设置各功能的启用标志
*/
void check_required_files(uchar *battery_status, uchar *battery_capacity, uchar *power_control,
                          uchar *step_charge, uchar *force_temp, uchar *current_change,
                          int *temp_sensor_num, char **temp_sensor,
                          char ***current_max_file, int *current_max_file_num, int **current_max_unit,
                          char ***current_limit_file, int *current_limit_file_num,
                          char ***temp_file, int *temp_file_num)
{
    char **power_supply_dir_list = NULL, **power_supply_dir = NULL;
    int i = 0, j = 0, power_supply_file_num = 0, power_supply_dir_list_num = 0, path_len = 0;
    int current_max_cap = 16, current_limit_cap = 16, temp_cap = 16;

    if(access("/sys/class/power_supply/battery/status", F_OK)) *battery_status=0;
    if(access("/sys/class/power_supply/battery/capacity", F_OK)) *battery_capacity=0;
    if(!*battery_status || !*battery_capacity)
    {
        *power_control=0;
        if(*battery_status && !*battery_capacity) printf_with_time("由于找不到/sys/class/power_supply/battery/capacity文件，电量控制功能失效！");
        else if(!*battery_status && *battery_capacity) printf_with_time("由于找不到/sys/class/power_supply/battery/status文件，电量控制功能失效，且“伪”旁路供电功能无法根据手机的充电状态而自动启停！");
        else printf_with_time("由于找不到/sys/class/power_supply/battery/status和/sys/class/power_supply/battery/capacity文件，电量控制功能失效，且“伪”旁路供电功能无法根据手机的充电状态而自动启停！");
    }
    else if(access("/sys/class/power_supply/battery/charging_enabled", F_OK) && access("/sys/class/power_supply/battery/battery_charging_enabled", F_OK) && access("/sys/class/power_supply/battery/input_suspend", F_OK) && access("/sys/class/qcom-battery/restricted_charging", F_OK))
    {
        *power_control=0;
        printf_with_time("由于找不到控制手机暂停充电的文件，电量控制功能失效！");
        printf_with_time("目前已知的有关文件有：/sys/class/power_supply/battery/charging_enabled、/sys/class/power_supply/battery/battery_charging_enabled、/sys/class/power_supply/battery/input_suspend、/sys/class/qcom-battery/restricted_charging");
        printf_with_time("如果您知道其他的有关文件，请联系模块制作者！");
    }
    if(access("/sys/class/power_supply/battery/step_charging_enabled", F_OK))
    {
        *step_charge=0;
        printf_with_time("由于找不到/sys/class/power_supply/battery/step_charging_enabled文件，阶梯式充电控制的所有功能失效！");
    }
    else if(!*battery_capacity)
    {
        *step_charge=2;
        printf_with_time("由于找不到/sys/class/power_supply/battery/capacity文件，阶梯式充电无法根据电量进行开关，此时若在配置中关闭阶梯式充电，则无论电量多少，阶梯式充电都会关闭！");
    }
    //通过字符串后缀匹配来查找相应文件，容量不足时自动扩容，不再写死100个的上限
    power_supply_dir=(char **)my_calloc(1, sizeof(char *));
    power_supply_file_num=list_dir("/sys/class/power_supply", &power_supply_dir);
    *current_limit_file=(char **)my_calloc(current_limit_cap, sizeof(char *));
    *current_max_file=(char **)my_calloc(current_max_cap, sizeof(char *));
    *temp_file=(char **)my_calloc(temp_cap, sizeof(char *));
    //遍历/sys/class/power_supply
    for(i=0;i < power_supply_file_num;i++)
    {
        power_supply_dir_list=(char **)my_calloc(1, sizeof(char *));
        power_supply_dir_list_num=list_dir(power_supply_dir[i], &power_supply_dir_list);
        //遍历/sys/class/power_supply路径下的所有文件夹
        for(j=0;j < power_supply_dir_list_num;j++)
        {
            //如果匹配到了文件，则分配一个与路径字符串大小相等的内存，装入路径，并且记录相应文件数量的变量值+1
            path_len=strlen(power_supply_dir_list[j]);
            /*
            input_current_limit 也一并纳入：MTK机型的输入限流默认只有3A，
            如果充电器协商到9V档，3A×9V=27W，正好把功率卡在27W上不去，
            这才是“峰值30W左右就上不去”的直接原因之一
            （thermal_input_current_limit 以“_input_current_limit”结尾，前一位是下划线不是斜杠，不会误入本分支）
            */
            if(has_suffix(power_supply_dir_list[j], "/constant_charge_current_max")
                || has_suffix(power_supply_dir_list[j], "/constant_charge_current")
                || has_suffix(power_supply_dir_list[j], "/fast_charge_current")
                || has_suffix(power_supply_dir_list[j], "/input_current_limit")
                || has_suffix(power_supply_dir_list[j], "/thermal_input_current"))
            {
                /*
                先探测节点是否真的可用，不可用的直接不收进列表
                MTK的主从充电架构里，未启用的从充电器节点（mtk-slave-charger、mtk-mst-div-chg等）
                读出来是-95这类错误码，往这种节点写电流不会有任何效果，
                如果把它们收进列表，还会让“找到了电流文件”的日志骗人，误以为功能已经生效
                */
                if(probe_unit_factor(power_supply_dir_list[j], NULL) <= 0)
                {
                    if(should_log()) printf_with_time("忽略不可用的电流文件：%s", power_supply_dir_list[j]);
                    continue;
                }
                add_path(current_max_file, current_max_file_num, &current_max_cap, power_supply_dir_list[j], path_len);
            }
            else if(has_suffix(power_supply_dir_list[j], "/thermal_input_current_limit"))
            {
                add_path(current_limit_file, current_limit_file_num, &current_limit_cap, power_supply_dir_list[j], path_len);
            }
            else if(has_suffix(power_supply_dir_list[j], "/temp"))
            {
                add_path(temp_file, temp_file_num, &temp_cap, power_supply_dir_list[j], path_len);
            }
        }
        free_malloc_memory(&power_supply_dir_list, power_supply_dir_list_num);
    }
    free_malloc_memory(&power_supply_dir, power_supply_file_num);
    //收缩内存
    if(*current_limit_file_num) *current_limit_file=(char **)my_realloc(*current_limit_file, sizeof(char *)*(*current_limit_file_num));
    else { my_free(*current_limit_file); *current_limit_file=NULL; }
    if(*current_max_file_num) *current_max_file=(char **)my_realloc(*current_max_file, sizeof(char *)*(*current_max_file_num));
    else { my_free(*current_max_file); *current_max_file=NULL; }
    //记录每个电流节点的数值单位（1个数值等于多少μA），MTK的充电IC节点用mA，其余用μA，差1000倍
    *current_max_unit=NULL;
    if(*current_max_file_num)
    {
        *current_max_unit=(int *)my_calloc(*current_max_file_num, sizeof(int));
        for(i=0;i < *current_max_file_num;i++)
        {
            (*current_max_unit)[i]=probe_unit_factor((*current_max_file)[i], NULL);
            if((*current_max_unit)[i] <= 0) (*current_max_unit)[i]=1;
        }
    }
    if(*temp_file_num) *temp_file=(char **)my_realloc(*temp_file, sizeof(char *)*(*temp_file_num));
    else { my_free(*temp_file); *temp_file=NULL; }
    //判断文件数量是否为0
    if(!*current_max_file_num)
    {
        *current_change=0;
        printf_with_time("无法在/sys/class/power_supply中的所有文件夹内找到constant_charge_current_max、fast_charge_current、thermal_input_current文件，有关电流的所有功能（包括“伪”旁路供电功能）失效！");
    }
    if(!*battery_status || !*temp_file_num)
    {
        *force_temp=0;
        if(*battery_status && !*temp_file_num) printf_with_time("无法在/sys/class/power_supply中的所有文件夹内找到temp文件，充电时强制显示28℃功能失效！");
        else if(!*battery_status && *temp_file_num) printf_with_time("由于找不到/sys/class/power_supply/battery/status文件，充电时强制显示28℃功能失效！");
        else printf_with_time("由于找不到/sys/class/power_supply/battery/status文件以及无法在/sys/class/power_supply中的所有文件夹内找到temp文件，充电时强制显示28℃功能失效！");
    }
    if(*force_temp || *current_change)
    {
        *temp_sensor_num = find_temp_sensor(temp_sensor);
        if(*temp_sensor_num == 0)
        {
            if(*force_temp)
            {
                printf_with_time("由于找不到程序支持的温度传感器，温度控制及充电时强制显示28℃功能失效！");
                *force_temp=0;
            }
            else printf_with_time("由于找不到程序支持的温度传感器，温度控制功能失效！");
            if(!*step_charge && !*power_control && !*force_temp && !*current_change)
            {
                printf_with_time("所有的所需文件均不存在，完全不适配此手机，程序强制退出！");
                exit(800);
            }
        }
    }
    else if(!*step_charge && !*power_control && !*force_temp && !*current_change)
    {
        printf_with_time("所有的所需文件均不存在，完全不适配此手机，程序强制退出！");
        exit(1000);
    }
    if(*current_change) for(i=0;i < *current_max_file_num;i++)
        printf_with_time("找到电流文件：%s（单位%s）", (*current_max_file)[i], (*current_max_unit)[i] == 1000?"mA":"μA");
    if(*force_temp) for(i=0;i < *temp_file_num;i++) printf_with_time("找到温度文件：%s", (*temp_file)[i]);
}

/*
读取电流相关配置并做下限保护
电流配置成0或极小的值会让内核直接停止充电，表现就是“快充不生效”，
配置文件的注释里只说了“必须大于等于0”，这个坑必须由程序兜住
*/
static void load_current_config(char *current_max_char, int current_max_size, char *highest_temp_current_char, int highest_temp_current_size)
{
    snprintf(current_max_char, current_max_size, "%d", read_one_option("CURRENT_MAX"));
    snprintf(highest_temp_current_char, highest_temp_current_size, "%d", read_one_option("HIGHEST_TEMP_CURRENT"));
    if(atoi(current_max_char) < CURRENT_MIN_LIMIT)
    {
        snprintf(current_max_char, current_max_size, "%d", CURRENT_MIN_LIMIT);
        if(should_log()) printf_with_time("CURRENT_MAX的配置值过小，已按%dμA处理", CURRENT_MIN_LIMIT);
    }
    if(atoi(highest_temp_current_char) < CURRENT_MIN_LIMIT)
    {
        snprintf(highest_temp_current_char, highest_temp_current_size, "%d", CURRENT_MIN_LIMIT);
        if(should_log()) printf_with_time("HIGHEST_TEMP_CURRENT的配置值过小，已按%dμA处理", CURRENT_MIN_LIMIT);
    }
}

/*
向“只在启动时写一次”的那些内核节点重新施加参数
返回真正发生写入的节点个数（内容本来就一致时不会重复写，所以本函数可以随时调用而不会产生副作用）
*/
static int apply_kernel_values()
{
    int written=0;
    written += (set_value("/sys/kernel/fast_charge/force_fast_charge", "1") > 0);
    written += (set_value("/sys/class/power_supply/battery/system_temp_level", "1") > 0);
    written += (set_value("/sys/class/power_supply/usb/boost_current", "1") > 0);
    written += (set_value("/sys/class/power_supply/battery/safety_timer_enabled", "0") > 0);
    written += (set_value("/sys/kernel/fast_charge/failsafe", "1") > 0);
    written += (set_value("/sys/class/power_supply/battery/allow_hvdcp3", "1") > 0);
    written += (set_value("/sys/class/power_supply/usb/pd_allowed", "1") > 0);
    written += (set_value("/sys/class/power_supply/battery/input_current_limited", "0") > 0);
    written += (set_value("/sys/class/power_supply/battery/input_current_settled", "1") > 0);
    written += (set_value("/sys/class/qcom-battery/restrict_chg", "0") > 0);
    return written;
}

/*
按电量计算本轮允许写入的电流上限
SMART_CHARGE=0时直接返回CURRENT_MAX，保持旧版“能多快就多快”的行为
SMART_CHARGE=1时以PEAK_CURRENT为满速基准，电量越高允许的电流越小：
  低于STAGE_START          100%
  STAGE_START~90%           70%
  90%~95%                   45%
  95%以上                   25%
分段的意义不是把峰值做大（峰值由充电IC和电池电压决定，sysfs改不了），
而是让系统自身的阶梯充电不再提前掐掉电流，把降速点挪到高电量段
*/
static int stage_current_limit(int capacity, int current_max, char *stage, int stage_size)
{
    int stage_start=0,peak=0,percent=100;
    if(read_one_option("SMART_CHARGE") != 1)
    {
        snprintf(stage, stage_size, "OFF");
        return current_max;
    }
    peak=read_one_option("PEAK_CURRENT");
    //基准电流配得比下限还小时，退回CURRENT_MAX，避免把充电电流限制到停止充电
    if(peak < CURRENT_MIN_LIMIT) peak=current_max;
    //基准电流不能超过用户配置的CURRENT_MAX，否则用户把CURRENT_MAX调小会被本功能架空
    if(peak > current_max) peak=current_max;
    stage_start=read_one_option("STAGE_START");
    if(stage_start < 50) stage_start=50;
    if(stage_start > 99) stage_start=99;
    if(capacity < stage_start)
    {
        percent=100;
        snprintf(stage, stage_size, "0-%d", stage_start);
    }
    else if(capacity < 90)
    {
        percent=70;
        snprintf(stage, stage_size, "%d-90", stage_start);
    }
    else if(capacity < 95)
    {
        percent=45;
        snprintf(stage, stage_size, "90-95");
    }
    else
    {
        percent=25;
        snprintf(stage, stage_size, "95-100");
    }
    return (int)((long long)peak*percent/100);
}

/*
==================== 高温软启动 ====================
插线瞬间电池已经偏热时，先降流把温度拉下来，再交回熔断/分段策略

来历：v78实测插线时电池49.5℃、插上瞬间52.2℃，直接进熔断锁在6A，
而6A在这台机器上仍会把温度推到59.1℃。当时的设想是先4A压几分钟把起点拉低

v79 实测否定了这个设想，因此默认关闭（SOFT_START_TEMP=0），代码保留备用：
  插线瞬间电池电流本身从1.2A自然爬升，4A限流在前20秒完全不起作用；
  等电流真的碰到4A时温度已经52.7℃，限流真正生效只有65秒、
  温度降到48.2℃就退出，随后6A用16分钟又顶回57℃。
  净效果只是延后90秒，还白扔一段4A低功率（15.6W vs 25W），少充约0.5个点。
  想重新启用把 SOFT_START_TEMP 改到 45~50 即可

为什么退出不用温度滞回：软启动退出后会立刻回到6A，若退出阈值定在48℃，
温度很快被6A推回52~56℃并再次满足软启动条件，形成"4A→6A→4A"的来回切换，
平均功率反而更低。所以退出只有两条路——降到目标温度，或超时。
超时兜底必须有：亮屏重负载时4A也可能降不到48℃，没有兜底会一直卡在4A
*/
static int soft_start_active=0;
static time_t soft_start_since=0;

//充电器连接边沿调用：按插线瞬间的电池温度决定是否进入软启动
static void soft_start_begin(void)
{
    int soft_on=0,soft_cur=0,start_tenth=0;
    soft_start_active=0;
    soft_on=read_one_option("SOFT_START_TEMP");
    soft_cur=read_one_option("SOFT_START_CURRENT");
    if(soft_on <= 0 || soft_cur <= 0) return;
    //读不到真实温度时不启动：宁可沿用原策略，也不把异常值当成高温
    if(!read_battery_real_temp(&start_tenth)) return;
    if(start_tenth < soft_on*10) return;
    soft_start_active=1;
    soft_start_since=time(NULL);
    printf_with_time("插线时电池温度%.1f℃高于软启动阈值%d℃，先用%dμA降温（降到%d℃或满%d秒后恢复原策略）",
                     (double)start_tenth/10.0, soft_on, soft_cur,
                     read_one_option("SOFT_START_RECOVER"), read_one_option("SOFT_START_TIMEOUT"));
}

/*
对传入的电流上限施加软启动限制，返回最终应写入的值
只降不升：传入值本身比软启动值更小时保持传入值
*/
static int soft_start_limit(int current, char *stage, int stage_size)
{
    int soft_cur=0,soft_rec=0,soft_timeout=0,now_tenth=0,have_now=0;
    long elapsed=0;
    if(!soft_start_active) return current;
    soft_cur=read_one_option("SOFT_START_CURRENT");
    soft_rec=read_one_option("SOFT_START_RECOVER");
    soft_timeout=read_one_option("SOFT_START_TIMEOUT");
    if(soft_cur <= 0) { soft_start_active=0; return current; }
    have_now=read_battery_real_temp(&now_tenth);
    elapsed=(long)(time(NULL)-soft_start_since);
    if(have_now && soft_rec > 0 && now_tenth <= soft_rec*10)
    {
        soft_start_active=0;
        printf_with_time("软启动结束：电池温度已降到%.1f℃（目标%d℃），恢复按熔断/分段策略执行", (double)now_tenth/10.0, soft_rec);
        return current;
    }
    if(soft_timeout > 0 && elapsed >= soft_timeout)
    {
        soft_start_active=0;
        printf_with_time("软启动结束：已持续%ld秒仍未降到%d℃，交回熔断/分段策略", elapsed, soft_rec);
        return current;
    }
    /*
    只有真的把电流压小了才改阶段名
    熔断/临界已经给出更低的电流时（例如临界4.5A > 软启动4A 是降，3A < 4A 是保持），
    阶段名必须反映实际生效的那一档，否则日志里会看到"SOFT-START"却写着3A
    */
    if(current > 0 && current <= soft_cur)
    {
        if(should_log() && have_now) printf_with_time("软启动中：电池温度%.1f℃，维持更低的%dμA（软启动上限%dμA）",
                                                     (double)now_tenth/10.0, current, soft_cur);
        return current;
    }
    if(stage != NULL && stage_size > 0) snprintf(stage, stage_size, "SOFT-START");
    if(should_log())
    {
        if(have_now) printf_with_time("软启动中：电池温度%.1f℃，电流限制%dμA（降到%d℃或满%ld秒后结束）",
                                      (double)now_tenth/10.0, soft_cur, soft_rec, (long)soft_timeout-elapsed);
        else printf_with_time("软启动中：暂时读不到电池温度，电流限制%dμA", soft_cur);
    }
    return soft_cur;
}

/*
==================== 温度梯度（三段阶梯） ====================
v80 实测依据（11-18 早，9%→100%，全程亮屏）：
  8A恒流把电池从45.2℃推到52.1℃用了8.5分钟，升温速率 +0.84℃/分；
  熔断降到6A后5分钟就回落到48.3℃，此后长期稳定在46~49℃，速率 -0.21℃/分。
  但解除阈值是46℃，所以从07:05一直锁到07:30（24分钟），白白丢掉20分钟的8A窗口。

线性外推热平衡电流约6.4A（≈27W），7A对应的平衡温度约54℃（≈29.5W）。
这就是"稳定30W"的现实落点：允许电池停在53~55℃，换接近30W的持续功率。

三级定义（每级各自带解除阈值，只降不升，一次只降一级）：
  1级 ≥FORCE_TEMP_MAX(52℃) → HIGHEST_TEMP_CURRENT(7A)，跌破 GUARD_RECOVER(49℃) 解除
  2级 ≥GUARD2_TEMP(55℃)    → GUARD2_CURRENT(6A)，跌破 GUARD2_RECOVER(52℃) 回1级
  3级 ≥CRITICAL_TEMP(57℃)  → CRITICAL_CURRENT(4.5A)，跌破 CRITICAL_RECOVER(54℃) 回2级

为什么每级单独设解除阈值、而不是共用最低那个：
共用低位阈值时，温度一跌到该值就整段放开到8A，下一分钟又顶回阈值，形成整段横跳。
分级滞回让温度落在 52~55℃ 时稳定停在1级（7A），既不横跳，也不会掉回8A。
*/
static int temp_tier=0;           //0=不限流 1=7A档 2=6A档 3=4.5A档
static time_t tier_high_since=0;  //进入3级的时刻，用于CRITICAL_HOLD最短保持

/*
把 option.txt 里的六个阈值整理成递增且带滞回的数组
配置写反（比如2级阈值低于1级）时强行修正，否则上一级永远不会单独生效，
用户只会看到"一直在大电流"或"一直在降流"这两种极端表现，无法从日志看出配错了
enter[i] 是进入第 i+1 级的温度，exitc[i] 是第 i 级跌破后解除的温度（0.1℃ 制由调用方换算）
*/
static void temp_tier_thresholds(int *enter, int *exitc)
{
    int t1=read_one_option("FORCE_TEMP_MAX"), e1=read_one_option("GUARD_RECOVER");
    int t2=read_one_option("GUARD2_TEMP"), e2=read_one_option("GUARD2_RECOVER");
    int t3=read_one_option("CRITICAL_TEMP"), e3=read_one_option("CRITICAL_RECOVER");
    if(t1 < 35) t1=35;
    if(e1 >= t1-1) e1=t1-3;
    if(e1 < 30) e1=30;
    //2级必须比1级高出至少2℃，否则1级形同虚设
    if(t2 < t1+2) t2=t1+2;
    if(e2 >= t2-1) e2=t2-2;
    if(e2 <= e1) e2=e1+2;
    if(t3 < t2+2) t3=t2+2;
    if(e3 >= t3-1) e3=t3-2;
    if(e3 <= e2) e3=e2+2;
    enter[0]=t1; enter[1]=t2; enter[2]=t3;
    exitc[0]=0; exitc[1]=e1; exitc[2]=e2; exitc[3]=e3;
}

static int temp_tier_current(int tier)
{
    int v=0;
    if(tier == 1) v=read_one_option("HIGHEST_TEMP_CURRENT");
    else if(tier == 2) v=read_one_option("GUARD2_CURRENT");
    else if(tier == 3) v=read_one_option("CRITICAL_CURRENT");
    if(v < 100000) v=100000;
    return v;
}

static const char *temp_tier_stage(int tier)
{
    if(tier == 3) return "TEMP-CRIT";
    if(tier == 2) return "TEMP-GUARD2";
    if(tier == 1) return "TEMP-GUARD";
    return NULL;
}

/*
按电池真实温度（0.1℃制）推进梯度状态
升温方向允许跨级：温度突升时必须一次性到位，逐级爬会漏掉高温
降温方向一次只降一级，且离开3级要满足最短保持时间：
  温度读数本身有±1.5℃抖动，v79 实测从57℃跌到54.5℃只用了20秒就放回大电流，
  随后立刻回升到56.3℃，形成 6A/4.5A 周期性横跳，平均功率反而更低
读不到温度（tenth<=0）时保持原状态，绝不因为读不到就放开大电流
*/
static void temp_tier_step(int tenth, const int *enter, const int *exitc, int hold_sec)
{
    if(tenth <= 0) return;
    while(temp_tier < 3 && tenth >= enter[temp_tier]*10)
    {
        temp_tier++;
        if(temp_tier == 3) tier_high_since=time(NULL);
    }
    if(temp_tier > 0 && tenth <= exitc[temp_tier]*10)
    {
        if(temp_tier == 3 && hold_sec > 0 && (long)(time(NULL)-tier_high_since) < hold_sec) return;
        temp_tier--;
    }
}

int main()
{
    char *temp_sensor = NULL,**current_limit_file = NULL,**current_max_file = NULL,**temp_file = NULL;
    char charge[25]={0},power[10]={0},current_max_char[20]={0},highest_temp_current_char[20]={0},stage_name[20]={0},last_appname[FOREGROUND_APP_STRING_MAX_SIZE]={0};
    uchar step_charge=1,power_control=1,force_temp=1,has_force_temp=0,current_change=1,battery_status=1,battery_capacity=1;
    int temp_int=0,current_limit_file_num=0,last_charge_stop=-1,charge_is_stop=0,is_first_time=1,temp_sensor_num=0;
    int *current_max_unit=NULL;
    int current_max_file_num=0,temp_file_num=0,is_bypass=0,can_get_foreground=0,screen_is_off=0,last_temp_max=-1,last_charge_status=0,read_option_time=5;
    int applied_current=0,full_handled=0;
    //温度梯度状态（temp_tier / tier_high_since）是文件作用域的静态变量，见 temp_tier_step 上方注释
    time_t last_reapply_time=0;
    pthread_t thread1 = {0},thread2 = {0};
    //初始化链表
    insert_all_option();
    charge_log_init();
    printf("作者：酷安@诺鸡鸭\n");
    printf("GitHub开源地址：https://github.com/chase535/turbo-charge\n\n");
    //如果是写入文件，则必须加上这句话，不然只能等缓冲区满了后才会一次性写入
    fflush(stdout);
    //只允许一个实例运行，退出码100代表“已有实例在运行”，守护脚本据此停止重启
    if(!ensure_single_instance())
    {
        printf_with_time("已有另一个实例在运行，本次不再启动");
        return 100;
    }
    //判断各个所需文件是否存在
    check_required_files(&battery_status, &battery_capacity, &power_control,
                         &step_charge, &force_temp, &current_change,
                         &temp_sensor_num, &temp_sensor,
                         &current_max_file, &current_max_file_num, &current_max_unit,
                         &current_limit_file, &current_limit_file_num,
                         &temp_file, &temp_file_num);
    //找不到可写电流节点时生成节点自检报告，用户把该文件发给模块作者即可定位本机真实节点名
    if(!current_change)
    {
        write_probe_report();
        printf_with_time("电流功能失效，已生成节点自检报告：%s，请将本文件发给模块作者", PROBE_FILE);
    }
    //如果有电流文件，则获取安卓版本，为以后“伪”旁路供电做准备
    if(current_change) can_get_foreground=check_android_version();
    //创建一个子线程用来读取配置文件
    pthread_create(&thread2, NULL, &read_option_file, (void *)&read_option_time);
    pthread_detach(thread2);
    load_current_config(current_max_char, 20, highest_temp_current_char, 20);
    printf_with_time("充电采样日志：/data/adb/turbo-charge/charge_log.csv（充电中每5秒一行）");
    /*
    启动时把当前生效的温控策略打印出来
    温度相关的三个阈值互相牵制，光看option.txt很容易配出"阈值低于环境常态"这种组合，
    结果就是充电全程都在熔断降流里跑，日志里只看得到一个很低的平均功率，很难定位
    */
    {
        /*
        等配置线程读完第一遍再打印，否则打印的是编译期默认值而不是option.txt里的实际值
        最多等2秒：配置文件异常读不出来时不能把启动过程卡死
        */
        int wait_ms=0;
        while(!option_load_ready && wait_ms < 2000) { usleep(20000); wait_ms+=20; }
        if(!option_load_ready) printf_with_time("警告：配置文件读取超时，以下策略为本次使用的值，可能尚未与option.txt同步");
        /*
        这条提示必须带上 step_charge 判断
        v81实测：本机没有 battery/step_charging_enabled 文件（启动时已明确报过"阶梯式充电控制的所有功能失效"），
        但这段提示只看配置项，于是出现了自相矛盾的日志——
        一边说阶梯充电功能失效，一边建议你把它关掉。功能本来就不存在，关不关都一样
        step_charge==0 表示本机不支持，此时直接跳过提示
        */
        if(step_charge != 0 && read_one_option("SMART_CHARGE") == 1 && read_one_option("STEP_CHARGING_DISABLED") != 1)
            printf_with_time("提示：智能分段充电已启用，但阶梯式充电仍处于开启状态，系统会在电量过半后自行降流，建议把option.txt中的STEP_CHARGING_DISABLED改为1");
        int guard_on_show=read_one_option("FORCE_TEMP_MAX"), guard_off_show=read_one_option("GUARD_RECOVER");
        int gap=guard_on_show-guard_off_show;
        printf_with_time("温度梯度：1级%d℃降为%dμA（%d℃解除） / 2级%d℃降为%dμA（%d℃回1级） / 3级%d℃降为%dμA（%d℃回2级，至少保持%d秒）",
                         guard_on_show, read_one_option("HIGHEST_TEMP_CURRENT"), guard_off_show,
                         read_one_option("GUARD2_TEMP"), read_one_option("GUARD2_CURRENT"), read_one_option("GUARD2_RECOVER"),
                         read_one_option("CRITICAL_TEMP"), read_one_option("CRITICAL_CURRENT"),
                         read_one_option("CRITICAL_RECOVER"), read_one_option("CRITICAL_HOLD"));
        printf_with_time("手机温控：%d℃进%d℃出 / 电池温度伪装的熔断上限由1级阈值决定", read_one_option("TEMP_MAX"), read_one_option("RECHARGE_TEMP"));
        {
            int boost_show=read_one_option("INPUT_CURRENT_BOOST");
            if(boost_show > 0)
                printf_with_time("警告：输入限流提升实验已开启，input_current_limit 固定写为%dμA。若充电器不支持更高电压档，可能出现掉到5V慢充，请在本轮充电中确认 icl_a/vin_v 是否正常；不需要时把 INPUT_CURRENT_BOOST 改回0", boost_show);
        }
        {
            int soft_on_show=read_one_option("SOFT_START_TEMP");
            if(soft_on_show > 0)
                printf_with_time("高温软启动：插线时电池≥%d℃先限%dμA，降到%d℃或满%d秒后恢复原策略",
                                 soft_on_show, read_one_option("SOFT_START_CURRENT"),
                                 read_one_option("SOFT_START_RECOVER"), read_one_option("SOFT_START_TIMEOUT"));
            else printf_with_time("高温软启动：已关闭（SOFT_START_TEMP=0）");
        }
        if(gap < 2) printf_with_time("警告：熔断阈值与解除阈值只差%d℃，等于没有滞回，温度抖动会导致大电流和小电流每秒来回切，建议拉开3℃以上", gap);
        if(guard_on_show <= 46) printf_with_time("警告：熔断阈值%d℃偏低，多数机型充电时的常态温度就在45~47℃，阈值压在这个区间会让充电大部分时间处于限流状态", guard_on_show);
        /*
        熔断判断和日志观测用的都是电池自身的温度，不是手机温度传感器
        手机温度传感器（wifi_therm等）在满速充电叠加重负载时能到54℃，
        而电池可能只有47℃，拿它当电池温度判断会在电池并不热的时候误降流
        */
        const char *bzone=battery_zone_path();
        const char *btype=battery_zone_type();
        if(bzone != NULL)
        {
            printf_with_time("电池真实温度观测通道：%s（type=%s）", (char *)bzone, (btype != NULL)?(char *)btype:"未知");
            if(battery_zone_is_mirror())
                printf_with_time("警告：该节点是 power_supply battery/temp 的镜像，本模块把温度伪造成28℃后它也会跟着变成28.0，日志的tbat_raw列将失去参考价值。请执行 ls /sys/class/thermal/ 并把每个 thermal_zoneN/type 的内容发给模块作者以适配本机");
        }
        else printf_with_time("警告：thermal子系统中找不到电池温度传感器，真实温度只能退回bms/temp，而它会被本模块伪装成28℃，日志tbat_raw列将失去参考价值");
    }
    printf_with_time("文件检测完毕，程序开始运行");
    //写入初值
    apply_kernel_values();
    last_reapply_time=time(NULL);
    set_array_value(current_limit_file, current_limit_file_num, "-1");
    charge_ctl("1");
    //前期工作全部完成，程序正式开始运行
    while(1)
    {
        //读配置文件并赋值给程序内部变量
        load_current_config(current_max_char, 20, highest_temp_current_char, 20);
        //周期性复查那些只在启动时写一次的内核参数，被系统改回时及时补写
        if(time(NULL) - last_reapply_time >= REAPPLY_INTERVAL)
        {
            int written=apply_kernel_values();
            if(written > 0) printf_with_time("检测到%d项内核参数被系统改回，已重新写入", written);
            last_reapply_time=time(NULL);
        }
        //如果无法判断手机的充电状态，则很多功能无法实现，剩余代码很简单
        if(!battery_status)
        {
            //电量文件存在时仍然按电量进行阶梯充电的判断，旧版本此处从未读取过电量
            if(battery_capacity && !read_file("/sys/class/power_supply/battery/capacity", power, sizeof(power)))
            {
                if(should_log()) printf_with_time("暂时无法读取电池电量，等待下次循环");
                sleep(read_one_option("CYCLE_TIME"));
                continue;
            }
            //阶梯充电
            if(step_charge == 1)
            {
                if(read_one_option("STEP_CHARGING_DISABLED") == 1) (atoi(power) < read_one_option("STEP_CHARGING_DISABLED_THRESHOLD"))?step_charge_ctl("1"):step_charge_ctl("0");
                else step_charge_ctl("1");
            }
            else if(step_charge == 2) (read_one_option("STEP_CHARGING_DISABLED") == 1)?step_charge_ctl("0"):step_charge_ctl("1");
            //“伪”旁路供电
            if(current_change && can_get_foreground)
            {
                bypass_charge_ctl(&thread1, &can_get_foreground, last_appname, &is_bypass, &screen_is_off, current_max_file, current_max_file_num, current_max_unit);
                if(is_bypass && !screen_is_off)
                {
                    sleep(read_one_option("CYCLE_TIME"));
                    continue;
                }
            }
            //向电流文件写入配置文件中所设置的最大电流
            if(current_change) set_current_array(current_max_file, current_max_unit, current_max_file_num, atoi(current_max_char));
            sleep(read_one_option("CYCLE_TIME"));
            continue;
        }
        if(force_temp && read_one_option("FORCE_TEMP") == 1 && !has_force_temp) has_force_temp=1;
        //读取手机电量
        if(!read_file("/sys/class/power_supply/battery/capacity", power, sizeof(power)))
        {
            if(should_log()) printf_with_time("暂时无法读取电池电量，等待下次循环");
            sleep(read_one_option("CYCLE_TIME"));
            continue;
        }
        //阶梯充电，无论是否在充电，都会跟随配置文件进行更改
        if(step_charge == 1)
        {
            if(read_one_option("STEP_CHARGING_DISABLED") == 1) (atoi(power) < read_one_option("STEP_CHARGING_DISABLED_THRESHOLD"))?step_charge_ctl("1"):step_charge_ctl("0");
            else step_charge_ctl("1");
        }
        else if(step_charge == 2) (read_one_option("STEP_CHARGING_DISABLED") == 1)?step_charge_ctl("0"):step_charge_ctl("1");
        //读取充电状态
        if(!read_file("/sys/class/power_supply/battery/status", charge, sizeof(charge)))
        {
            if(should_log()) printf_with_time("暂时无法读取充电状态，等待下次循环");
            sleep(read_one_option("CYCLE_TIME"));
            continue;
        }
        //如果手机在充电，则根据配置文件进行相应操作
        if(strcmp(charge, "Discharging"))
        {
            //此处通过两个变量来确定上次循环时的充电状态以及是否是程序的第一次循环
            if(is_first_time || !last_charge_status)
            {
                printf_with_time("充电器已连接");
                last_charge_status=1;
                is_first_time=0;
                //插线瞬间电池已经偏热时先降流降温，细节见 soft_start_begin 的说明
                soft_start_begin();
            }
            /*
            电池充满后不再干预
            继续往constant_charge_current_max写大电流并伪造成28℃，会让内核一直维持涓流充电，
            表现出来就是“电量已经100%了，充电器还在输出”
            */
            if(!strcmp(charge, "Full") || atoi(power) >= 100)
            {
                if(has_force_temp)
                {
                    set_temp(temp_sensor, temp_file, temp_file_num, 0);
                    has_force_temp=0;
                }
                if(!full_handled)
                {
                    printf_with_time("电池已充满，停止修改充电参数并交还系统控制");
                    full_handled=1;
                }
                if(power_control) powel_ctl(&last_charge_stop, &charge_is_stop);
                charge_log_sample(atoi(power), charge, 0, "FULL", temp_sensor);
                sleep(read_one_option("CYCLE_TIME"));
                continue;
            }
            if(full_handled)
            {
                printf_with_time("电量已回落到100%%以下，恢复充电参数控制");
                full_handled=0;
            }
            //充电时强制显示28度功能
            //不能无条件写28℃：真实温度超过FORCE_TEMP_MAX时必须把真实值交还给系统，
            //否则系统自身的JEITA过温保护会一直以为电池只有28℃
            if(force_temp && read_one_option("FORCE_TEMP") == 1)
            {
                set_temp(temp_sensor, temp_file, temp_file_num, 1);
                has_force_temp=1;
            }
            else if(has_force_temp) set_temp(temp_sensor, temp_file, temp_file_num, 0);
            //电量控制功能
            if(power_control) powel_ctl(&last_charge_stop, &charge_is_stop);
            //如果电流文件存在且安卓版本大于等于7，则执行“伪”旁路供电函数
            if(current_change && can_get_foreground)
            {
                bypass_charge_ctl(&thread1, &can_get_foreground, last_appname, &is_bypass, &screen_is_off, current_max_file, current_max_file_num, current_max_unit);
                //如果进入了“伪”旁路供电模式，则不执行下面的温度控制相关函数，直接进入下次循环
                if(is_bypass && !screen_is_off)
                {
                    sleep(read_one_option("CYCLE_TIME"));
                    continue;
                }
            }
            //温度控制功能，温度读数无效时不做任何动作，避免把异常值当成低温而误恢复大电流
            if(read_one_option("TEMP_CTRL") == 1 && temp_sensor_num && temp_sensor != NULL && current_change)
            {
                if(!read_temp(temp_sensor, &temp_int))
                {
                    if(should_log()) printf_with_time("暂时无法读取温度传感器的温度值，本轮跳过温度控制");
                }
                //如果温度大于等于配置文件所设置的值，则打印相关信息并进入另一个循环中，等待相关事件的发生后退出二层循环
                else if(temp_int >= read_one_option("TEMP_MAX")*1000)
                {
                    printf_with_time("手机温度大于等于降低充电电流的温度阈值，限制充电电流为%sμA", highest_temp_current_char);
                    //如果进入了“伪”旁路供电模式则退出此循环
                    while(!is_bypass)
                    {
                        //每次循环重新读取配置文件及温度值
                        load_current_config(current_max_char, 20, highest_temp_current_char, 20);
                        set_array_value(current_limit_file, current_limit_file_num, "-1");
                        if(force_temp && read_one_option("FORCE_TEMP") == 1 && !has_force_temp) has_force_temp=1;
                        //读数异常时保持当前的限制状态，不写入任何数值，等待下一次采样
                        if(!read_temp(temp_sensor, &temp_int))
                        {
                            if(should_log()) printf_with_time("暂时无法读取温度传感器的温度值，保持当前充电电流");
                            sleep(read_one_option("CYCLE_TIME"));
                            continue;
                        }
                        //此tmp数组在读取配置文件的函数中，若配置文件的TEMP_MAX发生改变则会得到相应更改
                        //作用是判断新的降低充电电流的温度阈值与旧的温度阈值的大小
                        if(last_temp_max == -1) last_temp_max=read_one_option("TEMP_MAX");
                        if(last_temp_max != read_one_option("TEMP_MAX"))
                        {
                            last_temp_max=read_one_option("TEMP_MAX");
                            if(temp_int < read_one_option("TEMP_MAX")*1000)
                            {
                                printf_with_time("新的降低充电电流的温度阈值高于旧的温度阈值，且手机温度小于新的温度阈值，恢复充电电流为%sμA", current_max_char);
                                break;
                            }
                            else printf_with_time("新的降低充电电流的温度阈值高于旧的温度阈值，但手机温度大于等于新的温度阈值，限制充电电流为%sμA", highest_temp_current_char);
                            last_temp_max=read_one_option("TEMP_MAX");
                        }
                        //判断手机充电状态
                        if(!read_file("/sys/class/power_supply/battery/status", charge, sizeof(charge))) charge[0]='\0';
                        if(!strcmp(charge, "Discharging"))
                        {
                            printf_with_time("充电器断开连接，恢复充电电流为%sμA", current_max_char);
                            last_charge_status=0;
                            break;
                        }
                        //判断温度是否小于恢复快充的温度阈值
                        if(temp_int <= read_one_option("RECHARGE_TEMP")*1000)
                        {
                            printf_with_time("手机温度小于等于恢复快充的温度阈值，恢复充电电流为%sμA", current_max_char);
                            break;
                        }
                        //判断温度控制功能是否关闭
                        if(!read_one_option("TEMP_CTRL"))
                        {
                            printf_with_time("温控关闭，恢复充电电流为%sμA", current_max_char);
                            break;
                        }
                        //阶梯充电
                        if(step_charge == 1)
                        {
                            if(read_one_option("STEP_CHARGING_DISABLED") == 1) (atoi(power) < read_one_option("STEP_CHARGING_DISABLED_THRESHOLD"))?step_charge_ctl("1"):step_charge_ctl("0");
                            else step_charge_ctl("1");
                        }
                        else if(step_charge == 2) (read_one_option("STEP_CHARGING_DISABLED") == 1)?step_charge_ctl("0"):step_charge_ctl("1");
                        //向电流文件中写入限制电流数值
                        /*
                        内层温控循环同样要跑完整的三级梯度
                        TEMP-CTRL 是二层循环，一旦进去就不再走外层的梯度判断，
                        如果这里只判最高一级，温度落在52~55℃之间时会一直用
                        HIGHEST_TEMP_CURRENT（7A）跑，把温度继续往上顶。
                        */
                        int inner_current=atoi(highest_temp_current_char);
                        /*
                        内层循环的起点电流取 1级(7A)与2级(6A)中较小的那个
                        能进到这里说明手机本体（wifi_therm）已经≥TEMP_MAX=58℃，
                        整机已经在发热，此时用6A起步比7A稳妥；电池温度再由下面的梯度继续下调
                        */
                        {
                            int g2=read_one_option("GUARD2_CURRENT");
                            if(g2 > 0 && (inner_current <= 0 || g2 < inner_current)) inner_current=g2;
                        }
                        int enter_th_inner[3]={0}, exit_th_inner[4]={0};
                        int crit_hold_inner=read_one_option("CRITICAL_HOLD");
                        int real_tenth_inner=0;
                        temp_tier_thresholds(enter_th_inner, exit_th_inner);
                        if(crit_hold_inner < 0) crit_hold_inner=0;
                        if(read_battery_real_temp(&real_tenth_inner))
                        {
                            int was_tier_inner=temp_tier;
                            temp_tier_step(real_tenth_inner, enter_th_inner, exit_th_inner, crit_hold_inner);
                            if(temp_tier != was_tier_inner)
                            {
                                //与外层同一套规则：升档和降档分开写，避免"低温却写着按高温阈值进档"
                                int rose_inner=(temp_tier > was_tier_inner);
                                if(temp_tier == 0)
                                    printf_with_time("电池温度%.1f℃已跌破%d℃，解除温度梯度，恢复分段电流%sμA",
                                                     (double)real_tenth_inner/10.0, exit_th_inner[1], current_max_char);
                                else if(!rose_inner)
                                    printf_with_time("电池温度%.1f℃已跌破%d℃，由%d级降到%d级，电流放开到%dμA",
                                                     (double)real_tenth_inner/10.0, exit_th_inner[was_tier_inner], was_tier_inner,
                                                     temp_tier, temp_tier_current(temp_tier));
                                else printf_with_time("电池温度%.1f℃升到%d级限流，电流%dμA",
                                                      (double)real_tenth_inner/10.0, temp_tier, temp_tier_current(temp_tier));
                            }
                            if(temp_tier > 0)
                            {
                                int lim=temp_tier_current(temp_tier);
                                if(lim > 0 && (inner_current <= 0 || inner_current > lim)) inner_current=lim;
                                if(should_log() && temp_tier == 2) printf_with_time("电池温度%.1f℃处于2级限流（%d~%d℃），电流保持%dμA",
                                                                                   (double)real_tenth_inner/10.0, exit_th_inner[2], enter_th_inner[1], inner_current);
                                else if(should_log() && temp_tier == 3) printf_with_time("电池温度%.1f℃处于3级限流，电流保持%dμA",
                                                                                        (double)real_tenth_inner/10.0, inner_current);
                            }
                        }
                        /*
                        内层温控循环同样要受软启动约束：软启动期间如果主板温度先一步
                        超过TEMP_MAX就会进到这个循环，这里若不再判一次，4A会被直接
                        覆盖成7A，软启动等于白做
                        */
                        char inner_stage[20]={0};
                        if(temp_tier > 0) snprintf(inner_stage, sizeof(inner_stage), "%s", temp_tier_stage(temp_tier));
                        else snprintf(inner_stage, sizeof(inner_stage), "TEMP-CTRL");
                        inner_current=soft_start_limit(inner_current, inner_stage, sizeof(inner_stage));
                        set_current_array(current_max_file, current_max_unit, current_max_file_num, inner_current);
                        charge_log_sample(atoi(power), charge, inner_current, inner_stage, temp_sensor);
                        //充电时强制显示28度功能
                        //此处已经处于高温限流状态，温度基本都超过FORCE_TEMP_MAX，
                        //set_temp(tempwall=1)会把真实温度交给系统，不再继续伪装
                        if(force_temp && read_one_option("FORCE_TEMP") == 1) set_temp(temp_sensor, temp_file, temp_file_num, 1);
                        else if(has_force_temp) set_temp(temp_sensor, temp_file, temp_file_num, 0);
                        //电量控制功能
                        if(power_control) powel_ctl(&last_charge_stop, &charge_is_stop);
                        sleep(read_one_option("CYCLE_TIME"));
                    }
                }
            }
            /*
            按电量分段限流，并用温度传感器的原始读数做独立熔断
            FORCE_TEMP把电池温度伪造成28℃之后，系统自己的过温保护等于失效，
            所以这里必须再拿一遍thermal_zone的真实温度，按三级梯度降流
            */
            applied_current=stage_current_limit(atoi(power), atoi(current_max_char), stage_name, sizeof(stage_name));
            if(read_one_option("TEMP_CTRL") == 1 && temp_sensor_num && temp_sensor != NULL)
            {
                int real_temp=0;
                /*
                熔断判断用电池自身的温度，而不是手机温度传感器
                实测wifi_therm（主板/WiFi区）在满速充电叠加重负载时能到54.4℃，
                同期电池只有47~48℃。拿主板温度当电池温度判断，会在电池并不热的时候
                误触发熔断，白白把功率压下去。thermal子系统的电池传感器模块不写，
                伪装成28℃之后仍然读得到真值，所以优先用它，读不到再退回手机温度传感器
                */
                int real_tenth=0;
                int have_bat_temp=read_battery_real_temp(&real_tenth);
                /*
                read_battery_real_temp() 返回的是0.1℃制（47.2℃ → 472），
                而下面所有阈值比较都按0.001℃制（47.2℃ → 47200），所以必须乘100。
                v76/v77 这里误写成乘10，49℃的阈值实际变成了490℃，熔断从来没有触发过 ——
                实测电池侧温度一路冲到62.9℃，模块全程没有任何干预。这个错误不会再犯：
                两个单位制之间只差10倍还是100倍，写反了不会有任何报错，只会安静地失效。
                */
                if(have_bat_temp) real_temp=real_tenth*100;
                else if(!read_temp(temp_sensor, &real_temp)) real_temp=0;
                if(real_temp > 0)
                {
                    /*
                    三级温度梯度（阈值与设计依据见 temp_tier_thresholds 上方注释）
                    限流值统一取 min(分段电流, 该级限流值)：
                    旧版是直接赋值，1级限流时会把 80-90% 段的 5.6A 抬回 6A，
                    属于往"电流变大"的方向改，与限流的本意相反
                    */
                    int enter_th[3]={0}, exit_th[4]={0};
                    int crit_hold=read_one_option("CRITICAL_HOLD");
                    int was_tier=temp_tier;
                    int tenth_for_tier=have_bat_temp?real_tenth:(real_temp/100);
                    temp_tier_thresholds(enter_th, exit_th);
                    if(crit_hold < 0) crit_hold=0;
                    temp_tier_step(tenth_for_tier, enter_th, exit_th, crit_hold);
                    if(temp_tier > 0)
                    {
                        int lim=temp_tier_current(temp_tier);
                        if(lim > 0 && (applied_current <= 0 || applied_current > lim)) applied_current=lim;
                        snprintf(stage_name, sizeof(stage_name), "%s", temp_tier_stage(temp_tier));
                    }
                    if(temp_tier != was_tier)
                    {
                        /*
                        必须区分升档和降档
                        v81实测日志里出现过"电池温度51.5℃进入2级限流（≥55℃）"这种自相矛盾的记录：
                        温度只有51.5℃，却写着按55℃阈值进档。实际是温度回落后逐级下移，
                        逻辑没错，但文案让人误读成升温跳档，排查时会往完全错误的方向找
                        */
                        int rose=(temp_tier > was_tier);
                        if(temp_tier == 0)
                            printf_with_time("电池温度%.1f℃已跌破%d℃，解除温度梯度，恢复分段电流%dμA",
                                             (double)tenth_for_tier/10.0, exit_th[1], applied_current);
                        else if(!rose)
                            printf_with_time("电池温度%.1f℃已跌破%d℃，由%d级降到%d级，电流放开到%dμA",
                                             (double)tenth_for_tier/10.0, exit_th[was_tier], was_tier, temp_tier, applied_current);
                        else if(temp_tier == 1)
                            printf_with_time("电池温度%.1f℃进入1级限流（≥%d℃），电流限制为%dμA，跌破%d℃解除",
                                             (double)tenth_for_tier/10.0, enter_th[0], applied_current, exit_th[1]);
                        else if(temp_tier == 2)
                            printf_with_time("电池温度%.1f℃进入2级限流（≥%d℃），电流降为%dμA，跌破%d℃回到1级",
                                             (double)tenth_for_tier/10.0, enter_th[1], applied_current, exit_th[2]);
                        else
                            printf_with_time("电池温度%.1f℃进入3级限流（≥%d℃），电流降为%dμA，跌破%d℃且至少保持%d秒才回到2级",
                                             (double)tenth_for_tier/10.0, enter_th[2], applied_current, exit_th[3], crit_hold);
                    }
                    else if(temp_tier > 0 && should_log())
                    {
                        if(temp_tier == 3 && crit_hold > 0 && tenth_for_tier <= exit_th[3]*10)
                        {
                            long held=(long)(time(NULL)-tier_high_since);
                            if(held < crit_hold)
                                printf_with_time("电池温度%.1f℃已回到3级解除阈值%d℃以下，但最短保持%d秒未到（已保持%ld秒），电流暂保持%dμA",
                                                 (double)tenth_for_tier/10.0, exit_th[3], crit_hold, held, applied_current);
                            else printf_with_time("电池温度%.1f℃处于3级限流，电流保持%dμA", (double)tenth_for_tier/10.0, applied_current);
                        }
                        else printf_with_time("电池温度%.1f℃处于%d级限流（%d~%d℃），电流保持%dμA",
                                              (double)tenth_for_tier/10.0, temp_tier, exit_th[temp_tier], enter_th[temp_tier-1], applied_current);
                    }
                }
                else if(temp_tier > 0)
                {
                    //读不到温度时沿用上一次的梯度档位，绝不因为"读不到"就放开大电流
                    int lim=temp_tier_current(temp_tier);
                    if(lim > 0 && (applied_current <= 0 || applied_current > lim)) applied_current=lim;
                    snprintf(stage_name, sizeof(stage_name), "%s", temp_tier_stage(temp_tier));
                    if(should_log()) printf_with_time("暂时无法读取温度，保持%d级限流，电流%dμA", temp_tier, applied_current);
                }
            }
            //高温软启动：只降不升，与分段/熔断/临界取最小值叠加
            applied_current=soft_start_limit(applied_current, stage_name, sizeof(stage_name));
            /*
            输入限流提升实验（INPUT_CURRENT_BOOST，默认0关闭）
            电池侧电流受温度约束，但实测的 26W 天花板在输入侧：
            v79 满速段 icl 整轮恒为 3.225A，按88%效率反推输入 8.87V，即适配器在 9V 档，
            9V×3.225A=29W 就是输入上限，输出不可能超过 26W。
            想让输出上 30W（7A@4.3V 需 34W 输入），必须让 PD 重协商到 11V/20V 档。
            这里把 input_current_limit 单独写大，不跟随电池电流，试探内核会不会重新协商。
            注意必须先把该节点从 current_max_file 里摘掉：否则 set_current_array 每轮
            都会把它改回 applied_current（临界降流时是4.5A），和本项写入互相覆盖，
            输入限流会在两个值之间每秒抖动，AICL 反而更容易误判
            */
            {
                int boost=read_one_option("INPUT_CURRENT_BOOST");
                /*
                最多摘 8 个：本机可写电流节点里带 input_current_limit 的有两个
                （mt6375 的 chg/input_current_limit 和 mtk-master-charger/input_current_limit），
                内核取两者最小值生效，只摘一个等于没摘
                */
                char *saved_ptr[8]={0};
                int saved_idx[8]={0};
                int saved_n=0,i=0,write_ret=0;
                static int last_boost=0;
                static time_t boost_written_at=0;
                time_t now_boost=time(NULL);
                if(boost > 0 && current_change && current_max_file != NULL)
                {
                    for(i=0;i<current_max_file_num && saved_n < 8;i++)
                    {
                        if(current_max_file[i] != NULL && strstr(current_max_file[i], "input_current_limit"))
                        {
                            saved_idx[saved_n]=i;
                            saved_ptr[saved_n]=current_max_file[i];
                            current_max_file[i]=NULL;
                            saved_n++;
                        }
                    }
                }
                //向电流文件中写入本轮允许的最大充电电流
                //返回0表示节点当前值已等于目标（快充满时系统自己降到同一个值），不需要告警
                if(current_change) write_ret=set_current_array(current_max_file, current_max_unit, current_max_file_num, applied_current);
                for(i=0;i<saved_n;i++) current_max_file[saved_idx[i]]=saved_ptr[i];
                if(current_change && write_ret < 0 && should_log())
                    printf_with_time("本轮未能写入任何电流节点（共%d个），节点可能已被系统独占，稍后重试", current_max_file_num);
                /*
                值变化时立即写；此外每 30 秒补写一次。
                补写是必需的：拔插充电器会让系统把该节点重置为协商值，
                只靠"值变化"判断的话重插之后本项就永久失效了
                */
                if(boost > 0 && (boost != last_boost || (now_boost-boost_written_at) >= 30))
                {
                    /*
                    写全部被摘下来的 input_current_limit 节点，而不是只写 mtk-master-charger
                    内核取这些节点的最小值生效，只写一个，另一个仍会卡在协商值上
                    */
                    char boost_buf[24]={0};
                    char boost_path[96]="/sys/class/power_supply/mtk-master-charger/input_current_limit";
                    int ok_n=0;
                    snprintf(boost_buf, sizeof(boost_buf), "%d", boost);
                    for(i=0;i<saved_n;i++)
                    {
                        if(set_value(saved_ptr[i], boost_buf) > 0) ok_n++;
                    }
                    //一个都没摘到（例如节点表里只有 constant_charge_current）时退回固定路径
                    if(saved_n == 0 && set_value(boost_path, boost_buf) > 0) ok_n=1;
                    if(ok_n > 0)
                    {
                        if(boost != last_boost)
                            printf_with_time("输入限流已设为%dμA（INPUT_CURRENT_BOOST），共写入%d个节点，请观察接下来1分钟的 icl_a / icl_max_a / vin_v 是否触发更高电压档", boost, ok_n);
                        last_boost=boost;
                        boost_written_at=now_boost;
                    }
                    else if(should_log()) printf_with_time("输入限流写入失败，节点可能不存在或只读，INPUT_CURRENT_BOOST 本轮未生效");
                }
                else if(boost <= 0) last_boost=0;
            }
            charge_log_sample(atoi(power), charge, applied_current, stage_name, temp_sensor);
        }
        //如果手机未在充电，则只会刷新显示温度，以及若执行了获取前台应用包名的子线程，则取消此子线程
        else
        {
            //此处通过两个变量来确定上次循环时的充电状态以及是否是程序的第一次循环
            if(is_first_time)
            {
                printf_with_time("充电器未连接");
                is_first_time=0;
                last_charge_status=0;
            }
            else if(last_charge_status)
            {
                printf_with_time("充电器断开连接");
                last_charge_status=0;
            }
            //通过判断子线程是否仍在运行来决定是否需要取消它，若在运行则取消并清空ForegroundAppName
            pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
            if(strlen((char *)ForegroundAppName) || foreground_thread_alive())
            {
                pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
                printf_with_time("手机未在充电状态，“伪”旁路供电功能暂时停用");
                pthread_cancel(thread1);
                /*
                在子线程中，使用sleep(5)来实现5秒循环获取前台应用
                且在sleep(5)后紧跟pthread_testcancel()函数来检查子线程是否收到了取消信号
                此处等待1秒以确保子线程能够处于等待5秒的过程中或使子线程完成等待5秒
                这样在完成等待后就能直接在取消点取消子线程，从而使ForegroundAppName不会被子线程重新赋值
                */
                sleep(1);
                pthread_mutex_lock((pthread_mutex_t *)&mutex_foreground_app);
                memset((void *)ForegroundAppName, 0, sizeof(ForegroundAppName));
            }
            pthread_mutex_unlock((pthread_mutex_t *)&mutex_foreground_app);
            //此标识符代表手机是否处于“伪”旁路供电模式
            //如果在停止充电前手机处于“伪”旁路供电模式，则停止充电后将此标识符置0
            if(is_bypass) is_bypass=0;
            //因为断电也有可能是由电量控制引起的，所以此处执行电量控制函数
            if(power_control) powel_ctl(&last_charge_stop, &charge_is_stop);
            //此标识符代表手机是否通过此程序修改了电池温度
            //如果修改了，则电池温度无法再自动刷新，需要通过此程序进行刷新
            //未充电时没有伪装温度的必要，一律写回真实温度，避免残留的28℃误导用户和系统
            if(has_force_temp)
            {
                set_temp(temp_sensor, temp_file, temp_file_num, 0);
                has_force_temp=0;
            }
            charge_log_sample(atoi(power), charge, 0, "IDLE", temp_sensor);
        }
        sleep(read_one_option("CYCLE_TIME"));
    }
    return 0;
}
