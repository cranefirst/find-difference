/* kernel/process.c */
#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

process* currents[NCPU]; 

// 静态资源池
static process procs[NCPU];

// [修改] 极简版分配函数，只负责返回结构体指针
// 具体的内存分配和映射交给 kernel.c 中的 load_user_program
process* alloc_process() {
  uint64 hartid = read_tp();
  return &procs[hartid];
}

void switch_to(process* proc) {
  assert(proc);
  current = proc;

  write_csr(stvec, (uint64)smode_trap_vector);
  proc->trapframe->kernel_sp = proc->kstack;
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;
  proc->trapframe->kernel_satp = read_csr(satp);

  vm_alloc_stage[read_tp()] = 1;

  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP; 
  x |= SSTATUS_SPIE; 
  write_csr(sstatus, x);

  write_csr(sepc, proc->trapframe->epc);
  write_csr(sscratch, (uint64)proc->trapframe);

  uint64 user_satp = MAKE_SATP(proc->pagetable);
  return_to_user(proc->trapframe, user_satp);
}