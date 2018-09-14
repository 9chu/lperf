/**
 * @file
 * @author chu
 * @date 2018/9/8
 */
#pragma once
#include <memory>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstddef>

#include <Moe.Core/ArrayView.hpp>
#include <Moe.Core/Exception.hpp>

namespace lperf
{
    /**
     * @brief 内存访问抽象
     */
    template <size_t Align = sizeof(size_t)>
    class MemoryAccessorBase
    {
    private:
        static constexpr size_t RoundUp(size_t n)noexcept
        {
            return (n + Align - 1) & ~(Align - 1);
        }

        static constexpr size_t RoundDown(size_t n)noexcept
        {
            return n & ~(Align - 1);
        }

    public:
        /**
         * @brief 读取内存
         * @param address 地址（要求对其到Align）
         * @param output 输出（要求大小必须为Align倍数）
         */
        virtual void Read(uintptr_t address, moe::MutableBytesView output) = 0;

        /**
         * @brief 读取C字符串
         * @param address 地址
         * @param maxlen 最大长度
         * @return 输出
         */
        virtual std::string ReadString(uintptr_t address, size_t maxlen=512) = 0;

        /**
         * @brief 读取结构体
         * @tparam T 类型
         * @param out 输出
         * @param address 地址
         */
        template <typename T>
        void Read(T& out, uintptr_t address)
        {
            auto lowBound = RoundDown(address);
            assert(lowBound <= address);
            auto highBound = RoundUp(address + sizeof(out));
            assert(highBound >= address + sizeof(out));
            m_stBuffer.resize(highBound - lowBound);
            assert(m_stBuffer.size() >= sizeof(out));

            Read(lowBound, moe::MutableBytesView(m_stBuffer.data(), m_stBuffer.size()));
            ::memset(&out, 0, sizeof(out));
            ::memcpy(&out, m_stBuffer.data() + (address - lowBound), sizeof(out));
        }

    private:
        std::vector<uint8_t> m_stBuffer;
    };

    using MemoryAccessorPtr = std::shared_ptr<MemoryAccessorBase<>>;

    MemoryAccessorPtr GetGlobalMemoryAccessor()noexcept;
    void SetGlobalMemoryAccessor(MemoryAccessorPtr ptr);

    template <typename T>
    struct RemotePtr
    {
        T* pointer;

        operator bool()const { return pointer != nullptr; }

        T operator*()const
        {
            T ret;
            Read(ret);
            return ret;
        }

        bool operator==(std::nullptr_t)const noexcept { return pointer == nullptr; }
        bool operator!=(std::nullptr_t)const noexcept { return pointer != nullptr; }
        bool operator==(const RemotePtr& rhs)const noexcept { return pointer == rhs.pointer; }
        bool operator!=(const RemotePtr& rhs)const noexcept { return pointer != rhs.pointer; }
        bool operator==(T* rhs)const noexcept { return pointer == rhs; }
        bool operator!=(T* rhs)const noexcept { return pointer != rhs; }
        bool operator==(uintptr_t rhs)const noexcept { return reinterpret_cast<uintptr_t>(pointer) == rhs; }
        bool operator!=(uintptr_t rhs)const noexcept { return reinterpret_cast<uintptr_t>(pointer) != rhs; }

        void Read(T& out)const
        {
            auto accessor = GetGlobalMemoryAccessor();
            if (!accessor)
                MOE_THROW(moe::InvalidCallException, "Memory accessor not set");
            if (!pointer)
                MOE_THROW(moe::InvalidCallException, "Object pointer is null");

            accessor->Read(out, reinterpret_cast<uintptr_t>(pointer));
        }

        template <typename P>
        RemotePtr<P> CastTo()const
        {
            return RemotePtr<P> { reinterpret_cast<P*>(pointer) };
        }

        std::string ToString()const
        {
            return moe::StringUtils::Format("0x{0,16[0]:H}", reinterpret_cast<size_t>(pointer));
        }
    };

