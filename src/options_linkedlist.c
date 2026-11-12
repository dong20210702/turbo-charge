#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_malloc.h"
#include "options_linkedlist.h"
#include "printf_with_time.h"
#include "my_thread.h"


//链表的头结点，只存储开始节点的地址
ListNode options_head;

//添加节点
void insert_option(char *name, int value)
{
    ListNode *next,*tmp=&options_head;
    next=(ListNode *)my_calloc(1, sizeof(ListNode));
    //用snprintf替代strncpy：源串长度达到上限时strncpy不补'\0'，会被-Werror=stringop-truncation判为错误
    snprintf(next->name, OPTION_NAME_MAX_SIZE, "%s", name);
    next->value=value;
    next->next=NULL;
    while(tmp->next) tmp=tmp->next;
    tmp->next=next;
}

//添加配置并将所有配置预先设为初始值
void insert_all_option()
{
    insert_option("CYCLE_TIME", 1);
    insert_option("FORCE_TEMP", 1);
    insert_option("CURRENT_MAX", 50000000);
    insert_option("STEP_CHARGING_DISABLED", 0);
    insert_option("TEMP_CTRL", 1);
    insert_option("POWER_CTRL", 0);
    insert_option("STEP_CHARGING_DISABLED_THRESHOLD", 15);
    insert_option("CHARGE_STOP", 95);
    insert_option("CHARGE_START", 80);
    insert_option("TEMP_MAX", 52);
    insert_option("HIGHEST_TEMP_CURRENT", 2000000);
    insert_option("RECHARGE_TEMP", 45);
    insert_option("BYPASS_CHARGE", 0);
    //智能分段充电：按电量分档限制电流，让降速发生在高电量段
    insert_option("SMART_CHARGE", 1);
    //分段起点（单位:%），低于该值给满速
    insert_option("STAGE_START", 80);
    //智能分段模式下的满速基准电流（单位:μA），8000000=8A
    insert_option("PEAK_CURRENT", 8000000);
    //真实温度熔断阈值（单位:℃），用温度传感器原始读数判断，不受强制显示28℃影响
    insert_option("FORCE_TEMP_MAX", 45);
}

//读取单个配置的值
int read_one_option(char *name)
{
    int value=-1;
    ListNode *node;
    pthread_mutex_lock((pthread_mutex_t *)&mutex_options);
    for(node=options_head.next;node;node=node->next)
    {
        if(!(strcmp(node->name, name)))
        {
            value=node->value;
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&mutex_options);
    if(value < 0)
    {
        printf_with_time("无法获取变量，程序发生了内部错误，请立即前往Github进行反馈！");
        exit(98765);
    }
    return value;
}
