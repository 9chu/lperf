/**
 * @file
 * @author chu
 * @date 2018/9/8
 */
#include "RemoteLuaWrapper.hpp"

#include <Moe.Core/Optional.hpp>

using namespace std;
using namespace moe;
using namespace lperf;

static MemoryAccessorPtr g_pAccessor;

MemoryAccessorPtr lperf::GetGlobalMemoryAccessor()noexcept
{
    return g_pAccessor;
}

void lperf::SetGlobalMemoryAccessor(MemoryAccessorPtr ptr)
{
    g_pAccessor = ptr;
}

namespace
{
    using namespace LuaObjects;

    string getstr(RemotePtr<TString> stringPtr)
    {
        auto accessor = GetGlobalMemoryAccessor();
        assert(accessor);

        auto address = reinterpret_cast<uintptr_t>(stringPtr.pointer) + sizeof(UTString);
        return accessor->ReadString(address, 1024);
    }

    bool noLuaClosure(Optional<Closure> closure)
    {
        if (!closure)
            return true;
        return closure->c.tt == LUA_TCCL;
    }

    void addstr(char*& a, const char* b, size_t l) { memcpy(a, b, l * sizeof(char)); a += l; }

    void luaO_chunkid(char* out, const char *source, size_t bufflen)
    {
        static const char* RETS = "...";
        static const char* PRE = "[string \"";
        static const char* POS = "\"]";

        size_t l = strlen(source);
        if (*source == '=')  /* 'literal' source */
        {
            if (l <= bufflen)  /* small enough? */
                memcpy(out, source + 1, l * sizeof(char));
            else  /* truncate it */
            {
                addstr(out, source + 1, bufflen - 1);
                *out = '\0';
            }
        }
        else if (*source == '@')  /* file name */
        {
            if (l <= bufflen)  /* small enough? */
                memcpy(out, source + 1, l * sizeof(char));
            else  /* add '...' before rest of name */
            {
                addstr(out, RETS, ::strlen(RETS));
                bufflen -= ::strlen(RETS);
                memcpy(out, source + 1 + l - bufflen, bufflen * sizeof(char));
            }
        }
        else  /* string; format as [string "source"] */
        {
            const char *nl = strchr(source, '\n');  /* find first new line (if any) */
            addstr(out, PRE, ::strlen(PRE));  /* add prefix */
            bufflen -= ::strlen(PRE) + ::strlen(RETS) + ::strlen(POS) + 1;  /* save space for prefix+suffix+'\0' */
            if (l < bufflen && nl == nullptr)  /* small one-line source? */
                addstr(out, source, l);  /* keep it */
            else
            {
                if (nl != NULL) l = nl - source;  /* stop at first newline */
                if (l > bufflen) l = bufflen;
                addstr(out, source, l);
                addstr(out, RETS, ::strlen(RETS));
            }
            memcpy(out, POS, (::strlen(POS) + 1) * sizeof(char));
        }
    }

    void funcinfo(lua_Debug& ar, Optional<Closure> closure)
    {
        if (noLuaClosure(closure))
        {
            ar.source = "=[C]";
            ar.linedefined = -1;
            ar.lastlinedefined = -1;
            ar.what = "C";
        }
        else
        {
            auto protoPtr = closure->l.p;
            auto proto = *protoPtr;
            ar.source = proto.source ? getstr(proto.source) : "=?";
            ar.linedefined = proto.linedefined;
            ar.lastlinedefined = proto.lastlinedefined;
            ar.what = (ar.linedefined == 0) ? "main" : "Lua";
        }
        luaO_chunkid(ar.short_src, ar.source.c_str(), LUA_IDSIZE);
    }

    int pcRel(Instruction* pc, Proto& p) { return static_cast<int>(pc - p.code.pointer) - 1; }

    int currentline(CallInfo& ci)
    {
        if (!ci.IsLua())
            MOE_THROW(BadStateException, "Invalid CallInfo state");

        auto val = *ci.func;
        auto cl = *(val.value_.gc.CastTo<Closure>());
        auto protoPtr = cl.l.p;
        auto proto = *protoPtr;
        auto pc = pcRel(ci.u.l.savedpc.pointer, proto);
        if (!proto.lineinfo)
            return -1;
        RemotePtr<int> info { proto.lineinfo.pointer + pc };
        return *info;
    }