    namespace LuaObjects
    {
        enum TMS
        {
            TM_INDEX,
            TM_NEWINDEX,
            TM_GC,
            TM_MODE,
            TM_LEN,
            TM_EQ,  /* last tag method with fast access */
            TM_ADD,
            TM_SUB,
            TM_MUL,
            TM_MOD,
            TM_POW,
            TM_DIV,
            TM_IDIV,
            TM_BAND,
            TM_BOR,
            TM_BXOR,
            TM_SHL,
            TM_SHR,
            TM_UNM,
            TM_BNOT,
            TM_LT,
            TM_LE,
            TM_CONCAT,
            TM_CALL,
            TM_N		/* number of elements in the enum */
        };

        static const unsigned LUA_NUMTAGS = 9;
        static const unsigned STRCACHE_N = 53;
        static const unsigned STRCACHE_M = 2;

        static const unsigned LUA_TNIL = 0;
        static const unsigned LUA_TBOOLEAN = 1;
        static const unsigned LUA_TLIGHTUSERDATA = 2;
        static const unsigned LUA_TNUMBER = 3;
        static const unsigned LUA_TSTRING = 4;
        static const unsigned LUA_TTABLE = 5;
        static const unsigned LUA_TFUNCTION = 6;
        static const unsigned LUA_TUSERDATA = 7;
        static const unsigned LUA_TTHREAD = 8;
        static const unsigned LUA_TSHRSTR = (LUA_TSTRING | (0 << 4));  /* short strings */
        static const unsigned LUA_TLNGSTR = (LUA_TSTRING | (1 << 4));  /* long strings */
        static const unsigned LUA_TNUMFLT = (LUA_TNUMBER | (0 << 4));  /* float numbers */
        static const unsigned LUA_TNUMINT = (LUA_TNUMBER | (1 << 4));  /* integer numbers */
        static const unsigned LUA_TLCL = (LUA_TFUNCTION | (0 << 4));  /* Lua closure */
        static const unsigned LUA_TLCF = (LUA_TFUNCTION | (1 << 4));  /* light C function */
        static const unsigned LUA_TCCL = (LUA_TFUNCTION | (2 << 4));  /* C closure */

        constexpr unsigned MarkAsCollectableType(unsigned t) { return t | (1 << 6); }

        static const unsigned CIST_LUA = 1 << 1;
        static const unsigned CIST_HOOKED = 1 << 2;  /* call is running a debug hook */
        static const unsigned CIST_TAIL = 1 << 5;  /* call was tail called */
        static const unsigned CIST_FIN = 1 << 8;  /* call is running a finalizer */

        using lu_byte = uint8_t;
        using lua_Number = double;
        using lua_Integer = int64_t;
        using lua_KContext = ptrdiff_t;

        struct lua_State;
        struct lua_Debug;

        using lua_CFunction = int(*)(lua_State*);
        using lua_KFunction = int(*)(lua_State*, int, lua_KContext);
        using lua_Alloc = void*(*)(void*, void*, size_t, size_t);
        using lua_Hook = void(*)(lua_State*, lua_Debug*);

        struct GCObject
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;
        };

        union Value
        {
            RemotePtr<GCObject> gc;    /* collectable objects */
            void* p;         /* light userdata */
            int b;           /* booleans */
            lua_CFunction f; /* light C functions */
            lua_Integer i;   /* integer numbers */
            lua_Number n;    /* float numbers */
        };

        struct lua_TValue
        {
            Value value_;
            int tt_;

