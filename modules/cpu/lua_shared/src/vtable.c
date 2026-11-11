/* Photon CPU L3: the CLuaInterface vtable, slot order of GMod's x86-64 lua_shared (clua_slots.txt). Generated from the slot
 * numbers in the li_* comments (iface_base.c, iface_iface.c, iface_more.c). */
#include "iface.h"

void li_Top(void);
void li_Push(void);
void li_Pop(void);
void li_GetTable(void);
void li_GetField(void);
void li_SetField(void);
void li_CreateTable(void);
void li_SetTable(void);
void li_SetMetaTable(void);
void li_GetMetaTable(void);
void li_Call(void);
void li_PCall(void);
void li_Equal(void);
void li_RawEqual(void);
void li_Insert(void);
void li_Remove(void);
void li_Next(void);
void li_NewUserdata(void);
void li_ThrowError(void);
void li_CheckType(void);
void li_ArgError(void);
void li_RawGet(void);
void li_RawSet(void);
void li_GetString(void);
void li_GetNumber(void);
void li_GetBool(void);
void li_GetCFunction(void);
void li_GetUserdata(void);
void li_PushNil(void);
void li_PushString(void);
void li_PushNumber(void);
void li_PushBool(void);
void li_PushCFunction(void);
void li_PushCClosure(void);
void li_PushUserdata(void);
void li_ReferenceCreate(void);
void li_ReferenceFree(void);
void li_ReferencePush(void);
void li_PushSpecial(void);
void li_IsType(void);
void li_GetType(void);
void li_GetTypeName(void);
void li_CreateMetaTableType(void);
void li_CheckString(void);
void li_CheckNumber(void);
void li_ObjLen(void);
void li_GetAngle(void);
void li_GetVector(void);
void li_PushAngle(void);
void li_PushVector(void);
void li_SetState(void);
void li_CreateMetaTable(void);
void li_PushMetaTable(void);
void li_PushUserType(void);
void li_SetUserType(void);
void li_Init(void);
void li_Shutdown(void);
void li_Cycle(void);
void li_Global(void);
void li_GetObject(void);
void li_PushLuaObject(void);
void li_PushLuaFunction(void);
void li_LuaError(void);
void li_TypeError(void);
void li_CallInternal(void);
void li_CallInternalNoReturns(void);
void li_CallInternalGetBool(void);
void li_CallInternalGetString(void);
void li_CallInternalGet(void);
void li_NewGlobalTable(void);
void li_NewTemporaryObject(void);
void li_isUserData(void);
void li_GetMetaTableObject_idx(void);
void li_GetMetaTableObject_name(void);
void li_GetReturn(void);
void li_IsServer(void);
void li_IsClient(void);
void li_IsMenu(void);
void li_DestroyObject(void);
void li_CreateObject(void);
void li_SetMember_str_obj(void);
void li_SetMember_str(void);
void li_SetMember_num_obj(void);
void li_SetMember_num(void);
void li_SetMember_obj_obj(void);
void li_GetNewTable(void);
void li_SetType(void);
void li_PushLong(void);
void li_GetFlags(void);
void li_FindOnObjectsMetaTable(void);
void li_FindObjectOnTable(void);
void li_SetMemberFast(void);
void li_RunString(void);
void li_IsEqual(void);
void li_Error(void);
void li_GetStringOrError(void);
void li_RunLuaModule(void);
void li_FindAndRunScript(void);
void li_SetPathID(void);
void li_GetPathID(void);
void li_ErrorNoHalt(void);
void li_Msg(void);
void li_PushPath(void);
void li_PopPath(void);
void li_GetPath(void);
void li_GetColor(void);
void li_PushColor(void);
void li_GetStack(void);
void li_GetInfo(void);
void li_GetLocal(void);
void li_GetUpvalue(void);
void li_RunStringEx(void);
void li_GetDataString(void);
void li_ErrorFromLua(void);
void li_GetCurrentLocation(void);
void li_MsgColour(void);
void li_GetCurrentFile(void);
void li_CompileString(void);
void li_CallFunctionProtected(void);
void li_Require(void);
void li_GetActualTypeName(void);
void li_PreCreateTable(void);
void li_PushPooledString(void);
void li_GetPooledString(void);
void li_AddThreadedCall(void);
void li_AppendStackTrace(void);
void li_CreateConVar(void);
void li_CreateConCommand(void);
void li_CheckStringOpt(void);
void li_CheckNumberOpt(void);
void li_RegisterMetaTable(void);
void li_FreeInternals(void);
void li_ErrorF(void);
void *photon_iface_vtable[133] = {
    (void *)li_Top,   /* 0 */
    (void *)li_Push,   /* 1 */
    (void *)li_Pop,   /* 2 */
    (void *)li_GetTable,   /* 3 */
    (void *)li_GetField,   /* 4 */
    (void *)li_SetField,   /* 5 */
    (void *)li_CreateTable,   /* 6 */
    (void *)li_SetTable,   /* 7 */
    (void *)li_SetMetaTable,   /* 8 */
    (void *)li_GetMetaTable,   /* 9 */
    (void *)li_Call,   /* 10 */
    (void *)li_PCall,   /* 11 */
    (void *)li_Equal,   /* 12 */
    (void *)li_RawEqual,   /* 13 */
    (void *)li_Insert,   /* 14 */
    (void *)li_Remove,   /* 15 */
    (void *)li_Next,   /* 16 */
    (void *)li_NewUserdata,   /* 17 */
    (void *)li_ThrowError,   /* 18 */
    (void *)li_CheckType,   /* 19 */
    (void *)li_ArgError,   /* 20 */
    (void *)li_RawGet,   /* 21 */
    (void *)li_RawSet,   /* 22 */
    (void *)li_GetString,   /* 23 */
    (void *)li_GetNumber,   /* 24 */
    (void *)li_GetBool,   /* 25 */
    (void *)li_GetCFunction,   /* 26 */
    (void *)li_GetUserdata,   /* 27 */
    (void *)li_PushNil,   /* 28 */
    (void *)li_PushString,   /* 29 */
    (void *)li_PushNumber,   /* 30 */
    (void *)li_PushBool,   /* 31 */
    (void *)li_PushCFunction,   /* 32 */
    (void *)li_PushCClosure,   /* 33 */
    (void *)li_PushUserdata,   /* 34 */
    (void *)li_ReferenceCreate,   /* 35 */
    (void *)li_ReferenceFree,   /* 36 */
    (void *)li_ReferencePush,   /* 37 */
    (void *)li_PushSpecial,   /* 38 */
    (void *)li_IsType,   /* 39 */
    (void *)li_GetType,   /* 40 */
    (void *)li_GetTypeName,   /* 41 */
    (void *)li_CreateMetaTableType,   /* 42 */
    (void *)li_CheckString,   /* 43 */
    (void *)li_CheckNumber,   /* 44 */
    (void *)li_ObjLen,   /* 45 */
    (void *)li_GetAngle,   /* 46 */
    (void *)li_GetVector,   /* 47 */
    (void *)li_PushAngle,   /* 48 */
    (void *)li_PushVector,   /* 49 */
    (void *)li_SetState,   /* 50 */
    (void *)li_CreateMetaTable,   /* 51 */
    (void *)li_PushMetaTable,   /* 52 */
    (void *)li_PushUserType,   /* 53 */
    (void *)li_SetUserType,   /* 54 */
    (void *)li_Init,   /* 55 */
    (void *)li_Shutdown,   /* 56 */
    (void *)li_Cycle,   /* 57 */
    (void *)li_Global,   /* 58 */
    (void *)li_GetObject,   /* 59 */
    (void *)li_PushLuaObject,   /* 60 */
    (void *)li_PushLuaFunction,   /* 61 */
    (void *)li_LuaError,   /* 62 */
    (void *)li_TypeError,   /* 63 */
    (void *)li_CallInternal,   /* 64 */
    (void *)li_CallInternalNoReturns,   /* 65 */
    (void *)li_CallInternalGetBool,   /* 66 */
    (void *)li_CallInternalGetString,   /* 67 */
    (void *)li_CallInternalGet,   /* 68 */
    (void *)li_NewGlobalTable,   /* 69 */
    (void *)li_NewTemporaryObject,   /* 70 */
    (void *)li_isUserData,   /* 71 */
    (void *)li_GetMetaTableObject_idx,   /* 72 */
    (void *)li_GetMetaTableObject_name,   /* 73 */
    (void *)li_GetReturn,   /* 74 */
    (void *)li_IsServer,   /* 75 */
    (void *)li_IsClient,   /* 76 */
    (void *)li_IsMenu,   /* 77 */
    (void *)li_DestroyObject,   /* 78 */
    (void *)li_CreateObject,   /* 79 */
    (void *)li_SetMember_str_obj,   /* 80 */
    (void *)li_SetMember_str,   /* 81 */
    (void *)li_SetMember_num_obj,   /* 82 */
    (void *)li_SetMember_num,   /* 83 */
    (void *)li_SetMember_obj_obj,   /* 84 */
    (void *)li_GetNewTable,   /* 85 */
    (void *)li_SetType,   /* 86 */
    (void *)li_PushLong,   /* 87 */
    (void *)li_GetFlags,   /* 88 */
    (void *)li_FindOnObjectsMetaTable,   /* 89 */
    (void *)li_FindObjectOnTable,   /* 90 */
    (void *)li_SetMemberFast,   /* 91 */
    (void *)li_RunString,   /* 92 */
    (void *)li_IsEqual,   /* 93 */
    (void *)li_Error,   /* 94 */
    (void *)li_GetStringOrError,   /* 95 */
    (void *)li_RunLuaModule,   /* 96 */
    (void *)li_FindAndRunScript,   /* 97 */
    (void *)li_SetPathID,   /* 98 */
    (void *)li_GetPathID,   /* 99 */
    (void *)li_ErrorNoHalt,   /* 100 */
    (void *)li_Msg,   /* 101 */
    (void *)li_PushPath,   /* 102 */
    (void *)li_PopPath,   /* 103 */
    (void *)li_GetPath,   /* 104 */
    (void *)li_GetColor,   /* 105 */
    (void *)li_PushColor,   /* 106 */
    (void *)li_GetStack,   /* 107 */
    (void *)li_GetInfo,   /* 108 */
    (void *)li_GetLocal,   /* 109 */
    (void *)li_GetUpvalue,   /* 110 */
    (void *)li_RunStringEx,   /* 111 */
    (void *)li_GetDataString,   /* 112 */
    (void *)li_ErrorFromLua,   /* 113 */
    (void *)li_GetCurrentLocation,   /* 114 */
    (void *)li_MsgColour,   /* 115 */
    (void *)li_GetCurrentFile,   /* 116 */
    (void *)li_CompileString,   /* 117 */
    (void *)li_CallFunctionProtected,   /* 118 */
    (void *)li_Require,   /* 119 */
    (void *)li_GetActualTypeName,   /* 120 */
    (void *)li_PreCreateTable,   /* 121 */
    (void *)li_PushPooledString,   /* 122 */
    (void *)li_GetPooledString,   /* 123 */
    (void *)li_AddThreadedCall,   /* 124 */
    (void *)li_AppendStackTrace,   /* 125 */
    (void *)li_CreateConVar,   /* 126 */
    (void *)li_CreateConCommand,   /* 127 */
    (void *)li_CheckStringOpt,   /* 128 */
    (void *)li_CheckNumberOpt,   /* 129 */
    (void *)li_RegisterMetaTable,   /* 130 */
    (void *)li_FreeInternals,   /* 131 */
    (void *)li_ErrorF,   /* 132 */
};
