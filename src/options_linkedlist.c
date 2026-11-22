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
    insert_option("TEMP_MAX", 58);
    /*
    1级限流的电流上限（单位:μA），7000000=7A
    v80 实测两条边界：8A 时电池升温 +0.84℃/分，6A 时降温 -0.21℃/分，
    线性外推热平衡电流约 6.4A；7A 的平衡温度约 54℃，输出约 29.5W，
    这是"稳定30W"在本机散热条件下唯一能长时间站住的档位
    */
    insert_option("HIGHEST_TEMP_CURRENT", 7000000);
    insert_option("RECHARGE_TEMP", 50);
    insert_option("BYPASS_CHARGE", 0);
    //智能分段充电：按电量分档限制电流，让降速发生在高电量段
    insert_option("SMART_CHARGE", 1);
    //分段起点（单位:%），低于该值给满速
    insert_option("STAGE_START", 80);
    //智能分段模式下的满速基准电流（单位:μA），8000000=8A
    insert_option("PEAK_CURRENT", 8000000);
    //1级限流阈值（单位:℃），用电池自身温度判断，不受强制显示28℃影响
    insert_option("FORCE_TEMP_MAX", 52);
    /*
    1级解除阈值（单位:℃）
    v80 实测：8A 把电池推到52℃用了8.5分钟，降6A后5分钟就回落到48.3℃，
    此后整段停在46~49℃。旧值46℃意味着要等跌到46才放开，结果一口气锁了24分钟。
    49℃让温度一离开 52℃ 的峰值带就交回 8A，与 2级阈值(55℃)之间留足间隔
    */
    insert_option("GUARD_RECOVER", 49);
    /*
    熔断时是否把真实温度交还给系统（0=不交还，1=交还）
    默认0：本机实测，系统看到46℃以上的真实电池温度后会对充电器做开/关硬切断，
    59分钟里电量从38%只涨到48%就不再增长，比限流慢充还差。
    交还真实温度虽然能换来系统自身的过温保护，但代价是几乎充不进电，
    所以默认由模块自己限流降温，真实温度改为记在日志的tbat_raw列里供人工核对。
    */
    insert_option("GUARD_HANDOVER", 0);
    /*
    临界温度（单位:℃），超过后再降一档电流，防止长时间停在高温区
    v78实测：这一档设在60℃时等于没有——6A限流下电池真实温度峰值59.1℃，
    全程一次都没触发过，二级降流形同虚设。前移到57℃才真正参与控制
    */
    insert_option("CRITICAL_TEMP", 57);
    /*
    临界温度下的电流上限（单位:μA），4500000=4.5A，在4.2V上约19W
    4.5A是实测能把温度从58℃往下拉的档位；3A（12.6W）降得太狠，
    只是为了压3℃的温度不值得多花十几分钟
    */
    insert_option("CRITICAL_CURRENT", 4500000);
    /*
    临界降流的解除温度（单位:℃）
    不能沿用熔断的GUARD_RECOVER(46℃)：一旦57℃触发临界，要等电池跌回46℃
    才恢复6A，而充电中后段温度长期在50~58℃之间，等于触发一次就永久锁在4.5A。
    53℃保留4℃滞回，既能退出又不会在57℃附近来回跳
    v79实测：54℃解除时，05:52:23读到54.5℃、20秒后读到53.0℃就退出了，
    而温度读数本身有±1.5℃抖动，54℃这个门限刚好落在抖动带上
    */
    insert_option("CRITICAL_RECOVER", 54);
    /*
    临界降流的最短保持时间（单位:秒）
    v79实测：临界 05:48:59 触发、05:52:34 就解除，只保持了3.5分钟，
    温度从57℃降到55.4℃就被放回6A，随后立刻回升到56.3℃。
    若那时没进恒压区，就会变成6A→57℃→4.5A→53℃→6A 的周期性横跳，
    平均功率反而比稳定4.5A更低。180秒足够让温度真正脱离临界带
    0=不限制（退回v79行为）
    */
    insert_option("CRITICAL_HOLD", 180);
    /*
    4级（最后一级）：亮屏重负载、SoC 把电池仓烤热时兜底
    v84实测（11-22 早，2%→97%）：全程 CPU 均 76.4℃、峰 97℃，电池温度与 CPU 相关系数 0.788、
    与充电电流相关系数 -0.301（电流越小反而越热）。8A 只跑了9.4分钟就被推到低档，
    之后 29.7 分钟停在 3级 4.5A，电池平均 56.9℃、峰值 59.5℃，43% 的电量是在这个温度下灌进去的。
    也就是说 3级 4.5A 是原设计的最后一级，到 59.5℃ 时模块已经无档可退。
    60℃ 是常见电芯的 JEITA 快充上限，也是这块电池（83% 健康、802 次循环）不该长期停留的温度
    */
    insert_option("CRITICAL2_TEMP", 60);
    /*
    4级的电流上限（单位:μA），2000000=2A，约8W
    只求把温度拉回来，不追求速度；重负载场景下用户本来就在用手机，
    8W 仍能让电量缓慢上升，比直接停充更容易被接受
    */
    insert_option("CRITICAL2_CURRENT", 2000000);
    /*
    4级解除温度（单位:℃），跌破58℃回3级
    与3级阈值57℃拉开3℃，和 CRITICAL_HOLD 一起避免在 60℃ 附近来回跳；
    注意回3级后电流是 4.5A 而不是 2A，所以解除点不能太紧
    */
    insert_option("CRITICAL2_RECOVER", 58);
    /*
    高温软启动：插线瞬间电池已经偏热时，先降流把温度拉下来再按正常策略走
    v79实测结论是这项没有收益，默认关闭：
    插线瞬间电池电流本身从1.2A自然爬升，4A限流前20秒完全不起作用；
    等电流碰到4A时温度已经52.7℃，限流真正生效只有65秒、
    温度降到48.2℃就退出，随后6A用16分钟又顶回57℃。
    净效果只是延后90秒，还白扔了一段4A低功率（15.6W vs 25W），少充约0.5个点
    想重新启用就把它改成45~50
    */
    insert_option("SOFT_START_TEMP", 0);
    //软启动期间的电流上限（单位:μA），4000000=4A，约17W
    insert_option("SOFT_START_CURRENT", 4000000);
    //软启动的结束温度（单位:℃），温度降到该值以下提前结束
    insert_option("SOFT_START_RECOVER", 48);
    /*
    软启动的最长持续时间（单位:秒）
    亮屏重负载时4A也可能降不到48℃，没有这个兜底会一直卡在4A，
    充电时间被无谓拉长。到点后交回熔断/分段策略，该降的温度已经降过了
    */
    insert_option("SOFT_START_TIMEOUT", 360);
    /*
    2级限流：温度继续上升时从7A再降一档
    v80 实测 7A 的平衡温度约 54℃，已经贴近 3级阈值，
    留给它的任务不是长期稳定，而是防止在逆光/高温环境（比如车里、太阳直射）
    里一直卡在1级时温度缓慢爬过 55℃ 而无人处理
    */
    insert_option("GUARD2_TEMP", 55);
    //2级限流的电流上限（单位:μA），6000000=6A，约25W
    insert_option("GUARD2_CURRENT", 6000000);
    /*
    2级解除阈值（单位:℃）
    跌破 52℃ 回到1级（7A），与 1级 52进/49出 交叉，形成 2℃ 的稳定带：
    温度落在 52~55℃ 之间时停在1级，既不会整段放开到 8A，也不会掉回 6A
    */
    insert_option("GUARD2_RECOVER", 52);
    /*
    输入限流提升实验开关（单位:μA，0=关闭）
    本项 >0 时把全部 input_current_limit 节点单独设成该值（不跟随电池电流），
    试图触发内核重新协商更高的电压档位。
    注意：早期曾用"icl_a 恒为 3.225A"反推出适配器在 9V 档、输入上限 29W，
    但这个结论已被实测推翻——同一配置下输出达到过 34.5W。
    icl_a 是静态寄存器读数，不能用来反推真实输入功率。
风险：若充电器/线材不支持更高档位，写大电流会被 AICL 压回，
      更坏的情况是掉到 5V 档变成慢充。默认 0 关闭，
      要用请单轮验证，并在充电开始后 1 分钟内确认 pbat_w 与 icl_a 没有恶化
    */
    insert_option("INPUT_CURRENT_BOOST", 0);
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
