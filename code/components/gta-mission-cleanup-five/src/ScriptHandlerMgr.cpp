/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <ScriptHandlerMgr.h>

#include "Hooking.h"

#include "Pool.h"

#include <MinHook.h>

#include <Error.h>

// pool functions, here temporarily we hope :)
static atPoolBase** g_scriptHandlerNetworkPool;

static CGameScriptHandlerMgr* g_scriptHandlerMgr;

static rage::scrThread*(*g_origGetThreadById)(uint32_t hash);

static std::map<uint32_t, rage::scrThread*> g_customThreads;

static rage::scrThread* GetThreadById(uint32_t hash)
{
	auto it = g_customThreads.find(hash);

	if (it != g_customThreads.end())
	{
		return it->second;
	}

	return g_origGetThreadById(hash);
}

DLL_EXPORT fwEvent<rage::scrThread*> OnCreateResourceThread;
DLL_EXPORT fwEvent<rage::scrThread*> OnDeleteResourceThread;

// find data fields and perform patches
static HookFunction hookFunction([] ()
{
	char* location = hook::pattern("BA E8 10 E8 4F 41").count(1).get(0).get<char>(52);

	g_scriptHandlerNetworkPool = (atPoolBase**)(location + *(int32_t*)location + 4);

	// rage::scriptHandler destructor does something incredibly stupid - the vtable gets set to the base as usual, but then the 'custom' code
	// in the case when we execute it decides to call GetScriptId, which is a __purecall in rage::scriptHandler.

	// therefore, we patch that check out to never execute.
	hook::put<uint8_t>(hook::pattern("80 78 32 00 75 34 B1 01 E8").count(1).get(0).get<void>(4), 0xEB);

	// find CGameScriptHandlerMgr pointer
	location = hook::pattern("48 8D 55 17 48 8D 0D ? ? ? ? FF").count(1).get(0).get<char>(7);

	g_scriptHandlerMgr = (CGameScriptHandlerMgr*)(location + *(int32_t*)location + 4);

	// script threads for dummies
	MH_Initialize();
	MH_CreateHook(hook::get_pattern("33 D2 44 8B C1 85 C9 74 2B 0F"), GetThreadById, (void**)&g_origGetThreadById);
	MH_EnableHook(MH_ALL_HOOKS);

	OnCreateResourceThread.Connect([](rage::scrThread* thread)
	{
		g_customThreads.insert({ thread->GetContext()->ThreadId, thread });
	});

	OnDeleteResourceThread.Connect([](rage::scrThread* thread)
	{
		g_customThreads.erase(thread->GetContext()->ThreadId);
	});
});

// functions
static hook::thiscall_stub<void(CGameScriptHandlerNetwork*, rage::scrThread*)> scriptHandlerNetwork__ctor([] ()
{
	//return hook::pattern("33 C0 48 89 83 A0 00 00 00 66 89 83 A8").count(1).get(0).get<void>(-0x18);
	return hook::pattern("33 C0 48 89 83 A0 00 00 00 89 83 A8 00 00 00 66 89 83 AC 00").count(1).get(0).get<void>(-0x18);
});

void* CGameScriptHandlerNetwork::operator new(size_t size)
{
	return rage::PoolAllocate(*g_scriptHandlerNetworkPool);
}

CGameScriptHandlerNetwork::CGameScriptHandlerNetwork(rage::scrThread* thread)
{
	scriptHandlerNetwork__ctor(this, thread);
}

static hook::thiscall_stub<void(void*, uint32_t*, void*)> setHashMap([] ()
{
	return hook::get_call(hook::pattern("48 8D 54 24 50 48 8B CE 89 44 24 50 E8").count(1).get(0).get<void>(12));
});

void CGameScriptHandlerMgr::scriptHandlerHashMap::Set(uint32_t* hash, rage::scriptHandler** handler)
{
	return setHashMap(this, hash, handler);
}

static void(*g_origDetachScript)(void*, void*);
void WrapDetachScript(void* a1, void* script)
{
	// sometimes scripts here are on the C++ side, which use a copied scripthandler from another script
	// these will except as they're _already_ freed, so we catch that exception here
	//__try
	{
		g_origDetachScript(a1, script);
	}
	//__except (EXCEPTION_EXECUTE_HANDLER)
	//{
//		trace("CGameScriptHandlerMgr::DetachScript() excepted, caught and returned.\n");
	//}
}

#include <mutex>

CGameScriptHandlerMgr* CGameScriptHandlerMgr::GetInstance()
{
	return g_scriptHandlerMgr;
}

// implemented parent functions for shutting up the compiler
namespace rage
{
	scriptHandler::~scriptHandler()
	{

	}

	scriptHandlerImplemented::~scriptHandlerImplemented()
	{
		FatalError(__FUNCTION__);
	}

	void scriptHandlerImplemented::CleanupObjectList()
	{
		FatalError(__FUNCTION__);
	}

	void scriptHandlerImplemented::m_8()
	{
		FatalError(__FUNCTION__);
	}

	void scriptHandlerImplemented::m_10()
	{
		FatalError(__FUNCTION__);
	}

	scriptId* scriptHandlerImplemented::GetScriptId()
	{
		FatalError(__FUNCTION__);

		return nullptr;
	}

	scriptId* scriptHandlerImplemented::GetScriptId_2()
	{
		FatalError(__FUNCTION__);

		return nullptr;
	}

	bool scriptHandlerImplemented::IsNetworkScript()
	{
		FatalError(__FUNCTION__);

		return false;
	}
}

static HookFunction hookFunctionVtbl([]()
{
	{
		auto vtable = hook::get_address<uintptr_t*>(hook::get_pattern("41 83 C8 FF 48 89 03 89 53 70 88 53 74 4C 89 4B", -11));

		g_origDetachScript = ((decltype(g_origDetachScript))vtable[11]);
		vtable[11] = (uintptr_t)WrapDetachScript;
	}
});