    enum OpCode
    {
        OP_MOVE,/*	A B	R(A) := R(B)					*/
        OP_LOADK,/*	A Bx	R(A) := Kst(Bx)					*/
        OP_LOADKX,/*	A 	R(A) := Kst(extra arg)				*/
        OP_LOADBOOL,/*	A B C	R(A) := (Bool)B; if (C) pc++			*/
        OP_LOADNIL,/*	A B	R(A), R(A+1), ..., R(A+B) := nil		*/
        OP_GETUPVAL,/*	A B	R(A) := UpValue[B]				*/

        OP_GETTABUP,/*	A B C	R(A) := UpValue[B][RK(C)]			*/
        OP_GETTABLE,/*	A B C	R(A) := R(B)[RK(C)]				*/

        OP_SETTABUP,/*	A B C	UpValue[A][RK(B)] := RK(C)			*/
        OP_SETUPVAL,/*	A B	UpValue[B] := R(A)				*/
        OP_SETTABLE,/*	A B C	R(A)[RK(B)] := RK(C)				*/

        OP_NEWTABLE,/*	A B C	R(A) := {} (size = B,C)				*/

        OP_SELF,/*	A B C	R(A+1) := R(B); R(A) := R(B)[RK(C)]		*/

        OP_ADD,/*	A B C	R(A) := RK(B) + RK(C)				*/
        OP_SUB,/*	A B C	R(A) := RK(B) - RK(C)				*/
        OP_MUL,/*	A B C	R(A) := RK(B) * RK(C)				*/
        OP_MOD,/*	A B C	R(A) := RK(B) % RK(C)				*/
        OP_POW,/*	A B C	R(A) := RK(B) ^ RK(C)				*/
        OP_DIV,/*	A B C	R(A) := RK(B) / RK(C)				*/
        OP_IDIV,/*	A B C	R(A) := RK(B) // RK(C)				*/
        OP_BAND,/*	A B C	R(A) := RK(B) & RK(C)				*/
        OP_BOR,/*	A B C	R(A) := RK(B) | RK(C)				*/
        OP_BXOR,/*	A B C	R(A) := RK(B) ~ RK(C)				*/
        OP_SHL,/*	A B C	R(A) := RK(B) << RK(C)				*/
        OP_SHR,/*	A B C	R(A) := RK(B) >> RK(C)				*/
        OP_UNM,/*	A B	R(A) := -R(B)					*/
        OP_BNOT,/*	A B	R(A) := ~R(B)					*/
        OP_NOT,/*	A B	R(A) := not R(B)				*/
        OP_LEN,/*	A B	R(A) := length of R(B)				*/

        OP_CONCAT,/*	A B C	R(A) := R(B).. ... ..R(C)			*/

        OP_JMP,/*	A sBx	pc+=sBx; if (A) close all upvalues >= R(A - 1)	*/
        OP_EQ,/*	A B C	if ((RK(B) == RK(C)) ~= A) then pc++		*/
        OP_LT,/*	A B C	if ((RK(B) <  RK(C)) ~= A) then pc++		*/
        OP_LE,/*	A B C	if ((RK(B) <= RK(C)) ~= A) then pc++		*/

        OP_TEST,/*	A C	if not (R(A) <=> C) then pc++			*/
        OP_TESTSET,/*	A B C	if (R(B) <=> C) then R(A) := R(B) else pc++	*/

        OP_CALL,/*	A B C	R(A), ... ,R(A+C-2) := R(A)(R(A+1), ... ,R(A+B-1)) */
        OP_TAILCALL,/*	A B C	return R(A)(R(A+1), ... ,R(A+B-1))		*/
        OP_RETURN,/*	A B	return R(A), ... ,R(A+B-2)	(see note)	*/

        OP_FORLOOP,/*	A sBx	R(A)+=R(A+2);
                        if R(A) <?= R(A+1) then { pc+=sBx; R(A+3)=R(A) }*/
        OP_FORPREP,/*	A sBx	R(A)-=R(A+2); pc+=sBx				*/

        OP_TFORCALL,/*	A C	R(A+3), ... ,R(A+2+C) := R(A)(R(A+1), R(A+2));	*/
        OP_TFORLOOP,/*	A sBx	if R(A+1) ~= nil then { R(A)=R(A+1); pc += sBx }*/

        OP_SETLIST,/*	A B C	R(A)[(C-1)*FPF+i] := R(A+i), 1 <= i <= B	*/

