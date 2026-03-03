#ifndef _CONFIG_H_
#define _CONFIG_H_
#include "memlayout.h"
// we use two HART (cpu) in challenge3
#define NCPU 2

#define KERNEL_STACK_SIZE 4096
//interval of timer interrupt. added @lab1_3
#define TIMER_INTERVAL 1000000

// the maximum memory space that PKE is allowed to manage. added @lab2_1
#define PKE_MAX_ALLOWABLE_RAM 128 * 1024 * 1024

// the ending physical address that PKE observes. added @lab2_1
#define PHYS_TOP (DRAM_BASE + PKE_MAX_ALLOWABLE_RAM)
//#define USER_STACK 0x81100000
//#define USER_KSTACK 0x81200000
//#define USER_TRAP_FRAME 0x81300000
#endif