            int GetTypeTag()const noexcept { return tt_ & 0x3F; }
            int GetTypeTagNoVariant()const noexcept { return tt_ & 0xF; }
            bool IsNumber()const noexcept { return GetTypeTagNoVariant() == LUA_TNUMBER; }
            bool IsFloat()const noexcept { return GetTypeTag() == LUA_TNUMFLT; }
            bool IsInteger()const noexcept { return GetTypeTag() == LUA_TNUMINT; }
            bool IsNil()const noexcept { return GetTypeTag() == LUA_TNIL; }
            bool IsBoolean()const noexcept { return GetTypeTag() == LUA_TBOOLEAN; }
            bool IsLightUserData()const noexcept { return GetTypeTag() == LUA_TLIGHTUSERDATA; }
            bool IsString()const noexcept { return GetTypeTagNoVariant() == LUA_TSTRING; }
            bool IsShrString()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TSHRSTR); }
            bool IsLngString()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TLNGSTR); }
            bool IsTable()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TTABLE); }
            bool IsFunction()const noexcept { return GetTypeTagNoVariant() == LUA_TFUNCTION; }
            bool IsClosure()const noexcept { return (tt_ & 0x1F) == LUA_TFUNCTION; }
            bool IsCClosure()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TCCL); }
            bool IsLClosure()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TLCL); }
            bool IsLightCFunction()const noexcept { return GetTypeTag() == LUA_TLCF; }
            bool IsFullUserData()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TUSERDATA); }
            bool IsThread()const noexcept { return GetTypeTag() == MarkAsCollectableType(LUA_TTHREAD); }
            //bool IsDeadKey()const noexcept { return GetTypeTag() == LUA_TDEADKEY; }
        };

        using TValue = lua_TValue;
        using StkId = RemotePtr<TValue>;
        using Instruction = uint32_t;
        using l_mem = ptrdiff_t;
        using lu_mem = size_t;
        using l_signalT = sig_atomic_t;

        struct lua_longjmp;

        struct TString
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte extra;  /* reserved words for short strings; "has hash" for longs */
            lu_byte shrlen;  /* length for short strings */
            unsigned int hash;
            union {
                size_t lnglen;  /* length for long strings */
                RemotePtr<TString> hnext;  /* linked list for hash table */
            } u;
        };

        union L_Umaxalign
        {
            lua_Number n;
            double u;
            void *s;
            lua_Integer i;
            long l;
        };

        union UTString
        {
            L_Umaxalign dummy;  /* ensures maximum alignment for strings */
            TString tsv;
        };

        struct stringtable
        {
            RemotePtr<RemotePtr<TString>> hash;
            int nuse;  /* number of elements */
            int size;
        };

        union TKey
        {
            struct {
                Value value_;
                int tt_;
                int next;  /* for chaining (offset for next node) */
            } nk;
            TValue tvk;
        };

        struct Node
        {
            TValue i_val;
            TKey i_key;
        };

        struct Table
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte flags;  /* 1<<p means tagmethod(p) is not present */
            lu_byte lsizenode;  /* log2 of size of 'node' array */
            unsigned int sizearray;  /* size of 'array' array */
            RemotePtr<TValue> array;  /* array part */
            RemotePtr<Node> node;
            RemotePtr<Node> lastfree;  /* any free position is before this position */
            RemotePtr<Table> metatable;
            RemotePtr<GCObject> gclist;
        };

        struct Udata
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte ttuv_;  /* user value's tag */
            RemotePtr<Table> metatable;
            size_t len;  /* number of bytes */
            Value user_;  /* user value */
        };

        struct CClosure
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte nupvalues;
            RemotePtr<GCObject> gclist;

            lua_CFunction f;
            TValue upvalue[1];  /* list of upvalues */
        };

        struct UpVal
        {
            RemotePtr<TValue> v;  /* points to stack or to its own value */
            lu_mem refcount;  /* reference counter */
            union {
                struct {  /* (when open) */
                    RemotePtr<UpVal> next;  /* linked list */
                    int touched;  /* mark to avoid cycles with dead threads */
                } open;
                TValue value;  /* the value (when closed) */
            } u;
        };

        struct Proto;
        struct LClosure
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte nupvalues;
            RemotePtr<GCObject> gclist;

            RemotePtr<Proto> p;
            RemotePtr<UpVal> upvals[1];  /* list of upvalues */
        };

        union Closure
        {
            CClosure c;
            LClosure l;
        };

        struct Upvaldesc
        {
            RemotePtr<TString> name;  /* upvalue name (for debug information) */
            lu_byte instack;  /* whether it is in stack (register) */
            lu_byte idx;  /* index of upvalue (in stack or in outer function's list) */
        };

        struct LocVar
        {
            RemotePtr<TString> varname;
            int startpc;  /* first point where variable is active */
            int endpc;    /* first point where variable is dead */
        };

        struct Proto
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            lu_byte numparams;  /* number of fixed parameters */
            lu_byte is_vararg;
            lu_byte maxstacksize;  /* number of registers needed by this function */
            int sizeupvalues;  /* size of 'upvalues' */
            int sizek;  /* size of 'k' */
            int sizecode;
            int sizelineinfo;
            int sizep;  /* size of 'p' */
            int sizelocvars;
            int linedefined;  /* debug information  */
            int lastlinedefined;  /* debug information  */
            RemotePtr<TValue> k;  /* constants used by the function */
            RemotePtr<Instruction> code;  /* opcodes */
            RemotePtr<RemotePtr<Proto>> p;  /* functions defined inside the function */
            RemotePtr<int> lineinfo;  /* map from opcodes to source lines (debug information) */
            RemotePtr<LocVar> locvars;  /* information about local variables (debug information) */
            RemotePtr<Upvaldesc> upvalues;  /* upvalue information */
            RemotePtr<LClosure> cache;  /* last-created closure with this prototype */
            RemotePtr<TString>  source;  /* used for debug information */
            RemotePtr<GCObject> gclist;
        };

        struct global_State
        {
            lua_Alloc frealloc;  /* function to reallocate memory */
            void* ud;         /* auxiliary data to 'frealloc' */
            l_mem totalbytes;  /* number of bytes currently allocated - GCdebt */
            l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
            lu_mem GCmemtrav;  /* memory traversed by the GC */
            lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */
            stringtable strt;  /* hash table for strings */
            TValue l_registry;
            unsigned int seed;  /* randomized seed for hashes */
            lu_byte currentwhite;
            lu_byte gcstate;  /* state of garbage collector */
            lu_byte gckind;  /* kind of GC running */
            lu_byte gcrunning;  /* true if GC is running */
            RemotePtr<GCObject> allgc;  /* list of all collectable objects */
            RemotePtr<RemotePtr<GCObject>> sweepgc;  /* current position of sweep in list */
            RemotePtr<GCObject> finobj;  /* list of collectable objects with finalizers */
            RemotePtr<GCObject> gray;  /* list of gray objects */
            RemotePtr<GCObject> grayagain;  /* list of objects to be traversed atomically */
            RemotePtr<GCObject> weak;  /* list of tables with weak values */
            RemotePtr<GCObject> ephemeron;  /* list of ephemeron tables (weak keys) */
            RemotePtr<GCObject> allweak;  /* list of all-weak tables */
            RemotePtr<GCObject> tobefnz;  /* list of userdata to be GC */
            RemotePtr<GCObject> fixedgc;  /* list of objects not to be collected */
            RemotePtr<lua_State> twups;  /* list of threads with open upvalues */
            unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
            int gcpause;  /* size of pause between successive GCs */
            int gcstepmul;  /* GC 'granularity' */
            lua_CFunction panic;  /* to be called in unprotected errors */
            RemotePtr<lua_State> mainthread;
            RemotePtr<lua_Number> version;  /* pointer to version number */
            RemotePtr<TString> memerrmsg;  /* memory-error message */
            RemotePtr<TString> tmname[TM_N];  /* array with tag-method names */
            RemotePtr<Table> mt[LUA_NUMTAGS];  /* metatables for basic types */
            RemotePtr<TString> strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
        };

        struct CallInfo
        {
            StkId func;  /* function index in the stack */
            StkId top;  /* top for this function */
            RemotePtr<CallInfo> previous;  /* dynamic call link */
            RemotePtr<CallInfo> next;
            union {
                struct {  /* only for Lua functions */
                    StkId base;  /* base for this function */
                    RemotePtr<Instruction> savedpc;
                } l;
                struct {  /* only for C functions */
                    lua_KFunction k;  /* continuation in case of yields */
                    ptrdiff_t old_errfunc;
                    lua_KContext ctx;  /* context info. in case of yields */
                } c;
            } u;
            ptrdiff_t extra;
            short nresults;  /* expected number of results from this function */
            unsigned short callstatus;

            bool IsLua() { return (callstatus & CIST_LUA) != 0; }
            bool IsHooked() { return (callstatus & CIST_HOOKED) != 0; }
            bool IsTailCall() { return (callstatus & CIST_TAIL) != 0; }
            bool IsFinalizer() { return (callstatus & CIST_FIN) != 0 ; }
        };

        static const unsigned LUA_IDSIZE = 60;

        struct lua_Debug
        {
            int event;
            std::string name;	/* (n) */
            const char* namewhat;	/* (n) 'global', 'local', 'field', 'method' */
            const char* what;	/* (S) 'Lua', 'C', 'main', 'tail' */
            std::string source;	/* (S) */
            int currentline;	/* (l) */
            int linedefined;	/* (S) */
            int lastlinedefined;	/* (S) */
            unsigned char nups;	/* (u) number of upvalues */
            unsigned char nparams;/* (u) number of parameters */
            bool isvararg;        /* (u) */
            bool istailcall;	/* (t) */
            char short_src[LUA_IDSIZE]; /* (S) */
            uintptr_t address;
            /* private part */
            RemotePtr<CallInfo> i_ci;  /* active function */
        };

        struct lua_State
        {
            RemotePtr<GCObject> next;
            lu_byte tt;
            lu_byte marked;

            unsigned short nci;  /* number of items in 'ci' list */
            lu_byte status;
            StkId top;  /* first free slot in the stack */
            RemotePtr<global_State> l_G;
            RemotePtr<CallInfo> ci;  /* call info for current function */
            RemotePtr<Instruction> oldpc;  /* last pc traced */
            StkId stack_last;  /* last free slot in the stack */
            StkId stack;  /* stack base */
            RemotePtr<UpVal> openupval;  /* list of open upvalues in this stack */
            RemotePtr<GCObject> gclist;
            RemotePtr<lua_State> twups;  /* list of threads with open upvalues */
            lua_longjmp* errorJmp;  /* current error recover point */
            CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
            lua_Hook hook;
            ptrdiff_t errfunc;  /* current error handling function (stack index) */
            int stacksize;
            int basehookcount;
            int hookcount;
            unsigned short nny;  /* number of non-yieldable calls in stack */
            unsigned short nCcalls;  /* number of nested C calls */
            l_signalT hookmask;
            lu_byte allowhook;

            /**
             * @brief 获取栈的调用信息
             * @param address lua_State的远端地址
             * @param level 栈层级
             * @return 调试信息
             *
             * 见 lua_getstack。
             */
            lua_Debug GetStack(uintptr_t address, int level);

            /**
             * @brief 获取活动记录下的信息
             * @param what 需要的信息
             * @param ar 活动记录
             *
             * 见 lua_getinfo。
             * 注意：不支持'f'、'L'操作符，'>'操作符不会改变栈结构。
             */
            void GetInfo(const char* what, lua_Debug& ar);
        };

        union GCUnion
        {
            GCObject gc;  /* common header */
            TString ts;
            Udata u;
            Closure cl;
            Table h;
            Proto p;
            lua_State th;  /* thread */
        };
    }
}
