#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "charge_log.h"
#include "value_set.h"
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
  icl_a       内核此刻真正允许的输入电流。模块写进去的是8A，但MTK的AICL会按
              充电器/线材实际能力把它压回去，压到多少只有读这个节点才知道
  icl_max_a   内核认定的限流上限。与 icl_a 相等说明已打满、充电器到顶；
              远大于 icl_a 说明是系统自己在压
  icl_bat_a   battery/input_current_limit 的实际值，即模块写进去的那个节点
  vin_v       充电器实际协商到的电压。9V档×3.2A=29W，正好卡在"峰值30W上不去"
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
/*
扫到的真实输入电压节点
候选表在MTK上经常全部落空：mtk-master-charger没有voltage_now，
usb/voltage_now报的是10mV这种无效差值。与其继续猜节点名，
不如直接把power_supply下每个节点的voltage_now都读一遍，
取3.5V~30V区间里的最大值——电池电压被排除，剩下的最大值必然是充电器输入
*/
static char vin_scan_path[192]={0};
static int vin_scanned=0;

/*
判断 power_supply 下的节点是否属于电池侧
v79 的教训：bms/voltage_now 与 battery/voltage_now 同源，报的就是电池电压。
上一版扫描时 bms 是唯一通过 3.5V 下限的候选，vin_v 列于是变成了 vbat 的副本，
与 vbat 逐行完全相等（3.65/4.17/4.20…4.46）。这比记 NA 更糟：
NA 至少说明"读不到"，而假数据会让人以为输入电压已经采到了
判定顺序：节点名 → type 文件 → 名字兜底，任一命中即判为电池侧
*/
static int is_battery_side(const char *name)
{
    char path[192]={0};
    char type[48]={0};
    if(name == NULL) return 1;
    if(!strncmp(name, "battery", 7) || !strncmp(name, "bms", 3)) return 1;
    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", name);
    if(!raw_read(path, type, sizeof(type))) return (strstr(name, "batt") != NULL);
    if(strstr(type, "Battery") || strstr(type, "bms") || strstr(type, "batt")) return 1;
    return 0;
}

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