        OP_CLOSURE,/*	A Bx	R(A) := closure(KPROTO[Bx])			*/

        OP_VARARG,/*	A B	R(A), R(A+1), ..., R(A+B-2) = vararg		*/

        OP_EXTRAARG/*	Ax	extra (larger) argument for previous opcode	*/
    };

    enum OpArgMask {
        OpArgN,  /* argument is not used */
        OpArgU,  /* argument is used */
        OpArgR,  /* argument is a register or a jump offset */
        OpArgK   /* argument is a constant or register/constant */
    };

    enum OpMode { iABC, iABx, iAsBx, iAx };

    constexpr lu_byte opmode(int t, int a, int b, int c, int m)
    {
        return static_cast<lu_byte>(((t)<<7) | ((a)<<6) | ((b)<<4) | ((c)<<2) | (m));
    }

    const lu_byte luaP_opmodes[static_cast<int>(OP_EXTRAARG) + 1] = {
        /*       T  A    B       C     mode		   opcode	*/
        opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_MOVE */
        ,opmode(0, 1, OpArgK, OpArgN, iABx)		/* OP_LOADK */
        ,opmode(0, 1, OpArgN, OpArgN, iABx)		/* OP_LOADKX */
        ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_LOADBOOL */
        ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_LOADNIL */
        ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_GETUPVAL */
        ,opmode(0, 1, OpArgU, OpArgK, iABC)		/* OP_GETTABUP */
        ,opmode(0, 1, OpArgR, OpArgK, iABC)		/* OP_GETTABLE */
        ,opmode(0, 0, OpArgK, OpArgK, iABC)		/* OP_SETTABUP */
        ,opmode(0, 0, OpArgU, OpArgN, iABC)		/* OP_SETUPVAL */
        ,opmode(0, 0, OpArgK, OpArgK, iABC)		/* OP_SETTABLE */
        ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_NEWTABLE */
        ,opmode(0, 1, OpArgR, OpArgK, iABC)		/* OP_SELF */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_ADD */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SUB */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_MUL */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_MOD */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_POW */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_DIV */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_IDIV */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BAND */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BOR */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BXOR */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SHL */
        ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SHR */
        ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_UNM */
        ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_BNOT */
        ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_NOT */
        ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_LEN */
        ,opmode(0, 1, OpArgR, OpArgR, iABC)		/* OP_CONCAT */
        ,opmode(0, 0, OpArgR, OpArgN, iAsBx)		/* OP_JMP */
        ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_EQ */
        ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_LT */
        ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_LE */
        ,opmode(1, 0, OpArgN, OpArgU, iABC)		/* OP_TEST */
        ,opmode(1, 1, OpArgR, OpArgU, iABC)		/* OP_TESTSET */
        ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_CALL */
        ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_TAILCALL */
        ,opmode(0, 0, OpArgU, OpArgN, iABC)		/* OP_RETURN */
        ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_FORLOOP */
        ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_FORPREP */
        ,opmode(0, 0, OpArgN, OpArgU, iABC)		/* OP_TFORCALL */
        ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_TFORLOOP */
        ,opmode(0, 0, OpArgU, OpArgU, iABC)		/* OP_SETLIST */
        ,opmode(0, 1, OpArgU, OpArgN, iABx)		/* OP_CLOSURE */
        ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_VARARG */
        ,opmode(0, 0, OpArgU, OpArgU, iAx)		/* OP_EXTRAARG */
    };

    static const unsigned SIZE_OP = 6;
    static const unsigned SIZE_A = 8;
    static const unsigned SIZE_B = 9;
    static const unsigned SIZE_C = 9;
    static const unsigned SIZE_Bx = SIZE_C + SIZE_B;
    static const unsigned SIZE_Ax = SIZE_C + SIZE_B + SIZE_A;

    static const unsigned POS_OP = 0;
    static const unsigned POS_A = (POS_OP + SIZE_OP);
    static const unsigned POS_C	= POS_A + SIZE_A;
    static const unsigned POS_B = POS_C + SIZE_C;
    static const unsigned POS_Bx = POS_C;
    static const unsigned POS_Ax = POS_A;

    static const unsigned MAXARG_Bx = static_cast<unsigned>((1 << SIZE_Bx) - 1);
    static const unsigned MAXARG_sBx = MAXARG_Bx >> 1;

    static const unsigned BITRK = 1 << (SIZE_B - 1);

