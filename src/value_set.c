#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "value_set.h"

/*
向文件写入数据，返回1代表发生了写入，0代表内容本来就一致（未写入），-1代表写入失败或文件不存在
运行期间节点短暂不可用是正常现象（拔插充电器、驱动重新注册节点），本函数只跳过本次写操作，绝不结束进程
使用原始文件IO而不是标准C的流：内核属性文件不支持随意定位，fopen/fgets/rewind/fputs这套组合
在部分节点上会写不进去（v67之前反复出现的“写不进电流”就是同一个原因）
*/
int set_value(char *file, char *numb)
{
    int fd;
    ssize_t len=0,value_len=0;
    char content[64]={0};
    if(file == NULL || numb == NULL) return -1;
    value_len=(ssize_t)strlen(numb);
    fd=open(file, O_RDWR);
    if(fd < 0)
    {
        //权限0644不得简写成644
        chmod(file, 0644);
        fd=open(file, O_RDWR);
        //部分节点是只写属性，读不了但能写
        if(fd < 0) fd=open(file, O_WRONLY);
        if(fd < 0) return -1;
    }
    len=read(fd, content, sizeof(content)-1);
    if(len > 0)
    {
        content[len]='\0';
        line_feed(content);
        //先判断文件内容与所写内容是否一致，若不一致才进行写入
        if(!strcmp(content, numb))
        {
            close(fd);
            return 0;
        }
    }
    //回到文件开头再写，内核节点只接受从偏移0开始的一次写入
    if(lseek(fd, 0, SEEK_SET) != 0)
    {
        close(fd);
        return -1;
    }
    len=write(fd, numb, value_len);
    close(fd);
    return (len == value_len)?1:-1;
}

//向二级指针所指向的文件们写入数据
void set_array_value(char **file, int num, char *value)
{
    if(file == NULL || value == NULL) return;
    for(int i=0;i < num;i++) set_value(file[i], value);
}

/*
向二级指针所指向的温度文件们写入温度
tempwall为是否启用超过一定温度时强制向温度文件写入28℃
*/
void set_temp(char *temp_sensor, char **temp_file, int temp_file_num, uchar tempwall)
{
    char thermal[15]={0},bat_temp[8]={0};
    int raw_temp=0,temp_int=0;
    if(temp_sensor == NULL) return;
    //读取温度文件
    if(!read_file(temp_sensor, thermal, sizeof(thermal))) return;
    raw_temp=atoi(thermal);
    //0、1、-1是节点不可用时的占位值，直接丢弃，避免把“读不到温度”当成“温度很低”
    if(raw_temp <= 1) return;
    //thermal_zone的temp单位是0.001℃，power_supply的temp单位是0.1℃，直接除以100取整
    //旧版本用snprintf截断字符串来换算，温度低于10℃或为负数时会被算错（9.5℃→95℃、-5℃→-50℃）
    temp_int=raw_temp/100;
    snprintf(bat_temp, sizeof(bat_temp), "%d", temp_int);
    //如果tempwall为1，则判断温度是否超过指定值，若超过则显示真实温度（交给系统自己的温控），否则显示28℃
    //如果tempwall为0，则永远显示正常温度
    /*
    阈值以前是写死的45℃，现在改为读取配置FORCE_TEMP_MAX：
    电池温度被长期伪造成28℃后，系统自身的JEITA保护完全失效，
    用户在看到"28.0℃"的同时真实温度可能已经四十多度，
    所以超过阈值时必须把真实温度交还给系统，不能继续骗下去
    */
    if(tempwall)
    {
        int threshold=read_one_option("FORCE_TEMP_MAX");
        if(threshold < 35) threshold=35;
        (temp_int >= threshold)?set_array_value(temp_file, temp_file_num, bat_temp):set_array_value(temp_file, temp_file_num, "280");
    }
    else set_array_value(temp_file, temp_file_num, bat_temp);
}
