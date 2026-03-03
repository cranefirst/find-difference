/* kernel/sync_utils.h */
#ifndef _SYNC_UTILS_H_
#define _SYNC_UTILS_H_

#include "util/types.h"

typedef struct spinlock_t {
    volatile int locked;
} spinlock_t;

static inline void spinlock_lock(spinlock_t *lock) {
    int tmp = 1;
    while (1) {
        asm volatile (
            "amoswap.w.aq %0, %1, (%2)"
            : "=r"(tmp)
            : "r"(1), "r"(&lock->locked)
            : "memory"
        );
        if (tmp == 0) break;
        // [优化] 如果没拿到锁，先自旋读取，不要一直发原子指令（减少总线压力，虽说Spike无所谓）
        // while (lock->locked) ; 
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    asm volatile (
        "amoswap.w.rl x0, x0, (%0)"
        : 
        : "r"(&lock->locked)
        : "memory"
    );
}

static inline void sync_barrier(volatile int *counter, int all) {
  int local;
  asm volatile("amoadd.w %0, %2, (%1)\n"
               : "=r"(local)
               : "r"(counter), "r"(1)
               : "memory");

  if (local + 1 < all) {
    do {
      asm volatile("lw %0, (%1)\n" : "=r"(local) : "r"(counter) : "memory");
    } while (local < all);
  }
}

#endif