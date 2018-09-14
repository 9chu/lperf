# LUA PERF

本工具用于在运行时导出LUA堆栈，用于产生火焰图。

## 编译

**gcc >= 4.8 && cmake >= 3.1**

```
git clone https://github.com/9chu/lperf
cd lperf
git submodule update --init
mkdir build && cd build && cmake .. && make -j8
```

## 快速上手

```bash
# 以10毫秒间隔采样10000次
./lperf -p PID -i 10 -c 10000 | ./flamegraph.pl > graph.html
```

```bash
# 当调试符号不可用时，手动指定用于获取lua_State*的LUA函数地址
./lperf -p PID -i 10 -c 10000 -k 0x40c64f | ./flamegraph.pl > graph.html
```

## 前置条件

- 只支持LUA 5.3.4的ABI
- 不可用于协程环境/多个lua_State*的环境

## 原理

为了从LUA中获取运行时的堆栈，程序通过`ptrace`来访问被调程序的内存。
首先，通过向`lua_pcallk`设置软件断点，从寄存器中获取`lua_State*`。
然后每隔一段时间取样LUA堆栈。

因此，在没有调试符号的情况下，需要使用`-k`命令行来手动指定一个函数用于插入断点。
这种情况下具备一定风险，请谨慎使用。

## REFERENCE

- https://github.com/uber/pyflame
- https://blog.tartanllama.xyz/writing-a-linux-debugger-breakpoints/
