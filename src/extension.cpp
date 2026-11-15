/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "convarhelper.h"
#include "CDetour/detours.h"
#include <sourcehook.h>
#include <iclient.h>
#include <iserver.h>
#include <igameevents.h>
#include <iplayerinfo.h>
#include <soundinfo.h>
#include <threadtools.h>

class CFrameSnapshot;
class CClientFrame;
class CFrameSnapshotManager {};

typedef intptr_t PackedEntityHandle_t;

class CBaseClient : public IGameEventListener2, public IClient
{

};

SSF g_SSF;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_SSF);

IGameConfig *g_pGameConf = NULL;
CGlobalVars *gpGlobals = NULL;

CDetour *g_Detour_CBaseServer__WriteTempEntities = NULL;
CDetour *g_Detour_CFrameSnapshot__ReleaseReference = NULL;
CDetour *g_Detour_CFrameSnapshot__CreateEmptySnapshot = NULL;
CDetour *g_Detour_CFrameSnapshotManager__LevelChanged = NULL;

// CFrameSnapshotManager::RemoveEntityReference( PackedEntityHandle_t ) - not virtual, called through its address
typedef void (CFrameSnapshotManager::*RemoveEntityReference_t)(PackedEntityHandle_t);
RemoveEntityReference_t g_pfnRemoveEntityReference = NULL;
// Offset of CFrameSnapshotManager::m_pPackedData[MAX_EDICTS]
int g_iPackedDataOffset = -1;

unsigned int g_nPackedLastReleased = 0;
unsigned long long g_nPackedTotalReleased = 0;
unsigned int g_nPackedLevelChanges = 0;

// Mutex for m_FrameSnapshots array
CThreadFastMutex									m_FrameSnapshotsWriteMutex;

// ConVar *g_SvSSFLog = CreateConVar("sv_ssf_log", "0", FCVAR_NOTIFY, "Log ssf debug print statements.");
ConVar *g_sv_multiplayer_maxtempentities = CreateConVar("sv_multiplayer_maxtempentities", "64");
ConVar *g_sv_ssf_fix_packedentity_leak = CreateConVar("sv_ssf_fix_packedentity_leak", "1", FCVAR_NONE, "Release the last sent packed entities on level change (fixes a memory leak in CFrameSnapshotManager::LevelChanged).");

DETOUR_DECL_MEMBER2(CFrameSnapshot__CreateEmptySnapshot, CFrameSnapshot *, int, tickcount, int, maxEntities )
{
	AUTO_LOCK(m_FrameSnapshotsWriteMutex);
	
	CFrameSnapshot* snap = DETOUR_MEMBER_CALL(CFrameSnapshot__CreateEmptySnapshot)(tickcount, maxEntities);

	return snap;
}

// Keep list building thread-safe
// This lock was moved to to fix bug https://bugbait.valvesoftware.com/show_bug.cgi?id=53403
// Crash in CFrameSnapshotManager::GetPackedEntity where a CBaseClient's m_pBaseline snapshot could be removed the CReferencedSnapshotList destructor 
// for another client that is in WriteTempEntities

DETOUR_DECL_MEMBER0(CFrameSnapshot__ReleaseReference, void)
{
	AUTO_LOCK(m_FrameSnapshotsWriteMutex);
	
	DETOUR_MEMBER_CALL(CFrameSnapshot__ReleaseReference)();
}

DETOUR_DECL_MEMBER5(CBaseServer__WriteTempEntities, void, CBaseClient *, client, CFrameSnapshot *, pCurrentSnapshot, CFrameSnapshot *, pLastSnapshot, bf_write &, buf, int, ev_max)
{
	if (!client->IsHLTV() && !client->IsReplay())
	{
		// send all unreliable temp entities between last and current frame
		// send max 64 events in multi player, 255 in SP
		ev_max = client->GetServer()->IsMultiplayer() ? g_sv_multiplayer_maxtempentities->GetInt() : 255;
	}

	AUTO_LOCK(m_FrameSnapshotsWriteMutex);

	DETOUR_MEMBER_CALL(CBaseServer__WriteTempEntities)(client, pCurrentSnapshot, pLastSnapshot, buf, ev_max);
}

// Memory leak in CFrameSnapshotManager::LevelChanged: it memsets m_pPackedData[] to zero without
// releasing the reference each slot holds (see CreatePackedEntity, the "mru" reference), so every
// PackedEntity still referenced there (its data + change frame list) leaks on every map change.
// Release those references first, then let the original function clear the array.
DETOUR_DECL_MEMBER0(CFrameSnapshotManager__LevelChanged, void)
{
	if (g_sv_ssf_fix_packedentity_leak->GetBool())
	{
		PackedEntityHandle_t *pPackedData = reinterpret_cast<PackedEntityHandle_t *>(reinterpret_cast<uint8_t *>(this) + g_iPackedDataOffset);
		CFrameSnapshotManager *pManager = reinterpret_cast<CFrameSnapshotManager *>(this);

		unsigned int nReleased = 0;
		for (int i = 0; i < MAX_EDICTS; i++)
		{
			if (pPackedData[i] != 0)
			{
				PackedEntityHandle_t handle = pPackedData[i];
				pPackedData[i] = 0;
				(pManager->*g_pfnRemoveEntityReference)(handle);
				nReleased++;
			}
		}

		g_nPackedLastReleased = nReleased;
		g_nPackedTotalReleased += nReleased;
		g_nPackedLevelChanges++;
	}

	DETOUR_MEMBER_CALL(CFrameSnapshotManager__LevelChanged)();
}

