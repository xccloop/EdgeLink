#include "boot_jump.h"
#include <stdint.h>
#include "gd32f10x.h"

typedef void (*app_reset_handler_t)(void); //app_reset_handler_t 是一种指针，这种指针指向“没有参数、没有返回值”的函数。

/*
    这里 naked 表示编译器不生成函数入栈和出栈代码；ARM 调用约定会把前两个参数分别放进 R0、R1。
    noreturn告诉编译器：这个函数永远不会返回。因为最后的 bx r1 会直接跳到 APP 的 Reset_Handler，APP 永远不会回到 Bootloader。
    第一句话：这句话的意思是把 r0 里的值（也就是我们传入的 new_msp，APP 的栈顶地址）写入 MSP（主栈指针）寄存器。这就是前面说的“告诉 CPU 栈在哪里”。
    第二句话：转到 r1 里存放的地址（也就是 reset_handler，APP 的 Reset_Handler 地址）去执行。
    所以这一个函数的意思就是切换栈指针 MSP，然后跳转到 APP 的 Reset_Handler 去执行。
    为什么不用c语言写呢，或者说为什么不直接app_resethandler()呢？
    这个函数是在我们设定号app_msp之后运行的，所以我们不希望后面的函数会碰栈，但 C 编译器生成的代码，处处都在碰栈。
    一旦 MSP 被切换，当前函数就不能再碰栈了。
    但是我们又需要跳转，所以这里采用汇编的方式进行
*/
/*
    两个参数标 __attribute__((unused)) 是为了压掉 -Wunused-parameter：
    它们的值由 ARM 调用约定直接放进 r0 / r1，只被下面的汇编消费，
    GCC 在 C 层面看不见引用，就以为没用到。

    不能用 "(void)new_msp;" 那种写法 —— naked 函数里不允许出现汇编以外的
    语句（GCC 会直接报 "non-ASM statement in naked function"）。
    加这个属性只影响警告，不改变调用约定，也不生成任何指令。
*/
__attribute__((naked, noreturn))
static void boot_jump_asm(uint32_t new_msp __attribute__((unused)),
                          uint32_t reset_handler __attribute__((unused)))
{
    __asm volatile(
        "msr msp, r0\n"
        /*
            在跳过去之前，把中断总开关打开（cpsie i 清 PRIMASK）。

            为什么必须在这里补这一句：
              本函数上面的 boot_jump_to_vector() 调过 __disable_irq()，
              那会把 PRIMASK 置 1，屏蔽【所有】可屏蔽中断。
              而 PRIMASK 是 CPU 级开关，跳转不会把它复位 ——
              如果我们不管，应用就会全程在"全局关中断"的状态下跑。

              实测后果：应用初始化里凡是依赖 SysTick 中断的 delay_ms()
              都会死等（board_systick_ms 永远不涨）。应用要等到
              vTaskStartScheduler() 才重新开中断，但那时早就卡死了。

            为什么放在这里安全：
              此刻 VTOR 已经指向应用的向量表，NVIC 的使能和挂起位
              都被 boot_jump_to_vector() 清过，SysTick 也已经关掉 ——
              从 cpsie i 到 bx r1 之间没有任何中断源能触发。

              真实的上电复位里 PRIMASK 本来就是 0，这一句只是把
              CPU 恢复到应用有权期待的那个状态。
        */
        "cpsie i\n"
        "bx r1\n"
    );
}

