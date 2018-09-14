/**
 * @file
 * @author chu
 * @date 2018/9/4
 */
#pragma once
#include <climits>
#include <unordered_map>

#include <elf++.hh>
#include <dwarf++.hh>
#include <Moe.Core/Exception.hpp>

#ifndef __x86_64__
#error "Unsupported platform"
#endif

namespace lperf
{
    using ProcessId = uint64_t;
    using Word = size_t;

    class Debugger;

    /**
     * @brief 断点
     */
    class Breakpoint
    {
    public:
        Breakpoint(Debugger& dbg, uintptr_t address);
        ~Breakpoint();

    public:
        /**
         * @brief 获取断点地址
         */
        uintptr_t GetAddress()const noexcept { return m_uAddress; }

        /**
         * @brief 获取断点是否生效
         */
        bool IsEnabled()const noexcept { return m_bEnabled; }

        /**
         * @brief 激活断点
         */
        void Enable();

        /**
         * @brief 取消断点
         */
        void Disable();

    private:
        Debugger& m_pDebugger;
        uintptr_t m_uAddress = 0;

        bool m_bEnabled = false;
        uint8_t m_uOriginalByte = 0;
    };

    /**
     * @brief 寄存器
     */
    enum class Registers
    {
        RAX, RBX, RCX, RDX,
        RDI, RSI, RBP, RSP,
        R8, R9, R10, R11,
        R12, R13, R14, R15,
        RIP, EFLAGS, CS,
        ORIG_RAX, FS_BASE,
        GS_BASE,
        FS, GS, SS, DS, ES,
    };

    /**
     * @brief 进程执行状态
     */
    enum class ProcessStatus
    {
        Terminated,
        Running,
        Paused,
    };

    /**
     * @brief 调试器
     */
    class Debugger
    {
    public:
        /**
         * @brief 将调试器挂接到进程上
         * @param pid 进程ID
         * @param interrupt 是否在挂接调试器后立即打断进程（SIGSTOP）
         *
         * 当 interrupt = true 时，会等待直到进程打断。
         */
        Debugger(ProcessId pid, bool interrupt=false);

        /**
         * @brief 析a构函数
         *
         * 当析构时将自动从目标进程DETACH。
         */
        ~Debugger();

    public:
        /**
         * @brief 获取进程状态
         */
        ProcessStatus GetStatus()const noexcept { return m_uStatus; }

        /**
         * @brief 获取退出代码
         */
        int GetExitCode()const noexcept { return m_iExitCode; }

        /**
         * @brief 获取上一次 Wait 方法执行后获取的信号
         */
        int GetLastSignal()const noexcept { return m_iLastSignal; }

        /**
         * @brief 等待事件触发
         * @return 当进程终止返回false，否则返回true。
         */
        bool Wait();

        /**
         * @brief 打断进程执行
         */
        void Interrupt();

        /**
         * @brief 打断进程执行（无异常）
         */
        void InterruptSafe()noexcept;

        /**
         * @brief 继续进程执行
         */
        void Continue();

        /**
         * @brief 继续进程执行（无异常）
         */
        void ContinueSafe()noexcept;

        /**
         * @brief 单步执行
         */
        void SingleStep();

        /**
         * @brief 获取寄存器
         */
        Word GetRegister(Registers reg);

        /**
         * @brief 设置寄存器
         * @param regs 寄存器状态
         */
        void SetRegister(Registers reg, Word val);

        /**
         * @brief 获取指令计数器
         */
        Word GetPC() { return GetRegister(Registers::RIP); }

        /**
         * @brief 设置指令计数器
         * @param pc 计数器
         */
        void SetPC(Word pc) { SetRegister(Registers::RIP, pc); }

        /**
         * @brief 从指定地址读取一个字长的数据
         * @param address 地址
         * @return 读取的数据
         */
        Word Read(uintptr_t address);

        /**
         * @brief 从指定地址读取一个字节的数据
         * @param address 地址
         * @return 读取的数据
         */
        uint8_t ReadByte(uintptr_t address);

        /**
         * @brief 从指定地址读取一个 NULL-TERMINATED 的字符串
         * @param address 地址
         * @param maxlen 最长长度
         * @return 读取的字符串
         */
        std::string ReadString(uintptr_t address, size_t maxlen=1024);

        /**
         * @brief 从指定地址读取若干字节数据
         * @param address 地址
         * @param buffer 缓冲区
         * @param count 读取的数量
         * @return 实际读取的数量
         */
        size_t ReadBytes(uintptr_t address, uint8_t buffer[], size_t count);

        /**
         * @brief 将数据写入指定地址
         * @param address 地址
         * @param data 数据
         */
        void Write(uintptr_t address, Word data);

        /**
         * @brief 将一个字节的数据写入指定地址
         * @param address 地址
         * @param data 数据
         */
        void WriteByte(uintptr_t address, uint8_t data);

        /**
         * @brief 向进程发送信号
         * @param signum 信号
         */
        void SendSignal(int signum);

        /**
         * @brief 创建断点
         * @param address 地址
         * @return 断点对象
         */
        Breakpoint* CreateBreakpoint(uintptr_t address);

        /**
         * @brief 通过函数名创建断点
         * @param func 函数名
         * @param skipPrologue 跳过编译器生成的栈平衡代码
         */
        Breakpoint* CreateBreakpoint(const char* func, bool skipPrologue=true);

        /**
         * @brief 获取指定地址处的断点
         * @param address 地址
         * @return 若有断点则返回非nullptr
         */
        Breakpoint* GetBreakpoint(uintptr_t address);

        /**
         * @brief 检查是否命中断点
         * @return 若命中返回非nullptr
         */
        Breakpoint* IsHitBreakpoint();

        /**
         * @brief 移除断点
         * @param breakpoint 断点
         */
        void RemoveBreakpoint(Breakpoint* breakpoint);

        /**
         * @brief 对于相对定位的映像，获取相对地址偏移
         */
        uintptr_t GetAddressOffset()const noexcept { return m_uAddressOffset; }

        /**
         * @brief 根据地址获取函数名称
         * @param address 地址
         * @return 函数名称
         */
        const std::string& GetFunctionName(uintptr_t address);

    private:
        void GetProcessBaseAddress();
        void InternalStepOver();
        bool StepOverBreakpoint();
        dwarf::line_table::iterator GetLineEntryFromPC(uint64_t pc);

    private:
        ProcessStatus m_uStatus = ProcessStatus::Terminated;
        ProcessId m_uPid = 0;
        int m_iExitCode = 0;
        int m_iLastSignal = 0;

        std::unordered_map<uintptr_t, std::unique_ptr<Breakpoint>> m_stBreakpoints;

        elf::elf m_stElfParser;
        dwarf::dwarf m_stDwarfParser;
        uintptr_t m_uAddressOffset = 0;
        std::unordered_map<uintptr_t, std::string> m_stSymbolCacheMap;
    };
}
