#ifndef BOOT_JUMP_H_
#define BOOT_JUMP_H_

#include <stdint.h>

/*
    调用者必须先确认 vector_address 指向一个有效应用的向量表。
    本函数只负责切换 CPU 启动环境并跳转，不负责镜像校验和槽位选择。
*/
void boot_jump_to_vector(uint32_t vector_address);

#endif