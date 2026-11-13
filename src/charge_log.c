#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "charge_log.h"
#include "printf_with_time.h"

#define CHARGE_LOG_FILE      "/data/adb/turbo-charge/charge_log.csv"
#define CHARGE_LOG_OLD_FILE  "/data/adb/turbo-charge/charge_log.old.csv"
//充电中每5秒一行、未充电每30秒一行
#define CHARGE_LOG_INTERVAL_CHARGING   5
#define CHARGE_LOG_INTERVAL_IDLE       30
//超过4MB就轮转，避免长时间挂机把分区写满
#define CHARGE_LOG_MAX_SIZE            (4*1024*1024)

static FILE *log_fp = NULL;
static time_t last_write_time = 0;

/*
轻量读取，不调用read_file
read_file内部会通过check_read_file对每个不存在的节点重试250ms，
而本模块要读十几个节点，其中不少是机型相关、可能永久不存在的
（pd_active、voltage_max、charge_full等），
用read_file会让主循环被拖慢好几秒，进而影响充电控制的实时性
*/
static int raw_read(const char *path, char *buf, int size)
{
    int fd;
    ssize_t len;
    if(path == NULL || size <= 1) return 0;
    fd=open(path, O_RDONLY);
    if(fd < 0) return 0;
    len=read(fd, buf, size-1);
    close(fd);
    if(len <= 0) return 0;
    buf[len]='\0';
    while(len > 0 && (buf[len-1]=='\n' || buf[len-1]=='\r' || buf[len-1]==' ')) buf[--len]='\0';
    return 1;
}

static int read_long(const char *path, long *out)
{
    char buf[64]={0};
    char *end=NULL;
    long value=0;
    if(!raw_read(path, buf, sizeof(buf))) return 0;
    value=strtol(buf, &end, 10);
    if(end == buf) return 0;
    *out=value;
    return 1;
}

//按倍率换算后写入，读不到时写NA
static void put_scaled(FILE *fp, const char *path, double divider, int precision)
{
    long value=0;
    if(!read_long(path, &value)) { fputs("NA", fp); return; }
    fprintf(fp, "%.*f", precision, (double)value/divider);
}

//同上，但取绝对值（内核上报的充电电流在部分机型上是负数）
static void put_scaled_abs(FILE *fp, const char *path, double divider, int precision)
{
    long value=0;
    if(!read_long(path, &value)) { fputs("NA", fp); return; }
    if(value < 0) value=-value;
    fprintf(fp, "%.*f", precision, (double)value/divider);
}

static void put_long(FILE *fp, const char *path)
{
    long value=0;
    if(!read_long(path, &value)) { fputs("NA", fp); return; }
    fprintf(fp, "%ld", value);
}

static void put_text(FILE *fp, const char *path)
{
    char buf[32]={0};
    if(!raw_read(path, buf, sizeof(buf))) { fputs("NA", fp); return; }
    //CSV里不能出现分隔符
    for(int i=0;buf[i];i++) if(buf[i]==',') buf[i]=';';
    fputs(buf, fp);
}

/*
探测输入限流(ICL)与输入电压节点
这两项是判断"功率天花板在哪"的核心：
  icl_a   内核此刻真正允许的输入电流。模块写进去的是8A，但MTK的AICL会按
          充电器/线材实际能力把它压回去，压到多少只有读这个节点才知道
  vin_v   充电器实际协商到的电压。9V档×3A=27W，正好卡在用户抱怨的"峰值30W上不去"
机型不同节点名不同，所以按候选表逐个试，第一次探测成功后缓存路径，避免每5秒扫一遍
*/
static const char *icl_candidates[]={
    "/sys/class/power_supply/mtk-master-charger/input_current_limit",
    "/sys/class/power_supply/battery/input_current_limit",
    "/sys/class/power_supply/usb/input_current_limit",
    "/sys/class/power_supply/charger/input_current_limit",
    NULL
};
static const char *vin_candidates[]={
    "/sys/class/power_supply/mtk-master-charger/voltage_now",
    "/sys/class/power_supply/charger/voltage_now",
    "/sys/class/power_supply/usb/voltage_now",
    NULL
};
static const char *icl_path = NULL;
static const char *vin_path = NULL;

