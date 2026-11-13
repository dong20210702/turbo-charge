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
探测电流节点是否可用，并判断它的数值单位
返回该节点“1个数值”等于多少μA：μA节点返回1，mA节点返回1000
返回0表示节点不可用（读不到，或当前值是负数）

必须区分单位的原因：同一台MTK机器上，
/sys/class/power_supply/battery/constant_charge_current 读出来是 22000000（μA，即22A），
而 /sys/class/power_supply/11280000.i2c:mt6375@34:chg/constant_charge_current 读出来是 2700（mA，即2.7A），
两者语义相同但单位差1000倍，用同一个数字去写会把电流放大或缩小一千倍
*/
int probe_unit_factor(const char *path, int *raw_value)
{
    int fd=0, v=0;
    ssize_t len=0;
    char buf[48]={0}, *p=buf;
    if(path == NULL) return 0;
    fd=open(path, O_RDONLY);
    if(fd < 0) return 0;
    len=read(fd, buf, sizeof(buf)-1);
    close(fd);
    if(len <= 0) return 0;
    buf[len]='\0';
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    v=(int)strtol(p, NULL, 10);
    if(raw_value != NULL) *raw_value=v;
    //MTK的充电IC节点（形如 11280000.i2c:mt6375@34:chg）用mA，未启用的从充电器会读到-95这类错误码
    if(v < 0) return 0;
    //先按节点名判断，名字不受当前充电阶段影响，比按数值猜更可靠
    if(strstr(path, "i2c:") != NULL || strstr(path, ":chg/") != NULL) return 1000;
    //名字判断不出来时再看数值大小：mA节点的值不会超过30000（30A）
    if(v > 0 && v <= 30000) return 1000;
    return 1;
}

/*
向一组电流节点写入目标电流（单位μA），unit为check_required_files扫描时定下的每个节点的换算系数
与set_array_value的区别：
1.按节点单位自动换算，mA节点写入换算后的数值
2.节点当前值为负数（MTK未启用的从充电器节点返回-95）时直接跳过，写进去也没有任何作用
3.值本来就一致时不重复写，减少每秒一次的无谓系统调用
返回真正发生写入的节点个数
*/
int set_current_array(char **file, int *unit, int num, int target_ua)
{
    int i=0, written=0, factor=1, cur_raw=0;
    char buf[24]={0};
    if(file == NULL || num <= 0) return 0;
    if(target_ua < 0) target_ua=0;
    for(i=0;i < num;i++)
    {
        if(file[i] == NULL) continue;
        factor=probe_unit_factor(file[i], &cur_raw);
        if(factor <= 0) continue;
        if(unit != NULL && unit[i] > 0) factor=unit[i];
        if(cur_raw*factor == target_ua) continue;
        snprintf(buf, sizeof(buf), "%d", target_ua/factor);
        if(set_value(file[i], buf) > 0) written++;
    }
    return written;
}

/*
向二级指针所指向的温度文件们写入温度
tempwall为是否启用超过一定温度时强制向温度文件写入28℃
*/
void set_temp(char *temp_sensor, char **temp_file, int temp_file_num, uchar tempwall)
{
    char thermal[15]={0},bat_temp[8]={0};
    int raw_temp=0,temp_int=0,i=0;
    if(temp_sensor == NULL) return;
    //读取温度文件
    if(!read_file(temp_sensor, thermal, sizeof(thermal))) return;
    raw_temp=atoi(thermal);
    //0、1、-1是节点不可用时的占位值，直接丢弃，避免把“读不到温度”当成“温度很低”
    if(raw_temp <= 1) return;
    /*
    温度源可能是 thermal_zone（0.001℃）也可能是 power_supply（0.1℃），按量级判定：
    raw < 2000 视为 0.1℃ 制，否则视为 0.001℃ 制。
    temp_int 统一换算成整摄氏度用于阈值比较，bat_temp 一律按 0.1℃ 制写回
    （power_supply 的 temp 节点就是 0.1℃，写 280 表示 28.0℃）
    */
    if(raw_temp < 2000) temp_int=raw_temp/10;
    else temp_int=raw_temp/1000;
    snprintf(bat_temp, sizeof(bat_temp), "%d", temp_int*10);
    /*
    只伪装系统JEITA真正读取的 battery/temp，其余温度节点一律不动
    /sys/class/power_supply 下同时存在 bms/temp、cp_master/temp、mtk-master-charger/temp 等多个温度节点，
    旧版本把它们全部写成28℃，后果是系统自己的过温保护被绕过之后，
    连"真实电池温度是多少"都再没有任何地方能读到 —— 一旦散热出问题，日志里也看不出来。
    现在保留 bms/temp 等节点的真实读数，作为独立观测通道（charge_log 的 tbat_raw 列就是它）
    */
    for(i=0;i < temp_file_num;i++)
    {
        if(temp_file[i] == NULL) continue;
        if(strstr(temp_file[i], "/battery/") == NULL) continue;
        if(tempwall)
        {
            int threshold=read_one_option("FORCE_TEMP_MAX");
            if(threshold < 35) threshold=35;
            set_value(temp_file[i], (temp_int >= threshold)?bat_temp:"280");
        }
        else set_value(temp_file[i], bat_temp);
    }
}