//电流类节点的通用输出：单位可能是μA也可能是mA，按量级判定后统一换算成A
static void put_current_auto(FILE *fp, const char *path)
{
    long value=0;
    char buf[32]={0};
    if(path == NULL) { fputs("NA", fp); return; }
    if(!raw_read(path, buf, sizeof(buf))) { fputs("NA", fp); return; }
    value=strtol(buf, NULL, 10);
    if(value <= 0) { fputs("NA", fp); return; }
    //<=30000 只可能是mA制（30A以上的μA值不现实），其余按μA处理
    if(value <= 30000) fprintf(fp, "%.3f", (double)value/1000.0);
    else fprintf(fp, "%.3f", (double)value/1e6);
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

/*
遍历 /sys/class/power_supply 下所有节点，挑出真正的充电器输入电压
输入电压的正常量级是 5V/9V/11V/20V（μV制即 5000000 起），
所以区间取 3.5V~30V：低于3.5V只可能是无效读数或单节电芯电压，
高于30V不可能是USB PD档位
两道排除，缺一不可：
  ① 电池侧节点（bms / battery / type=Battery）直接跳过
  ② 与 vbat 相差小于 0.4V 的候选跳过 —— 5V 是最低的 USB 输入档，
     与 4.4V 电池至少差 0.6V，落在 0.4V 以内只可能是电池电压的另一个镜像节点
扫描结果只打一次日志，并把每个候选的电压与排除原因一并列出，方便定位本机真实节点
*/
static void scan_voltage_nodes(void)
{
    DIR *dir=NULL;
    struct dirent *ent=NULL;
    char path[192]={0};
    char list[1024]={0};
    long value=0,best=0,vbat_uv=0;
    int n=0,has_vbat=0;
    vin_scanned=1;
    vin_scan_path[0]='\0';
    has_vbat=read_long("/sys/class/power_supply/battery/voltage_now", &vbat_uv) && vbat_uv > 0;
    dir=opendir("/sys/class/power_supply");
    if(dir == NULL) return;
    while((ent=readdir(dir)) != NULL)
    {
        int batt_side=0,near_vbat=0;
        if(ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/voltage_now", ent->d_name);
        if(!read_long(path, &value)) continue;
        batt_side=is_battery_side(ent->d_name);
        near_vbat=(!batt_side && has_vbat && value > vbat_uv-400000 && value < vbat_uv+400000);
        n=(int)strlen(list);
        if(n < (int)sizeof(list)-64)
        {
            if(batt_side)
                snprintf(list+n, sizeof(list)-n, "%s=%.2fV[电池侧] ", ent->d_name, (double)value/1e6);
            else if(near_vbat)
                snprintf(list+n, sizeof(list)-n, "%s=%.2fV[≈vbat] ", ent->d_name, (double)value/1e6);
            else
                snprintf(list+n, sizeof(list)-n, "%s=%.2fV ", ent->d_name, (double)value/1e6);
        }
        if(batt_side || near_vbat) continue;
        if(value >= 3500000 && value <= 30000000 && value > best)
        {
            best=value;
            snprintf(vin_scan_path, sizeof(vin_scan_path), "%s", path);
        }
    }
    closedir(dir);
    if(vin_scan_path[0] != '\0')
        printf_with_time("输入电压节点：%s（扫描到 %s）", vin_scan_path, (list[0] != '\0')?list:"无其他节点");
    else
        printf_with_time("警告：power_supply 下找不到可用的输入电压节点，vin_v 列将记为NA（扫描到 %s）", (list[0] != '\0')?list:"无");
}

static void put_vin(FILE *fp, int charging)
{
    long value=0,vbat_uv=0;
    int has_vbat=0;
    /*
    每次插上充电器都重新扫一遍：拔插后 power_supply 下的节点可能重新注册，
    指针或读数都会失效。只在充电边沿重置，充电过程中不重扫，避免刷日志
    */
    static int last_charging=0;
    if(charging && !last_charging) { vin_scanned=0; vin_path=NULL; }
    last_charging=charging;
    /*
    不充电时直接记NA：未插充电器时 power_supply 下本来就没有输入电压，
    部分机型还会把最后的读数留在节点里，读出来就是一个假的输入电压
    */
    if(!charging) { fputs("NA", fp); return; }
    if(!vin_scanned) scan_voltage_nodes();
    if(vin_scan_path[0] != '\0')
    {
        has_vbat=read_long("/sys/class/power_supply/battery/voltage_now", &vbat_uv) && vbat_uv > 0;
        if(read_long(vin_scan_path, &value) && value >= 3500000 && value <= 30000000
           && !(has_vbat && value > vbat_uv-400000 && value < vbat_uv+400000))
        {
            fprintf(fp, "%.2f", (double)value/1e6);
            return;
        }
        //读到无效值（低于3.5V或与电池电压接近）时清掉缓存并记NA
        //这里不重置 vin_scanned：重置会让下一轮立刻重扫，而扫描本身会打日志，
        //若该节点长期读数异常就会每5秒刷一条"输入电压节点"，反而盖住真正有用的信息
        vin_scan_path[0]='\0';
        fputs("NA", fp);
        return;
    }
    //扫描落空时才退回候选表，同样排除与电池电压接近的读数
    has_vbat=read_long("/sys/class/power_supply/battery/voltage_now", &vbat_uv) && vbat_uv > 0;
    if(vin_path == NULL) vin_path=pick_current_node(vin_candidates, 3500000);
    if(vin_path == NULL) { fputs("NA", fp); return; }
    if(!read_long(vin_path, &value) || value < 3500000 || value > 30000000
       || (has_vbat && value > vbat_uv-400000 && value < vbat_uv+400000)) { vin_path=NULL; fputs("NA", fp); return; }
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
                  "tbat_c,tbat_raw,tsensor_c,"
                  "vusb_v,iusb_a,usb_online,vbus_max_v,ibus_max_a,"
                  "ccc_max_a,ccc_a,icl_limited,icl_a,icl_max_a,icl_bat_a,vin_v,"
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

    //电池温度：这一列在本模块启用FORCE_TEMP后会一直是28.0（伪装生效的标志），要看真值请用下一列
    put_scaled(log_fp, "/sys/class/power_supply/battery/temp", 10.0, 1);
    fputc(',', log_fp);

    /*
    真实电池温度：取自thermal子系统的电池传感器（MTK为mtktsbtsnrpa/mtktsbtsmdpa），
    这个节点本模块从不写入，所以伪装生效后仍然读得到真值；
    只有在thermal里找不到电池传感器时才退回bms/temp，而bms/temp此时已被伪造成28.0，
    那一列的参考价值就没了 —— 启动日志会打印实际用的是哪一个
    v75曾经只伪装battery/temp、把bms/temp留给真值，结果bms驱动立刻覆盖battery/temp，
    伪装整体失效（实测电池47℃时系统把充电器反复切断，59分钟电量纹丝不动）
    */
    {
        int real_tenth=0;
        if(read_battery_real_temp(&real_tenth)) fprintf(log_fp, "%.1f", real_tenth/10.0);
        else fputs("NA", log_fp);
    }
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
    /*
    usb/voltage_now 在MTK平台上报的是10mV这种无效差值（日志里整轮恒为0.01），
    直接把原始值写进来会把"读不到"伪装成"读到了0.01V"，比NA更误导。
    低于3.5V一律记NA；真正的输入电压看后面的 vin_v 列，那一列会自动扫描可用节点
    */
    {
        long uv=0;
        if(read_long("/sys/class/power_supply/usb/voltage_now", &uv) && uv >= 3500000 && uv <= 30000000)
            fprintf(log_fp, "%.2f", (double)uv/1e6);
        else fputs("NA", log_fp);
    }
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
    /*
    icl_max_a：内核认定的输入限流上限。与上一列 icl_a 相等，说明 AICL 已经把
              限流打满、充电器/线材到顶了；远大于 icl_a，说明是系统自己在压
    icl_bat_a：battery/input_current_limit 的实际值，即模块写入的那个节点的真实状态。
              与 applied_a 不一致说明写入被内核改写
    上一轮实测 icl_a 整轮恒为 3.225A、输出峰值 26.7W，按 88% 效率反推输入 8.87V，
    即适配器工作在 9V 档：9V×3.225A=29W 就是输入侧天花板，输出不可能超过 26W。
    这两列就是用来确认"3.225A 是充电器给的还是系统压的"，是在 9V 档还是 11V 档
    */
    put_current_auto(log_fp, "/sys/class/power_supply/mtk-master-charger/input_current_limit_max");
    fputc(',', log_fp);
    put_current_auto(log_fp, "/sys/class/power_supply/battery/input_current_limit");
    fputc(',', log_fp);
    put_vin(log_fp, (status != NULL && strcmp(status, "Discharging") != 0));
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