CON_COMMAND(sv_ssf_packedentity_stats, "Show how many packed entity references SSF released on level change.")
{
	if (g_Detour_CFrameSnapshotManager__LevelChanged == NULL)
	{
		META_CONPRINTF("[SSF] Packed entity leak fix unavailable on this platform (missing gamedata).\n");
		return;
	}

	META_CONPRINTF("[SSF] Packed entity leak fix %s: last level change released %u references, %llu total over %u level changes.\n",
		g_sv_ssf_fix_packedentity_leak->GetBool() ? "enabled" : "disabled",
		g_nPackedLastReleased, g_nPackedTotalReleased, g_nPackedLevelChanges);
}

// Optional: a platform without these gamedata entries keeps the other fixes.
static void SetupPackedEntityLeakFix()
{
	void *pRemoveEntityReference = NULL;
	if (!g_pGameConf->GetMemSig("CFrameSnapshotManager__RemoveEntityReference", &pRemoveEntityReference) || !pRemoveEntityReference
		|| !g_pGameConf->GetOffset("CFrameSnapshotManager__m_pPackedData", &g_iPackedDataOffset) || g_iPackedDataOffset <= 0)
	{
		smutils->LogMessage(myself, "Packed entity leak fix disabled: missing gamedata for this platform.");
		return;
	}

	void *pLevelChanged = NULL;
	if (!g_pGameConf->GetMemSig("CFrameSnapshotManager__LevelChanged", &pLevelChanged) || !pLevelChanged)
	{
		smutils->LogMessage(myself, "Packed entity leak fix disabled: missing gamedata for this platform.");
		return;
	}

	union
	{
		RemoveEntityReference_t mfp;
		struct
		{
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = pRemoveEntityReference;
	u.s.adjustor = 0;
	g_pfnRemoveEntityReference = u.mfp;

	g_Detour_CFrameSnapshotManager__LevelChanged = DETOUR_CREATE_MEMBER(CFrameSnapshotManager__LevelChanged, "CFrameSnapshotManager__LevelChanged");
	if (!g_Detour_CFrameSnapshotManager__LevelChanged)
	{
		smutils->LogError(myself, "Packed entity leak fix disabled: failed to detour CFrameSnapshotManager__LevelChanged.");
		return;
	}
	g_Detour_CFrameSnapshotManager__LevelChanged->EnableDetour();
}

bool SSF::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	gpGlobals = ismm->GetCGlobals();

    ConVar_Register(0, this);

	return true;
}

bool SSF::SDK_OnLoad(char *error, size_t maxlen, bool late)
{
	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("ssf.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
		{
			snprintf(error, maxlen, "Could not read ssf.games.txt: %s\n", conf_error);
		}
		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_Detour_CBaseServer__WriteTempEntities = DETOUR_CREATE_MEMBER(CBaseServer__WriteTempEntities, "CBaseServer__WriteTempEntities");
	if(!g_Detour_CBaseServer__WriteTempEntities)
	{
		snprintf(error, maxlen, "Failed to detour CBaseServer__WriteTempEntities.\n");
		return false;
	}
	g_Detour_CBaseServer__WriteTempEntities->EnableDetour();

	g_Detour_CFrameSnapshot__ReleaseReference = DETOUR_CREATE_MEMBER(CFrameSnapshot__ReleaseReference, "CFrameSnapshot__ReleaseReference");
	if(!g_Detour_CFrameSnapshot__ReleaseReference)
	{
		snprintf(error, maxlen, "Failed to detour CFrameSnapshot__ReleaseReference.\n");
		return false;
	}
	g_Detour_CFrameSnapshot__ReleaseReference->EnableDetour();

	g_Detour_CFrameSnapshot__CreateEmptySnapshot = DETOUR_CREATE_MEMBER(CFrameSnapshot__CreateEmptySnapshot, "CFrameSnapshot__CreateEmptySnapshot");
	if(!g_Detour_CFrameSnapshot__CreateEmptySnapshot)
	{
		snprintf(error, maxlen, "Failed to detour CFrameSnapshot__CreateEmptySnapshot.\n");
		return false;
	}
	g_Detour_CFrameSnapshot__CreateEmptySnapshot->EnableDetour();

	SetupPackedEntityLeakFix();

	AutoExecConfig(g_pCVar, true);

	return true;
}

void SSF::SDK_OnUnload()
{
	if(g_Detour_CBaseServer__WriteTempEntities)
	{
		g_Detour_CBaseServer__WriteTempEntities->Destroy();
		g_Detour_CBaseServer__WriteTempEntities = NULL;
	}

	if (g_Detour_CFrameSnapshot__ReleaseReference)
	{
		g_Detour_CFrameSnapshot__ReleaseReference->Destroy();
		g_Detour_CFrameSnapshot__ReleaseReference = NULL;
	}

	if (g_Detour_CFrameSnapshot__CreateEmptySnapshot)
	{
		g_Detour_CFrameSnapshot__CreateEmptySnapshot->Destroy();
		g_Detour_CFrameSnapshot__CreateEmptySnapshot = NULL;
	}

	if (g_Detour_CFrameSnapshotManager__LevelChanged)
	{
		g_Detour_CFrameSnapshotManager__LevelChanged->Destroy();
		g_Detour_CFrameSnapshotManager__LevelChanged = NULL;
	}

	gameconfs->CloseGameConfigFile(g_pGameConf);
}

void SSF::SDK_OnAllLoaded()
{
}

bool SSF::RegisterConCommandBase(ConCommandBase *pVar)
{
	/* Always call META_REGCVAR instead of going through the engine. */
    return META_REGCVAR(pVar);
}
