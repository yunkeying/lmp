// Copyright 2023 The LMP Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://github.com/linuxkerneltravel/lmp/blob/develop/LICENSE
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// author: jinyufeng2000@gmail.com
//
// 设置断点，为探针提供时间

#ifndef UTRACE_GDB_H
#define UTRACE_GDB_H

#include <stdint.h>     // for uint8_t
#include <sys/types.h>  // for pid_t

/*
 * @brief 记录跨函数传递的信息
 */
struct gdb {
  pid_t pid;          /**< gdb附加到的进程的编号 */
  uint8_t saved_inst; /**< 被int3覆盖的单字节指令 */
};

/*
 * @brief 创建一个gdb结构体
 * @param[in] pid 进程号
 * @return 指向gdb结构体的指针
 * @note 从堆中申请空间
 */
struct gdb *gdb_init(pid_t pid);

/*
 * @brief 设置一个断点
 * @param[in] gdb 指向一个gdb结构体
 * @param[in] addr 虚拟地址
 */
long gdb_enable_breakpoint(struct gdb *gdb, size_t addr);

/*
 * @brief 取消一个断点
 * @param[in] gdb 指向一个gdb结构体
 * @param[in] addr 虚拟地址
 * @note 需要保证之前调用过enable_breakpoint(gdb, pid, addr)
 */
long gdb_disable_breakpoint(const struct gdb *gdb, size_t addr);

/*
 * @brief 继续执行
 * @param[in] gdb 指向一个gdb结构体
 */
long gdb_continue_execution(const struct gdb *gdb);

/*
 * @brief 等待进程收到信号
 * @param[in] gdb 指向一个gdb结构体
 */
long gdb_wait_for_signal(const struct gdb *gdb);

/*
 * @brief detach
 * @param[in] gdb 指向要释放的gdb结构体
 */
long gdb_detach(const struct gdb *gdb);

/*
 * @brief 取消ptrace并释放gdb结构体的空间
 * @param[in] gdb 指向要释放的gdb结构体
 */
void gdb_free(struct gdb *gdb);

#endif  // UTRACE_GDB_H