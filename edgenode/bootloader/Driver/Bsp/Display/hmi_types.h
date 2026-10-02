#ifndef HMI_TYPES_H_
#define HMI_TYPES_H_

/*
    从应用的 User/App/Presentation/Hmi/hmi_types.h 裁剪而来。

    应用那份还包含 hmi_view_data_t（温度、WiFi 状态、日志滚动窗口、页编号……），
    那些是应用的数据模型，Bootloader 用不上，全部去掉，只留屏幕尺寸。

    判断标准是"横还是竖"，不是"哪个更长"：
    一行是横着走的，所以"一行有多少个格子"用的是 HMI_WIDTH。
*/
#define HMI_WIDTH  320U
#define HMI_HEIGHT 240U

#endif