/*
    Bootloader 已决定启动 B
            ↓
    传入 B 的向量表地址：0x08021900
            ↓
    1. 读取 B 向量表前两个数
    ├─ 第 1 个：B 应用的栈顶地址 app_msp
    └─ 第 2 个：B 应用的 Reset_Handler 地址 app_reset_handler
            ↓
    2. 停止 Bootloader 的中断环境
    ├─ 关闭全局中断
    ├─ 关闭 SysTick
    └─ 清理可能残留的中断状态
            ↓
    3. 告诉 Cortex-M3：以后中断向量表改用 B 的
    └─ SCB->VTOR = 0x08021900
            ↓
    4. 切换主栈指针
    └─ MSP = app_msp
            ↓
    5. 跳转到 app_reset_handler
            ↓
    B 应用的 Reset_Handler
            ↓
    B 应用自己的 SystemInit()
            ↓
    复制 .data、清零 .bss
            ↓
    进入 B 应用的 main()
*/
void boot_jump_to_vector(uint32_t vector_address)
{
    /*
    * 这段代码只干一件事：
    * 从对应分区的向量表里，读出 MSP 和 Reset_Handler 这两项。
    * vector_address 是传入的“向量表地址”，
    * 也就是分区首地址跳过镜像头之后的地址（例如 OTA_SLOT_A_VECTOR_ADDRESS）。
    * 向量表开头就是完整中断向量表：
    *   +0  第 0 项：MSP
    *   +4  第 1 项：Reset_Handler
    * 两项相差 4 字节，所以按 uint32_t 划分：
    *   vector_table[0] = MSP
    *   vector_table[1] = Reset_Handler
    * MSP 不是拿来跳的，它是初始主栈指针，
    * 复位时会被硬件装进 SP 寄存器，告诉 CPU 栈在哪里。
    * Reset_Handler 才是拿来跳的，它会被装进 PC 寄存器并开始执行。
    * 为了跳转，需要一种“没有返回值、没有形参”的函数指针类型：
    *   typedef void (*app_reset_handler_t)(void);
    * 注意：这是函数指针类型，不是指针函数。
    *   函数指针：本质是指针，指向函数。
    *   指针函数：本质是函数，返回指针。
    * vector_table[1] 读出来本来是 uint32_t 类型的地址值，
    * 强转成 app_reset_handler_t 后，类型变成函数指针。
    * 值不变，但编译器现在允许我们调用它：
    *   app_reset_handler();
    * 调用它就等于跳到 APP 的 Reset_Handler 开始执行。
    */
    const volatile uint32_t *vector_table;
    uint32_t app_msp;
    app_reset_handler_t app_reset_handler;
    vector_table = (const volatile uint32_t*)vector_address;
    app_msp = vector_table[0];
    app_reset_handler = (app_reset_handler_t)vector_table[1]; 

    __disable_irq();//我们会先换中断向量表、再换 MSP。如果此时 Bootloader 的某个中断突然触发，CPU 可能处于“向量表属于应用、栈却还属于 Bootloader”的混合状态，所以先禁止中断。

    /*
        这是用于清除bootloader运行期间的systick计数，后续APP进行systick_init的时候才真正开始计数
        CTRL = 0U：停止 SysTick，并关闭它的中断请求。
        LOAD = 0U：清掉下次重装载的计数值。
        VAL = 0U：清掉当前已经数到哪一步，避免残留计数。
    */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /*
        ICER：Interrupt Clear Enable Register，关闭对应的一组外设中断。
        ICPR：Interrupt Clear Pending Register，清除已经挂起、等待处理的中断。
        全局关中断只是不让 CPU 立刻响应；某个中断可能已经处于“已使能”或“等待处理”状态。应用随后重新开中断时，旧中断可能会立刻进入应用。
    */
    for (uint32_t i = 0U; i < 8U; ++i)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /*
        SCB->VTOR = vector_address;：把 Cortex-M3 的中断向量表基址改为 A 或 B 应用的向量表。
        __DSB();：确保这次寄存器写入已经完成。
        __ISB();：让 CPU 丢弃之前按旧配置预取的指令，后续按新向量表配置继续执行。
        这几句话就是切换中断向量表
    */
    SCB->VTOR = vector_address;
    __DSB();
    __ISB();

    //设置栈顶指针然后跳转到 APP 的 Reset_Handler 
    boot_jump_asm(app_msp, (uint32_t)app_reset_handler);
}

/*
    应用的 Reset_Handler 会调用 SystemInit()，
    而当前 [system_gd32f10x.c (line 45)] 
    把 VECT_TAB_OFFSET 固定为 0x00。因此应用启动后会把 VTOR 从 A/B 地址重设为 0x08000000；首次外设中断就会跳到 Bootloader 的向量表。
    所以接下来先让它支持“由构建目标指定向量表偏移”。
    具体操作如下：
    原本：
        #define VECT_TAB_OFFSET  (uint32_t)0x00
    修改后：
        #ifndef VECT_TAB_OFFSET
        #define VECT_TAB_OFFSET  ((uint32_t)0x00)
        #endif
    这句话意思就是：
    现有普通应用和 Bootloader 没有传入宏时，仍使用 0x00。
    以后构建 A 槽时传入 -DVECT_TAB_OFFSET=0x4100U。
    以后构建 B 槽时传入 -DVECT_TAB_OFFSET=0x21900U。
    实际上就是调用我们现在的boot_jump
    这句话就是我们所说的传入SCB->VTOR = vector_address;
*/