static const char *pick_current_node(const char **candidates, long min_value)
{
    long value=0;
    int i=0;
    for(i=0;candidates[i];i++)
    {
        if(read_long(candidates[i], &value) && value >= min_value) return candidates[i];
    }
    return NULL;
}

//输入电流，单位可能是μA也可能是mA，按量级判定后统一换算成A
static void put_icl(FILE *fp)
{
    long value=0;
    char buf[32]={0};
    if(icl_path == NULL) icl_path=pick_current_node(icl_candidates, 1000);
    if(icl_path == NULL) { fputs("NA", fp); return; }
    if(!raw_read(icl_path, buf, sizeof(buf))) { icl_path=NULL; fputs("NA", fp); return; }
    value=strtol(buf, NULL, 10);
    if(value <= 0) { fputs("NA", fp); return; }
    //<=30000 只可能是mA制（30A以上的μA值不现实），其余按μA处理
    if(value <= 30000) fprintf(fp, "%.3f", (double)value/1000.0);
    else fprintf(fp, "%.3f", (double)value/1e6);
}

static void put_vin(FILE *fp)
{
    long value=0;
    if(vin_path == NULL) vin_path=pick_current_node(vin_candidates, 1000000);
    if(vin_path == NULL) { fputs("NA", fp); return; }
    if(!read_long(vin_path, &value) || value <= 0) { vin_path=NULL; fputs("NA", fp); return; }
    fprintf(fp, "%.2f", (double)value/1e6);
}

void charge_log_init(void)
{
    log_fp=NULL;
    last_write_time=0;
}

void charge_log_close(void)
{
    if(log_fp != NULL)
    {
        fflush(log_fp);
        fclose(log_fp);
        log_fp=NULL;
    }
}

