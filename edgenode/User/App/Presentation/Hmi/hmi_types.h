#ifndef HMI_TYPES_H_
#define HMI_TYPES_H_

/*
    判断标准是"横还是竖"，不是"哪个更长"。
    一行是横着走的，所以"一行有多少个格子"用的是 HMI_WIDTH。
    自检：HMI_WIDTH * 2 必须等于 640，否则就是名字配错了。
*/
#define HMI_WIDTH  320U
#define HMI_HEIGHT 240U

#endif
