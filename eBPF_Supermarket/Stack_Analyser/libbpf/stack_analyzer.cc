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
// author: luiyanbing@foxmail.com
//
// 用户态bpf的主程序代码，主要用于数据的显示和整理

#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fstream>

#include "rapidjson/document.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/writer.h"
#include "symbol.h" /*符号解析库头文件*/

#ifdef __cplusplus
extern "C"
{
#endif

#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <bpf/bpf.h>
#include <errno.h>
#include <bpf/libbpf.h>
#include <signal.h>

#include "stack_analyzer.h"
#include "bpf/on_cpu_count.skel.h"
#include "bpf/off_cpu_count.skel.h"
#include "bpf/mem_count.skel.h"
#include "bpf/io_count.skel.h"

#ifdef __cplusplus
}
#endif

/// @brief  printing help information
/// @param progname progname printed in the help info
static void show_help(const char *progname)
{
	printf("Usage: %s [-F <frequency>=49] [-p <pid>=-1] [-T <time>=INT_MAX] [-m <0 on cpu|1 off cpu|2 mem|3 io>=0] "
		   "[-U user stack only] [-K kernel stack only] [-f flame graph but not json] [-h help] \n",
		   progname);
}

/// @brief staring perf event
/// @param hw_event attribution of the perf event
/// @param pid the pid to track. 0 for the calling process. -1 for all processes.
/// @param cpu the cpu to track. -1 for all cpu
/// @param group_fd fd of event group leader
/// @param flags setting
/// @return fd of perf event
static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
							unsigned long flags)
{
	return syscall(SYS_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

namespace env
{
	int pid = -1;												  /*pid filter*/
	int cpu = -1;												  /*cpu index*/
	unsigned run_time = __INT_MAX__;							  /*run time*/
	int freq = 49;												  /*simple frequency*/
	MOD mod = MOD_ON_CPU;										  /*mod setting*/
	bool u = true;												  /*user stack setting*/
	bool k = true;												  /*kernel stack setting*/
	bool fla = false;											  /*flame graph instead of json*/
	char *object = (char *)"/usr/lib/x86_64-linux-gnu/libc.so.6"; /*executable binary file for uprobe*/
	static volatile sig_atomic_t exiting;						  /*exiting flag*/
}

class bpf_loader
{
protected:
	int pid;	  // 用于设置ebpf程序跟踪的pid
	int cpu;	  // 用于设置ebpf程序跟踪的cpu
	int err;	  // 用于保存错误代码
	int count_fd; // 栈计数表的文件描述符
	int tgid_fd;  // pid-tgid表的文件描述符
	int comm_fd;  // pid-进程名表的文件描述符
	int trace_fd; // 栈id-栈轨迹表的文件描述符
	bool ustack;  // 是否跟踪用户栈
	bool kstack;  // 是否跟踪内核栈

/// @brief 获取epbf程序中指定表的文件描述符
/// @param name 表的名字
#define OPEN_MAP(name) bpf_map__fd(skel->maps.name)

/// @brief 获取所有表的文件描述符
#define OPEN_ALL_MAP                 \
	count_fd = OPEN_MAP(psid_count); \
	tgid_fd = OPEN_MAP(pid_tgid);    \
	comm_fd = OPEN_MAP(pid_comm);    \
	trace_fd = OPEN_MAP(stack_trace);

/// @brief 加载、初始化参数并打开指定类型的ebpf程序
/// @param name ebpf程序的类型名
/// @param ... 一些ebpf程序全局变量初始化语句
/// @note 失败会使上层函数返回-1
#define LO(name, ...)                              \
	skel = name##_bpf__open();                     \
	CHECK_ERR(!skel, "Fail to open BPF skeleton"); \
	__VA_ARGS__;                                   \
	err = name##_bpf__load(skel);                  \
	CHECK_ERR(err, "Fail to load BPF skeleton");   \
	OPEN_ALL_MAP

/// @class rapidjson::Value
/// @brief 添加字符串常量键和任意值，值可使用内存分配器
/// @param k 设置为键的字符串常量
/// @param ... 对应值，可使用内存分配器
#define CKV(k, ...)                                 \
	AddMember(k,                                    \
			  rapidjson::Value(__VA_ARGS__).Move(), \
			  alc)

/// @class rapidjson::Value
/// @brief 添加需要分配内存的变量字符串键和值，值可使用内存分配器
/// @param k 设置为键的字符串变量
/// @param ... 对应值，可使用内存分配器
#define KV(k, ...)                                  \
	AddMember(rapidjson::Value(k, alc).Move(),      \
			  rapidjson::Value(__VA_ARGS__).Move(), \
			  alc)

/// @class rapidjson::Value::kArray
/// @brief 添加字符串变量
/// @param v 要添加的字符串变量
#define PV(v) PushBack(rapidjson::Value(v, alc), alc)

	struct pksid_count
	{
		int32_t pid, ksid, usid;
		uint64_t count;

		pksid_count(int32_t p, int32_t k, int32_t u, uint64_t c)
		{
			pid = p;
			ksid = k;
			usid = u;
			count = c;
		};

		bool operator<(const pksid_count b) { return count < b.count; };
	};

	std::vector<pksid_count> *sortD()
	{
		if (count_fd < 0)
			return NULL;
		std::vector<pksid_count> *D = new std::vector<pksid_count>();
		__u64 count;
		for (psid prev = {0}, id; !bpf_map_get_next_key(count_fd, &prev, &id); prev = id)
		{
			bpf_map_lookup_elem(count_fd, &id, &count);
			pksid_count d(id.pid, id.ksid, id.usid, count);
			D->insert(std::lower_bound(D->begin(), D->end(), d), d);
		}
		return D;
	}

public:
	bpf_loader(int p = env::pid, int c = env::cpu, bool u = env::u, bool k = env::k) : pid(p), cpu(c), ustack(u), kstack(k)
	{
		count_fd = tgid_fd = comm_fd = trace_fd = -1;
		err = 0;
	};

	/// @brief 负责ebpf程序的加载、参数设置和打开操作
	/// @param  无
	/// @return 成功则返回0，否则返回负数
	virtual int load(void) = 0;

	/// @brief 将ebpf程序挂载到跟踪点上
	/// @param  无
	/// @return 成功则返回0，否则返回负数
	virtual int attach(void) = 0;

	/// @brief 断开ebpf的跟踪点和处理函数间的连接
	/// @param  无
	virtual void detach(void) = 0;

	/// @brief 卸载ebpf程序
	/// @param  无
	virtual void unload(void) = 0;

	/// @brief 将表中的栈数据保存为火焰图
	/// @param  无
	/// @return 表未成功打开则返回负数
	int flame_save(void)
	{
		printf("saving flame...\n");
		CHECK_ERR(count_fd < 0, "count map open failure");
		CHECK_ERR(trace_fd < 0, "trace map open failure");
		CHECK_ERR(comm_fd < 0, "comm map open failure");
		int max_deep = 0;
		for (psid prev = {}, key; !bpf_map_get_next_key(count_fd, &prev, &key); prev = key)
		{
			__u64 ip[MAX_STACKS];
			bpf_map_lookup_elem(trace_fd, &key.usid, ip);
			int deep = 0;
			for (int i = 0; i < MAX_STACKS && ip[i]; i++)
				deep++;
			if (max_deep < deep)
				max_deep = deep;
		}
		std::ostringstream tex("");
		for (psid prev = {}, id; !bpf_map_get_next_key(count_fd, &prev, &id); prev = id)
		{
			std::string line("");
			symbol sym;
			__u64 ip[MAX_STACKS];
			if (id.ksid >= 0)
			{
				bpf_map_lookup_elem(trace_fd, &id.ksid, ip);
				for (auto p : ip)
				{
					if (!p)
						break;
					sym.reset(p);
					if (g_symbol_parser.find_kernel_symbol(sym))
						line = sym.name + ';' + line;
					else
					{
						char a[19];
						sprintf(a, "0x%016llx", p);
						std::string s(a);
						line = s + ';' + line;
						g_symbol_parser.putin_symbol_cache(pid, p, s);
					}
				}
			}
			else
				line = "[MISSING KERNEL STACK];" + line;
			line = std::string("----------------;") + line;
			unsigned deep = 0;
			if (id.usid >= 0)
			{
				bpf_map_lookup_elem(trace_fd, &id.usid, ip);
				std::string *s = 0, symbol;
				elf_file file;
				for (auto p : ip)
				{
					if (!p)
						break;
					sym.reset(p);

					if (g_symbol_parser.find_symbol_in_cache(id.pid, p, symbol))
						s = &symbol;
					else if (g_symbol_parser.get_symbol_info(id.pid, sym, file) &&
							 g_symbol_parser.find_elf_symbol(sym, file, id.pid, id.pid))
					{
						s = &sym.name;
						g_symbol_parser.putin_symbol_cache(id.pid, p, sym.name);
					}
					if (!s)
					{
						char a[19];
						sprintf(a, "0x%016llx", p);
						std::string s(a);
						line = s + ';' + line;
						g_symbol_parser.putin_symbol_cache(pid, p, s);
					}
					else
						line = *s + ';' + line;
					deep++;
				}
			}
			else
			{
				line = std::string("[MISSING USER STACK];") + line;
				deep = 1;
			}
			deep = max_deep - deep;
			for (int i = 0; i < deep; i++)
			{
				line = ".;" + line;
			}
			{
				char cmd[COMM_LEN];
				bpf_map_lookup_elem(comm_fd, &id.pid, cmd);
				line = std::string(cmd) + ':' + std::to_string(id.pid) + ';' + line;
			}
			int count;
			bpf_map_lookup_elem(count_fd, &id, &count);
			line += " " + std::to_string(count) + "\n";
			tex << line;
		}
		std::string tex_s = tex.str();
		FILE *fp = 0;

		fp = fopen("flatex.log", "w");
		CHECK_ERR(!fp, "Failed to save flame text");
		fwrite(tex_s.c_str(), sizeof(char), tex_s.size(), fp);
		fclose(fp);

		fp = popen("flamegraph.pl > flame.svg", "w");
		CHECK_ERR(!fp, "Failed to draw flame graph");
		// fwrite("", 1, 0, fp);
		fwrite(tex_s.c_str(), sizeof(char), tex_s.size(), fp);
		pclose(fp);
		printf("complete\n");
		return 0;
	}

	/// @brief 将表中的栈数据保存为json文件
	/// @param  无
	/// @return 表未成功打开则返回负数
	int data_save(void)
	{
		printf("saving...\n");
		CHECK_ERR(comm_fd < 0, "comm map open failure");
		CHECK_ERR(tgid_fd < 0, "tgid map open failure");
		CHECK_ERR(count_fd < 0, "count map open failure");
		CHECK_ERR(trace_fd < 0, "trace map open failure");
		rapidjson::Document ajson;
		rapidjson::Document::AllocatorType &alc = ajson.GetAllocator();
		ajson.SetObject();

		std::map<int, int> pidtgid_map;
		for (int prev = 0, pid, tgid; !bpf_map_get_next_key(tgid_fd, &prev, &pid); prev = pid)
		{
			bpf_map_lookup_elem(tgid_fd, &pid, &tgid);

			std::string tgid_s = std::to_string(tgid);
			const char *tgid_c = tgid_s.c_str();
			ajson.KV(tgid_c, rapidjson::kObjectType);

			std::string pid_s = std::to_string(pid);
			const char *pid_c = pid_s.c_str();
			ajson[tgid_c].KV(pid_c, rapidjson::kObjectType);

			ajson[tgid_c][pid_c].CKV("stacks", rapidjson::kObjectType);
			pidtgid_map[pid] = tgid;
		}

		comm cmd;
		for (int prev = 0, pid; !bpf_map_get_next_key(comm_fd, &prev, &pid); prev = pid)
		{
			bpf_map_lookup_elem(comm_fd, &pid, &cmd);
			std::string tgid_s = std::to_string(pidtgid_map[pid]);
			std::string pid_s = std::to_string(pid);
			ajson[tgid_s.c_str()][pid_s.c_str()].CKV("name", cmd.str, alc);
		}

		auto D = sortD();
		for (auto id = D->rbegin(); id != D->rend(); ++id)
		{
			rapidjson::Value *trace;
			{
				rapidjson::Value *stacks;
				{
					std::string tgid_s = std::to_string(pidtgid_map[id->pid]);
					std::string pid_s = std::to_string(id->pid);
					stacks = &(ajson[tgid_s.c_str()][pid_s.c_str()]["stacks"]);
				}
				auto sid_c = (std::to_string(id->usid) + "," + std::to_string(id->ksid)).c_str();
				stacks->KV(sid_c, rapidjson::kObjectType);
				(*stacks)[sid_c].CKV("count", id->count);
				(*stacks)[sid_c].CKV("trace", rapidjson::kArrayType);
				trace = &((*stacks)[sid_c]["trace"]);
			}
			// symbolize
			symbol sym;
			__u64 ip[MAX_STACKS];
			if (id->ksid >= 0)
			{
				bpf_map_lookup_elem(trace_fd, &id->ksid, ip);
				for (auto p : ip)
				{
					if (!p)
						break;
					sym.reset(p);
					if (g_symbol_parser.find_kernel_symbol(sym))
					{
						unsigned offset = p - sym.start;
						char offs[20];
						sprintf(offs, "+0x%x", offset);
						std::string s = sym.name + std::string(offs);
						trace->PV(s.c_str());
					}
					else
					{
						char a[19];
						sprintf(a, "0x%016llx", p);
						std::string s(a);
						trace->PV(a);
						g_symbol_parser.putin_symbol_cache(pid, p, s);
					}
				}
			}
			else
				trace->PV("[MISSING KERNEL STACK]");
			trace->PV("----------------");
			if (id->usid >= 0)
			{
				std::string symbol;
				elf_file file;
				bpf_map_lookup_elem(trace_fd, &id->usid, ip);
				for (auto p : ip)
				{
					if (!p)
						break;
					sym.reset(p);
					std::string *s = NULL;
					if (g_symbol_parser.find_symbol_in_cache(id->pid, p, symbol))
						s = &symbol;
					else if (g_symbol_parser.get_symbol_info(id->pid, sym, file) &&
							 g_symbol_parser.find_elf_symbol(sym, file, id->pid, id->pid))
					{
						s = &(sym.name);
						g_symbol_parser.putin_symbol_cache(id->pid, p, sym.name);
					}
					if (!s)
					{
						char a[19];
						sprintf(a, "0x%016llx", p);
						std::string addr_s(a);
						trace->PV(a);
						g_symbol_parser.putin_symbol_cache(id->pid, p, addr_s);
					}
					else
					{
						if (kill(id->pid, 0))
							trace->PV(s->c_str());
						else
						{
							unsigned offset = p - sym.start;
							char offs[20];
							sprintf(offs, " +0x%x", offset);
							*s = *s + std::string(offs);
							trace->PV(s->c_str());
						}
					}
				}
			}
			else
				trace->PV("[MISSING USER STACK]");
		}
		delete D;

		FILE *fp = fopen("stack_count.json", "w");
		char writeBuffer[65536];
		rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
		rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
		ajson.Accept(writer);
		fclose(fp);
		return 0;
	};

	/// @brief 每隔5s输出计数表中的栈及数量
	/// @param time 输出的持续时间
	/// @return 返回被强制退出时的剩余时间，计数表未打开则返回-1
	int count_log(int time)
	{
		CHECK_ERR(count_fd < 0, "count map open failure");
		/*for traverse map*/
		for (; !env::exiting && time > 0 && (pid < 0 || !kill(pid, 0)); time -= 5)
		{
			printf("---------%d---------\n", count_fd);
			sleep(5);
			auto D = sortD();
			for (auto d : *D)
			{
				printf("%6d\t(%6d,%6d)\t%-6lu\n", d.pid, d.ksid, d.usid, d.count);
			}
			delete D;
		};
		return time;
	};

	/// @brief 一个执行ebpf程序的总流程
	/// @param  无
	/// @return 成功则返回0，失败返回负数
	int test(void)
	{
		do
		{
			err = load();
			if (err)
				break;
			err = attach();
			if (err)
				break;
			count_log(env::run_time);
		} while (false);
		detach();
		if (env::fla)
			err = flame_save();
		else
			err = data_save();
		// unload();
		return err;
	};
};

class on_cpu_loader : public bpf_loader
{
protected:
	int pefd;
	unsigned long long freq;
	struct perf_event_attr attr;
	struct on_cpu_count_bpf *skel;

public:
	on_cpu_loader(int p = env::pid, int c = env::cpu, bool u = env::u, bool k = env::k, unsigned long long f = env::freq) : bpf_loader(p, c, u, k), freq(f)
	{
		pefd = -1;
		attr = {
			.type = PERF_TYPE_SOFTWARE, // hardware event can't be used
			.size = sizeof(attr),
			.config = PERF_COUNT_SW_CPU_CLOCK,
			.sample_freq = freq,
			.freq = 1, // use freq instead of period
		};
		skel = 0;
	};
	int load(void) override
	{
		LO(on_cpu_count,
		   skel->bss->u = ustack,
		   skel->bss->k = kstack)
		return 0;
	};
	int attach(void) override
	{
		pefd = perf_event_open(&attr, pid, -1, -1, PERF_FLAG_FD_CLOEXEC); // don't track child process
		CHECK_ERR(pefd < 0, "Fail to set up performance monitor on a CPU/Core");
		skel->links.do_stack = bpf_program__attach_perf_event(skel->progs.do_stack, pefd);
		CHECK_ERR(!(skel->links.do_stack), "Fail to attach bpf");
		return 0;
	}
	void detach(void) override
	{
		if (skel->links.do_stack)
			bpf_link__destroy(skel->links.do_stack);
		if (pefd)
			close(pefd);
	}
	void unload(void) override
	{
		if (skel)
			on_cpu_count_bpf__destroy(skel);
		skel = 0;
	}
};

class off_cpu_loader : public bpf_loader
{
protected:
	struct off_cpu_count_bpf *skel;

public:
	off_cpu_loader(int p = env::pid, int c = env::cpu, bool u = env::u, bool k = env::k) : bpf_loader(p, c, u, k)
	{
		skel = 0;
	};
	int load(void) override
	{
		LO(off_cpu_count,
		   skel->bss->apid = pid,
		   skel->bss->u = ustack,
		   skel->bss->k = kstack)
		return 0;
	};
	int attach(void) override
	{
		err = bpf_attach(off_cpu_count, skel);
		CHECK_ERR(err, "Failed to attach BPF skeleton");
		return 0;
	};
	void detach(void) override
	{
		if (skel)
			off_cpu_count_bpf__detach(skel);
	};
	void unload(void) override
	{
		if (skel)
			off_cpu_count_bpf__destroy(skel);
		skel = 0;
	};
};

class mem_loader : public bpf_loader
{
protected:
	struct mem_count_bpf *skel;
	char *object;

public:
	mem_loader(int p = env::pid, int c = env::cpu, bool u = env::u, bool k = env::k, char *e = env::object) : bpf_loader(p, c, u, k), object(e)
	{
		skel = 0;
	};
	int load(void) override
	{
		LO(mem_count,
		   skel->bss->u = ustack,
		//    skel->bss->k = kstack,
		   skel->bss->apid = pid)
		return 0;
	};
	int attach(void) override
	{
		ATTACH_UPROBE_CHECKED(skel, malloc, malloc_enter);
		ATTACH_URETPROBE_CHECKED(skel, malloc, malloc_exit);
		ATTACH_UPROBE_CHECKED(skel, calloc, calloc_enter);
		ATTACH_URETPROBE_CHECKED(skel, calloc, calloc_exit);
		ATTACH_UPROBE_CHECKED(skel, realloc, realloc_enter);
		ATTACH_URETPROBE_CHECKED(skel, realloc, realloc_exit);
		ATTACH_UPROBE_CHECKED(skel, free, free_enter);

		ATTACH_UPROBE_CHECKED(skel, mmap, mmap_enter);
		ATTACH_URETPROBE_CHECKED(skel, mmap, mmap_exit);
		ATTACH_UPROBE_CHECKED(skel, munmap, munmap_enter);

		err = mem_count_bpf__attach(skel);
		CHECK_ERR(err, "Failed to attach BPF skeleton");
		return 0;
	};
	void detach(void) override
	{
		if (skel->links.free_enter)
			bpf_link__destroy(skel->links.free_enter);
		if (skel->links.malloc_exit)
			bpf_link__destroy(skel->links.malloc_exit);
		if (skel->links.malloc_enter)
			bpf_link__destroy(skel->links.malloc_enter);
	};
	void unload(void) override
	{
		if (skel)
			mem_count_bpf__destroy(skel);
		skel = 0;
	};
};

class io_loader : public bpf_loader
{
protected:
	struct io_count_bpf *skel;

public:
	io_loader(int p = env::pid, int c = env::cpu, bool u = env::u, bool k = env::k) : bpf_loader(p, c, u, k)
	{
		skel = 0;
	};
	int load(void) override
	{
		LO(io_count, {
			skel->bss->apid = pid;
			skel->bss->u = ustack;
			skel->bss->k = kstack;
		})
		return 0;
	};
	int attach(void) override
	{
		err = bpf_attach(io_count, skel);
		CHECK_ERR(err, "Failed to attach BPF skeleton");
		return 0;
	};
	void detach(void) override
	{
		if (skel)
			io_count_bpf__detach(skel);
	};
	void unload(void) override
	{
		if (skel)
			io_count_bpf__destroy(skel);
		skel = 0;
	};
};

typedef bpf_loader *(*bpf_load)();

void __handler(int)
{
	env::exiting = 1;
}

int main(int argc, char *argv[])
{
	char argp;
	while ((argp = getopt(argc, argv, "hF:p:T:m:UKf")) != -1) // parsing arguments
	{
		switch (argp)
		{
		case 'F':
			env::freq = atoi(optarg);
			if (env::freq < 1)
				env::freq = 1;
			break;
		case 'p':
			env::pid = atoi(optarg);
			if (env::pid < 1)
				env::pid = -1;
			break;
		case 'f':
			env::fla = true;
			break;
		case 'T':
			env::run_time = atoi(optarg);
			break;
		case 'm':
			env::mod = (MOD)atoi(optarg);
			break;
		case 'U':
			env::k = 0; // do not track kernel stack
			break;
		case 'K':
			env::u = 0; // do not track user stack
			break;
		case 'h':
		default:
			show_help(argv[0]);
			return 0;
		}
	}
	bpf_load arr[] = {
		[]() -> bpf_loader *
		{ return new on_cpu_loader(); },
		[]() -> bpf_loader *
		{ return new off_cpu_loader(); },
		[]() -> bpf_loader *
		{ return new mem_loader(); },
		[]() -> bpf_loader *
		{ return new io_loader(); },
	};
	CHECK_ERR(signal(SIGINT, __handler) == SIG_ERR, "can't set signal handler");
	return arr[env::mod]()->test();
}