    int MASK1(int n, int p) { return (~((~(Instruction)0) << n)) << p; }
    int getarg(int i, int pos, int size) { return static_cast<int>((i >> pos) & MASK1(size, 0)); }
    int GETARG_A(int i) { return getarg(i, POS_A, SIZE_A); }
    int GETARG_Ax(int i) { return getarg(i, POS_Ax, SIZE_Ax); }
    int GETARG_B(int i) { return getarg(i, POS_B, SIZE_B); }
    int GETARG_C(int i) { return getarg(i, POS_C, SIZE_C); }
    int GETARG_Bx(int i) { return getarg(i, POS_Bx, SIZE_Bx); }
    OpCode GET_OPCODE(int i) { return static_cast<OpCode>((i >> POS_OP) & MASK1(SIZE_OP, 0)); }
    int GETARG_sBx(int i) { return GETARG_Bx(i) - MAXARG_sBx; }

    int testAMode(OpCode m) { return luaP_opmodes[m] & (1 << 6); }

    bool ISK(int x) { return (x & BITRK) != 0; }
    int INDEXK(int r) { return r & ~BITRK; }

    string luaF_getlocalname(Proto& f, int local_number, int pc)
    {
        LocVar loc;
        for (int i = 0; i < f.sizelocvars && (loc = *(RemotePtr<LocVar> { f.locvars.pointer + i }), loc.startpc) <= pc;
            ++i)
        {
            if (pc < loc.endpc)  /* is variable active? */
            {
                --local_number;
                if (local_number == 0)
                    return getstr(loc.varname);
            }
        }
        return string();  /* not found */
    }

    int filterpc(int pc, int jmptarget)
    {
        if (pc < jmptarget)  /* is code conditional (inside a jump)? */
            return -1;  /* cannot know who sets that register */
        else
            return pc;  /* current position sets that register */
    }

    int findsetreg(Proto& p, int lastpc, int reg)
    {
        int setreg = -1;  /* keep last instruction that changed 'reg' */
        int jmptarget = 0;  /* any code before this address is conditional */
        for (int pc = 0; pc < lastpc; ++pc)
        {
            Instruction i = *(RemotePtr<Instruction> { p.code.pointer + pc });
            OpCode op = GET_OPCODE(i);
            int a = GETARG_A(i);
            switch (op)
            {
                case OP_LOADNIL:
                    {
                        int b = GETARG_B(i);
                        if (a <= reg && reg <= a + b)  /* set registers from 'a' to 'a+b' */
                            setreg = filterpc(pc, jmptarget);
                    }
                    break;
                case OP_TFORCALL:
                    if (reg >= a + 2)  /* affect all regs above its base */
                        setreg = filterpc(pc, jmptarget);
                    break;
                case OP_CALL:
                case OP_TAILCALL:
                    if (reg >= a)  /* affect all registers above base */
                        setreg = filterpc(pc, jmptarget);
                    break;
                case OP_JMP:
                    {
                        int b = GETARG_sBx(i);
                        int dest = pc + 1 + b;
                        /* jump is forward and do not skip 'lastpc'? */
                        if (pc < dest && dest <= lastpc)
                        {
                            if (dest > jmptarget)
                                jmptarget = dest;  /* update 'jmptarget' */
                        }
                    }
                    break;
                default:
                    if (testAMode(op) && reg == a)  /* any instruction that set A */
                        setreg = filterpc(pc, jmptarget);
                    break;
            }
        }
        return setreg;
    }

    string upvalname(Proto& p, int uv)
    {
        if (uv >= p.sizeupvalues)
            MOE_THROW(BadStateException, "Invalid data");

        RemotePtr<Upvaldesc> descPtr { p.upvalues.pointer + uv };
        auto desc = *descPtr;
        auto s = desc.name;
        if (!s)
            return "?";
        else
            return getstr(s);
    }

    const char* getobjname(Proto& p, int lastpc, int reg, string& name);

    void kname(Proto& p, int pc, int c, string& name)
    {
        if (ISK(c))  /* is 'c' a constant? */
        {
            TValue kvalue = *(RemotePtr<TValue> { p.k.pointer + INDEXK(c) });
            if (kvalue.IsString())  /* literal constant? */
            {
                name = getstr(kvalue.value_.gc.CastTo<TString>());  /* it is its own name */
                return;
            }
            /* else no reasonable name found */
        }
        else  /* 'c' is a register */
        {
            const char *what = getobjname(p, pc, c, name); /* search for 'c' */
            if (what && *what == 'c')  /* found a constant name? */
                return;  /* 'name' already filled */
            /* else no reasonable name found */
        }
        name = "?";  /* no reasonable name found */
    }

