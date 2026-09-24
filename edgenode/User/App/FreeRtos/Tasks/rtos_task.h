#ifndef RTOS_TASKS_H_
#define RTOS_TASKS_H_

void led_tasks_create(void);
void storage_task_create(void);
void collect_task_create(void);
void transmit_task_create(void);
void stack_monitor_task_create(void);
void hmi_task_create(void);
#endif
