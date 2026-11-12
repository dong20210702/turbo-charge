#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "probe.h"

//读取节点内容，去掉结尾的换行，并限制长度
static void read_node_value(const char *path, char *buf, size_t size)
{
    FILE *fp=fopen(path, "r");
    if(fp == NULL)
    {
        snprintf(buf, size, "<无法读取>");
        return;
    }
    size_t n=fread(buf, 1, size-1, fp);
    fclose(fp);
    buf[n]='\0';
    while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n]='\0';
}

/*
把/sys/class/power_supply下所有节点写成报告
输出格式：每一行是“[W]或[R] 节点名 = 当前值”，W表示可写，R表示只读
*/
void write_probe_report(void)
{
    DIR *ps=opendir("/sys/class/power_supply"), *sub=NULL;
    struct dirent *node_entry=NULL, *file_entry=NULL;
    FILE *fp=NULL;
    char dir_path[256], file_path[512], value[65];

    fp=fopen(PROBE_FILE, "w");
    if(fp == NULL) return;
    fprintf(fp, "turbo-charge 节点自检报告\n");
    fprintf(fp, "[W]=可写 [R]=只读，值最多显示64字节\n");
    fprintf(fp, "如果模块报告电流功能失效，请把本文件发给模块作者\n");
    if(ps == NULL)
    {
        fprintf(fp, "\n无法打开 /sys/class/power_supply\n");
        fclose(fp);
        return;
    }
    while((node_entry=readdir(ps)) != NULL)
    {
        //跳过 . 和 ..
        if(node_entry->d_name[0] == '.') continue;
        snprintf(dir_path, sizeof(dir_path), "/sys/class/power_supply/%s", node_entry->d_name);
        sub=opendir(dir_path);
        if(sub == NULL) continue;
        fprintf(fp, "\n[%s]\n", dir_path);
        while((file_entry=readdir(sub)) != NULL)
        {
            if(file_entry->d_name[0] == '.') continue;
            snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, file_entry->d_name);
            read_node_value(file_path, value, sizeof(value));
            fprintf(fp, "  %s %s = %s\n", access(file_path, W_OK)?"[R]":"[W]", file_entry->d_name, value);
        }
        closedir(sub);
        sub=NULL;
    }
    closedir(ps);
    fclose(fp);
}