    const char* getobjname(Proto& p, int lastpc, int reg, string& name)
    {
        static const char* LUA_ENV = "_ENV";

        name = luaF_getlocalname(p, reg + 1, lastpc);
        if (!name.empty())  /* is a local? */
            return "local";
        /* else try symbolic execution */
        int pc = findsetreg(p, lastpc, reg);
        if (pc != -1)  /* could find instruction? */
        {
            Instruction i = *(RemotePtr<Instruction> { p.code.pointer + pc });
            OpCode op = GET_OPCODE(i);
            switch (op)
            {
                case OP_MOVE:
                    {
                        int b = GETARG_B(i);  /* move from 'b' to 'a' */
                        if (b < GETARG_A(i))
                            return getobjname(p, pc, b, name);  /* get name for 'b' */
                    }
                    break;
                case OP_GETTABUP:
                case OP_GETTABLE:
                    {
                        int k = GETARG_C(i);  /* key index */
                        int t = GETARG_B(i);  /* table index */
                        string vn = (op == OP_GETTABLE) ? luaF_getlocalname(p, t + 1, pc) : upvalname(p, t);
                        kname(p, pc, k, name);
                        return (!vn.empty() && strcmp(vn.c_str(), LUA_ENV) == 0) ? "global" : "field";
                    }
                case OP_GETUPVAL:
                    {
                        name = upvalname(p, GETARG_B(i));
                        return "upvalue";
                    }
                case OP_LOADK:
                case OP_LOADKX:
                    {
                        int b = 0;
                        if (op == OP_LOADK)
                            b = GETARG_Bx(i);
                        else
                        {
                            auto i = *(RemotePtr<Instruction> { p.code.pointer + (pc + 1) });
                            b = GETARG_Ax(i);
                        }

                        TValue kvalue = *(RemotePtr<TValue> { p.k.pointer + b });
                        if (kvalue.IsString())
                        {
                            name = getstr(kvalue.value_.gc.CastTo<TString>());
                            return "constant";
                        }
                    }
                    break;
                case OP_SELF:
                    {
                        int k = GETARG_C(i);  /* key index */
                        kname(p, pc, k, name);
                        return "method";
                    }
                default:
                    break;  /* go through to return NULL */
            }
        }
        return nullptr;  /* could not find reasonable name */
    }

    const char* funcnamefromcode(lua_State& L, CallInfo& ci, string& name)
    {
        TMS tm = static_cast<TMS>(0);  /* (initial value avoids warnings) */
        auto val = *ci.func;
        if (!val.IsFunction())
            MOE_THROW(BadStateException, "Invalid data");
        auto cl = *(val.value_.gc.CastTo<LuaObjects::Closure>());
        auto protoPtr = cl.l.p;
        auto p = *protoPtr;
        int pc = pcRel(ci.u.l.savedpc.pointer, p);  /* calling instruction index */
        RemotePtr<Instruction> ip { p.code.pointer + pc };
        Instruction i = *ip;  /* calling instruction */
        if (ci.IsHooked())  /* was it called inside a hook? */
        {
            name = "?";
            return "hook";
        }
        switch (GET_OPCODE(i))
        {
            case OP_CALL:
            case OP_TAILCALL:  /* get function name */
                return getobjname(p, pc, GETARG_A(i), name);
            case OP_TFORCALL:  /* for iterator */
                name = "for iterator";
                return "for iterator";
            case OP_SELF: case OP_GETTABUP: case OP_GETTABLE:  /* other instructions can do calls through metamethods */
                tm = TM_INDEX;
                break;
            case OP_SETTABUP: case OP_SETTABLE:
                tm = TM_NEWINDEX;
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
            case OP_POW: case OP_DIV: case OP_IDIV: case OP_BAND:
            case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
                tm = static_cast<TMS>(static_cast<int>(GET_OPCODE(i)) - static_cast<int>(OP_ADD) +
                    static_cast<int>(TM_ADD));
                break;
            case OP_UNM: tm = TM_UNM; break;
            case OP_BNOT: tm = TM_BNOT; break;
            case OP_LEN: tm = TM_LEN; break;
            case OP_CONCAT: tm = TM_CONCAT; break;
            case OP_EQ: tm = TM_EQ; break;
            case OP_LT: tm = TM_LT; break;
            case OP_LE: tm = TM_LE; break;
            default:
                return nullptr;
        }
        name = getstr((*L.l_G).tmname[tm]);
        return "metamethod";
    }

