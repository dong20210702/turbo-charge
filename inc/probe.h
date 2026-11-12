#ifndef PROBE_H
#define PROBE_H

//节点自检报告的固定输出路径
#define PROBE_FILE "/data/adb/turbo-charge/probe.txt"

/*
把/sys/class/power_supply下所有节点的路径、可写性、当前值写成一份报告
当模块找不到任何可写的电流节点时调用，用户把probe.txt发出来即可定位本机真实的节点名
*/
void write_probe_report(void);

#endif