/*
写一行采样
各列含义见表头，单位统一为 V / A / ℃ / mAh，
与手机上的充电统计软件对照时注意：本文件的 ibat_a 是电池侧电流，不是充电器输入侧电流
*/
void charge_log_sample(int capacity, const char *status, int applied_current, const char *stage, const char *temp_sensor)
{
    struct stat statbuf={0};
    struct tm time_now;
    time_t now=time(NULL);
    char time_str[32]={0};
    char sensor_str[16]={0};
    int interval=CHARGE_LOG_INTERVAL_CHARGING;
    double vbat=0, ibat=0;
    long value=0;
    int has_vbat=0, has_ibat=0;

    if(status != NULL && !strcmp(status, "Discharging")) interval=CHARGE_LOG_INTERVAL_IDLE;
    //限频，主循环本身是每秒一轮
    if(last_write_time != 0 && (now - last_write_time) < interval) return;

    //超过上限就轮转，只保留上一份，避免长期挂机写满分区
    if(log_fp != NULL && !stat(CHARGE_LOG_FILE, &statbuf) && statbuf.st_size > CHARGE_LOG_MAX_SIZE)
    {
        fclose(log_fp);
        log_fp=NULL;
        rename(CHARGE_LOG_FILE, CHARGE_LOG_OLD_FILE);
    }
    if(log_fp == NULL)
    {
        int need_header=1;
        if(!stat(CHARGE_LOG_FILE, &statbuf) && statbuf.st_size > 0) need_header=0;
        log_fp=fopen(CHARGE_LOG_FILE, "a");
        if(log_fp == NULL) return;
        if(need_header)
        {
            fputs("time,cap_pct,status,chg_type,health,"
                  "vbat_v,ibat_a,pbat_w,"
                  "tbat_c,tsensor_c,"
                  "vusb_v,iusb_a,usb_online,vbus_max_v,ibus_max_a,"
                  "ccc_max_a,ccc_a,icl_limited,icl_a,vin_v,"
                  "chg_full_mah,chg_design_mah,cycles,"
                  "tcpu_c,"
                  "applied_a,stage\n", log_fp);
        }
    }
    last_write_time=now;

    localtime_r(&now, &time_now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_now);

    fprintf(log_fp, "%s,%d,%s,", time_str, capacity, (status != NULL && status[0])?status:"NA");
    //充电类型与电池健康度：Fast/Trickle/Standard 能直接看出内核是否已经降到涓流，
    //health变成Overheat就是系统自己的过温判定起作用了
    put_text(log_fp, "/sys/class/power_supply/battery/charge_type");
    fputc(',', log_fp);
    put_text(log_fp, "/sys/class/power_supply/battery/health");
    fputc(',', log_fp);

    //电池电压(V) 电池电流(A) 电池功率(W)
    if(read_long("/sys/class/power_supply/battery/voltage_now", &value) && value > 0)
    {
        vbat=(double)value/1e6;
        has_vbat=1;
        fprintf(log_fp, "%.2f", vbat);
    }
    else fputs("NA", log_fp);
    fputc(',', log_fp);

    if(read_long("/sys/class/power_supply/battery/current_now", &value) && value != 0)
    {
        ibat=(double)value/1e6;
        if(ibat < 0) ibat=-ibat;
        has_ibat=1;
        fprintf(log_fp, "%.3f", ibat);
    }
    else fputs("NA", log_fp);
    fputc(',', log_fp);

    if(has_vbat && has_ibat) fprintf(log_fp, "%.2f", vbat*ibat);
    else fputs("NA", log_fp);
    fputc(',', log_fp);

    //电池温度：这一列在本模块启用FORCE_TEMP后会被伪造成28.0，只能当参考
    put_scaled(log_fp, "/sys/class/power_supply/battery/temp", 10.0, 1);
    fputc(',', log_fp);

    //温度传感器的原始读数(thermal_zone，单位0.001℃)：这一列才是真实温度，模块不会改写它
    if(temp_sensor != NULL && raw_read(temp_sensor, sensor_str, sizeof(sensor_str)))
    {
        value=strtol(sensor_str, NULL, 10);
        if(value > 1 && value < 200000) fprintf(log_fp, "%.1f", (double)value/1000.0);
        else fputs("NA", log_fp);
    }
    else fputs("NA", log_fp);
    fputc(',', log_fp);

    //充电器侧：能否看到9V/20V协商结果，是判断"67W套件有没有被识别"的关键
    put_scaled_abs(log_fp, "/sys/class/power_supply/usb/voltage_now", 1e6, 2);
    fputc(',', log_fp);
    put_scaled_abs(log_fp, "/sys/class/power_supply/usb/current_now", 1e6, 3);
    fputc(',', log_fp);
    put_long(log_fp, "/sys/class/power_supply/usb/online");
    fputc(',', log_fp);
    put_scaled_abs(log_fp, "/sys/class/power_supply/usb/voltage_max", 1e6, 2);
    fputc(',', log_fp);
    put_scaled_abs(log_fp, "/sys/class/power_supply/usb/current_max", 1e6, 3);
    fputc(',', log_fp);

    //内核实际的恒流上限与实际恒流值：用来判断模块写进去的电流有没有被系统改回去
    put_scaled_abs(log_fp, "/sys/class/power_supply/battery/constant_charge_current_max", 1e6, 3);
    fputc(',', log_fp);
    put_scaled_abs(log_fp, "/sys/class/power_supply/battery/constant_charge_current", 1e6, 3);
    fputc(',', log_fp);
    put_long(log_fp, "/sys/class/power_supply/battery/input_current_limited");
    fputc(',', log_fp);
    //输入限流实际生效值(A)与输入电压(V)：写进去的电流和真正生效的电流往往不是一回事
    put_icl(log_fp);
    fputc(',', log_fp);
    put_vin(log_fp);
    fputc(',', log_fp);

    //电池健康度：满充容量相对设计容量的比值决定了恒压阶段会提前多久到来
    put_scaled(log_fp, "/sys/class/power_supply/battery/charge_full", 1000.0, 0);
    fputc(',', log_fp);
    put_scaled(log_fp, "/sys/class/power_supply/battery/charge_full_design", 1000.0, 0);
    fputc(',', log_fp);
    put_long(log_fp, "/sys/class/power_supply/battery/cycle_count");
    fputc(',', log_fp);

    //SoC温度，thermal_zone0在多数机型上是AP，个别机型不是，仅作趋势参考
    put_scaled(log_fp, "/sys/class/thermal/thermal_zone0/temp", 1000.0, 1);
    fputc(',', log_fp);

    //本模块实际生效的电流上限(A)与策略档位
    fprintf(log_fp, "%.3f,%s\n", (double)applied_current/1e6, (stage != NULL && stage[0])?stage:"NA");
    fflush(log_fp);
}