    const char* getfuncname(lua_State& L, Optional<CallInfo> ci, string& name)
    {
        if (!ci)
            return nullptr;
        else if (ci->IsFinalizer())  /* is this a finalizer? */
        {
            name = "__gc";
            return "metamethod";  /* report it as such */
        }
        else
        {
            auto previousPtr = ci->previous;
            auto previous = *previousPtr;
            if (!ci->IsTailCall() && previous.IsLua())  /* calling function is a known Lua function? */
                return funcnamefromcode(L, previous, name);
        }
        return nullptr;
    }

    void auxgetinfo(lua_State& L, const char* what, lua_Debug& ar, Optional<Closure> f, Optional<CallInfo> ci)
    {
        for (; *what; ++what)
        {
            switch (*what)
            {
                case 'S':
                    funcinfo(ar, f);
                    if (f && f->c.tt == LUA_TCCL)
                        ar.address = reinterpret_cast<uintptr_t>(f->c.f);
                    break;
                case 'l':
                    ar.currentline = (ci && ci->IsLua()) ? currentline(*ci) : -1;
                    break;
                case 'u':
                    ar.nups = static_cast<uint8_t>((!f) ? 0 : f->c.nupvalues);
                    if (noLuaClosure(f))
                    {
                        ar.isvararg = true;
                        ar.nparams = 0;
                    }
                    else
                    {
                        auto protoPtr = f->l.p;
                        auto proto = *protoPtr;
                        ar.isvararg = static_cast<bool>(proto.is_vararg);
                        ar.nparams = proto.numparams;
                    }
                    break;
                case 't':
                    ar.istailcall = ci ? ci->IsTailCall() : false;
                    break;
                case 'n':
                    ar.namewhat = getfuncname(L, ci, ar.name);
                    if (!ar.namewhat)
                    {
                        ar.namewhat = "";  /* not found */
                        ar.name.clear();
                    }
                    break;
                default:
                    assert(false);
                    break;
            }
        }
    }
}

LuaObjects::lua_Debug LuaObjects::lua_State::GetStack(uintptr_t address, int level)
{
    if (level < 0)
        MOE_THROW(BadArgumentException, "Invalid negative level");

    LuaObjects::lua_Debug ret {};
    memset(&ret, 0, sizeof(ret));

    RemotePtr<CallInfo> baseCallInfoPtr { reinterpret_cast<CallInfo*>(address + offsetof(lua_State, base_ci)) };
    RemotePtr<CallInfo> callInfoPtr { nullptr };
    for (callInfoPtr = ci; level > 0 && callInfoPtr != baseCallInfoPtr; callInfoPtr = (*callInfoPtr).previous)
        level--;
    if (level == 0 && callInfoPtr != baseCallInfoPtr)
    {
        ret.i_ci = callInfoPtr;
        return ret;
    }
    MOE_THROW(ObjectNotFoundException, "Stack level {0} not found", level);
}

void LuaObjects::lua_State::GetInfo(const char* what, lua_Debug& ar)
{
    Optional<CallInfo> callInfo;
    StkId funcPtr { nullptr };
    lua_TValue func {};
    if (*what == '>')
    {
        funcPtr.pointer = top.pointer - 1;
        func = *funcPtr;

        if (!func.IsFunction())
            MOE_THROW(BadStateException, "Function expected");
        ++what;
    }
    else
    {
        auto callInfoPtr = ar.i_ci;
        callInfo = *callInfoPtr;
        funcPtr = callInfo->func;
        func = *funcPtr;

        if (!func.IsFunction())
            MOE_THROW(BadStateException, "Bad remote data");
    }

    Optional<Closure> closure;
    if (func.IsClosure())
        closure = *(func.value_.gc.CastTo<LuaObjects::Closure>());
    else if (func.IsLightCFunction())
        ar.address = reinterpret_cast<uintptr_t>(func.value_.f);
    auxgetinfo(*this, what, ar, closure, callInfo);
}
