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
#include "extensionHelper.h"
#include "CDetour/detours.h"
#include "steam/steam_gameserver.h"
#include "sm_namehashset.h"
#include <sourcehook.h>
#include <bitbuf.h>
#include <netadr.h>
#include <ISDKTools.h>
#include <iserver.h>
#include <iclient.h>
#include <iplayerinfo.h>
#include <ihltvdirector.h>
#include <ihltv.h>
#include <inetchannelinfo.h>
#include <sys/socket.h>
#include <netinet/in.h>

size_t
strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc = src;
	size_t nleft = dsize;

	/* Copy as many bytes as will fit. */
	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}

	/* Not enough room in dst, add NUL and traverse rest of src. */
	if (nleft == 0) {
		if (dsize != 0)
			*dst = '\0';		/* NUL-terminate dst */
		while (*src++)
			;
	}

	return(src - osrc - 1);	/* count does not include NUL */
}

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

A2SQCache g_A2SQCache;		/**< Global singleton for extension's main interface */
A2SQCacheEvents g_A2SQCacheEvents;
A2SQCacheTimer g_A2SQCacheTimer;

SMEXT_LINK(&g_A2SQCache);

ConVar *g_SvLogging = CreateConVar("sv_qcache_logging", "0", FCVAR_NOTIFY, "Log connection checks.");
ConVar *g_SvGameDesc = CreateConVar("sv_gamedesc_override", "default", FCVAR_NOTIFY, "Overwrite the default game description. Set to 'default' to keep default description.");
ConVar *g_SvMapName = CreateConVar("sv_mapname_override", "default", FCVAR_NOTIFY, "Overwrite the map name. Set to 'default' to keep default name.");
ConVar *g_SvCountBotsInfo = CreateConVar("sv_count_bots_info", "1", FCVAR_NOTIFY, "Display bots as players in the a2s_info server query. Enable = '1', Disable = '0'");
ConVar *g_SvCountBotsPlayer = CreateConVar("sv_count_bots_player", "0", FCVAR_NOTIFY, "Display bots as players in the a2s_player server query. Enable = '1', Disable = '0'");
ConVar *g_pSvVisibleMaxPlayers;
ConVar *g_pSvTags;

IGameConfig *g_pGameConf = NULL;
IGameEventManager2 *g_pGameEvents = NULL;
ITimer *g_pA2SQCacheTimer = NULL;
ISDKTools *g_pSDKTools = NULL;
IServer *iserver = NULL;
CGlobalVars *gpGlobals = NULL;
IHLTVDirector *hltvdirector = NULL;
IHLTVServer *hltv = NULL;
double *net_time = NULL;
#if SOURCE_ENGINE == SE_CSGO
const char **g_sVersionString = NULL;
#endif

uint8_t g_UserIDtoClientMap[USHRT_MAX + 1];

typedef struct netpacket_s
{
	netadr_t		from;		// sender IP
#if SOURCE_ENGINE >= SE_CSGO
	// ns_address hack
	struct // CPeerToPeerAddress
	{
		CSteamID m_steamID;
		int m_steamChannel;
		int m_AddrType;
	} m_steamID; // SteamID destination
	int m_AddrType; // NetworkSystemAddressType_t
#endif
	int				source;		// received source
	double			received;	// received time
	unsigned char	*data;		// pointer to raw packet data
	bf_read			message;	// easy bitbuf data access
	int				size;		// size in bytes
	int				wiresize;   // size in bytes before decompression
	bool			stream;		// was send as stream
	struct netpacket_s *pNext;	// for internal use, should be NULL in public
} netpacket_t;

typedef struct
{
	int			nPort;		// UDP/TCP use same port number
#if SOURCE_ENGINE >= SE_CSGO
	int			hUDP;
	char		pad[4*15];
#else
	bool		bListening;	// true if TCP port is listening
	int			hUDP;		// handle to UDP socket from socket()
	int			hTCP;		// handle to TCP socket from socket()
#endif
} netsocket_t;

CUtlVector<netsocket_t> *net_sockets;
int g_ServerUDPSocket = 0;

CDetour *g_Detour_CBaseServer__InactivateClients = NULL;
SH_DECL_MANUALHOOK1(ProcessConnectionlessPacket, 0, 0, 0, bool, netpacket_t *); // virtual bool IServer::ProcessConnectionlessPacket( netpacket_t *packet ) = 0;

void *s_queryRateChecker = NULL;
bool (*CIPRateLimit__CheckIP)(void *pThis, netadr_t adr);
//bool (*CBaseServer__ValidChallenge)(void *pThis, netadr_t adr, int challengeNr);

struct CQueryCache
{
	struct CPlayer
	{
		bool active;
		bool fake;
		int userid;
		IClient *pClient;
		char name[MAX_PLAYER_NAME_LENGTH];
		unsigned nameLen;
		int32_t score;
		double time;
	} players[SM_MAXPLAYERS + 1];

	struct CInfo
	{
		uint8_t nProtocol = 17; // Protocol | byte | Protocol version used by the server.
		char aHostName[255]; // Name | string | Name of the server.
		uint8_t aHostNameLen;
		char aMapName[255]; // Map | string | Map the server has currently loaded.
		uint8_t aMapNameLen;
		char aGameDir[255]; // Folder | string | Name of the folder containing the game files.
		uint8_t aGameDirLen;
		char aGameDescription[255]; // Game | string | Full name of the game.
		uint8_t aGameDescriptionLen;
		uint16_t iSteamAppID; // ID | short | Steam Application ID of game.
		uint8_t nNumClients = 0; // Players | byte | Number of players on the server.
		uint8_t nMaxClients; // Max. Players | byte | Maximum number of players the server reports it can hold.
		uint8_t nFakeClients = 0; // Bots | byte | Number of bots on the server.
		uint8_t nServerType = 'd'; // Server type | byte | Indicates the type of server: 'd' for a dedicated server, 'l' for a non-dedicated server, 'p' for a SourceTV relay (proxy)
		uint8_t nEnvironment = 'l'; // Environment | byte | Indicates the operating system of the server: 'l' for Linux, 'w' for Windows, 'm' or 'o' for Mac (the code changed after L4D1)
		uint8_t nPassword; // Visibility | byte | Indicates whether the server requires a password: 0 for public, 1 for private
		uint8_t bIsSecure; // VAC | byte | Specifies whether the server uses VAC: 0 for unsecured, 1 for secured
		char aVersion[40]; // Version | string | Version of the game installed on the server.
		uint8_t aVersionLen;
		uint8_t nNewFlags = 0; // Extra Data Flag (EDF) | byte | If present, this specifies which additional data fields will be included.
		uint16_t iUDPPort; // EDF & 0x80 -> Port | short | The server's game port number.
		uint64_t iSteamID; // EDF & 0x10 -> SteamID | long long | Server's SteamID.
		uint16_t iHLTVUDPPort; // EDF & 0x40 -> Port | short | Spectator port number for SourceTV.
		char aHLTVName[255]; // EDF & 0x40 -> Name | string | Name of the spectator server for SourceTV.
		uint8_t aHLTVNameLen;
		char aKeywords[255]; // EDF & 0x20 -> Keywords | string | Tags that describe the game according to the server (for future use.) (sv_tags)
		uint8_t aKeywordsLen;
		uint64_t iGameID; // EDF & 0x01 -> GameID | long long | The server's 64-bit GameID. If this is present, a more accurate AppID is present in the low 24 bits. The earlier AppID could have been truncated as it was forced into 16-bit storage.
	} info;

	uint8_t info_cache[sizeof(CInfo)] = {0xFF, 0xFF, 0xFF, 0xFF, 'I'};
	uint16_t info_cache_len;

	uint8_t players_cache[4+1+1+SM_MAXPLAYERS*(1+MAX_PLAYER_NAME_LENGTH+4+4)] = {0xFF, 0xFF, 0xFF, 0xFF, 'D', 0};
	uint16_t players_cache_len;
} g_QueryCache;

class CBaseClient;
class CBaseServer;


void UpdateQueryCache()
{
	// A2S_INFO
	CQueryCache::CInfo &info = g_QueryCache.info;
	info.aHostNameLen = strlcpy(info.aHostName, iserver->GetName(), sizeof(info.aHostName));

	if(strcmp(g_SvMapName->GetString(), "default") == 0)
		info.aMapNameLen = strlcpy(info.aMapName, iserver->GetMapName(), sizeof(info.aMapName));
	else
		info.aMapNameLen = strlcpy(info.aMapName, g_SvMapName->GetString(), sizeof(info.aMapName));

	if(strcmp(g_SvGameDesc->GetString(), "default") == 0)
		info.aGameDescriptionLen = strlcpy(info.aGameDescription, gamedll->GetGameDescription(), sizeof(info.aGameDescription));
	else
		info.aGameDescriptionLen = strlcpy(info.aGameDescription, g_SvGameDesc->GetString(), sizeof(info.aGameDescription));

	if(g_pSvVisibleMaxPlayers->GetInt() >= 0)
		info.nMaxClients = g_pSvVisibleMaxPlayers->GetInt();
	else
		info.nMaxClients = iserver->GetMaxClients();
	info.nPassword = iserver->GetPassword() ? 1 : 0;
	info.bIsSecure = true;

	if(!(info.nNewFlags & 0x10) && engine->GetGameServerSteamID())
	{
		info.iSteamID = engine->GetGameServerSteamID()->ConvertToUint64();
		info.nNewFlags |= 0x10;
	}

	if(!(info.nNewFlags & 0x40) && hltvdirector->IsActive()) // tv_name can't change anymore
	{
#if SOURCE_ENGINE >= SE_CSGO
		hltv = hltvdirector->GetHLTVServer(0);
#else
		hltv = hltvdirector->GetHLTVServer();
#endif
		if(hltv)
		{
			IServer *ihltvserver = hltv->GetBaseServer();
			if(ihltvserver)
			{
				info.iHLTVUDPPort = ihltvserver->GetUDPPort();
				info.aHLTVNameLen = strlcpy(info.aHLTVName, ihltvserver->GetName(), sizeof(info.aHLTVName));
				info.nNewFlags |= 0x40;
			}
		}
	}

	info.aKeywordsLen = strlcpy(info.aKeywords, g_pSvTags->GetString(), sizeof(info.aKeywords));
	if(info.aKeywordsLen)
		info.nNewFlags |= 0x20;
	else
		info.nNewFlags &= ~0x20;


	uint8_t *info_cache = g_QueryCache.info_cache;
	uint16_t pos = 5; // header: FF FF FF FF I

	info_cache[pos++] = info.nProtocol;

	memcpy(&info_cache[pos], info.aHostName, info.aHostNameLen + 1);
	pos += info.aHostNameLen + 1;

	memcpy(&info_cache[pos], info.aMapName, info.aMapNameLen + 1);
	pos += info.aMapNameLen + 1;

	memcpy(&info_cache[pos], info.aGameDir, info.aGameDirLen + 1);
	pos += info.aGameDirLen + 1;

	memcpy(&info_cache[pos], info.aGameDescription, info.aGameDescriptionLen + 1);
	pos += info.aGameDescriptionLen + 1;

	*(uint16_t *)&info_cache[pos] = info.iSteamAppID;
	pos += 2;

	info_cache[pos++] = info.nNumClients;

	info_cache[pos++] = info.nMaxClients;

	if (g_SvCountBotsInfo->GetInt())
		info_cache[pos++] = 0;
	else
		info_cache[pos++] = info.nFakeClients;

	info_cache[pos++] = info.nServerType;

	info_cache[pos++] = info.nEnvironment;

	info_cache[pos++] = info.nPassword;

	info_cache[pos++] = info.bIsSecure;

	memcpy(&info_cache[pos], info.aVersion, info.aVersionLen + 1);
	pos += info.aVersionLen + 1;

	info_cache[pos++] = info.nNewFlags;

	if(info.nNewFlags & 0x80) {
		*(uint16_t *)&info_cache[pos] = info.iUDPPort;
		pos += 2;
	}

	if(info.nNewFlags & 0x10) {
		*(uint64_t *)&info_cache[pos] = info.iSteamID;
		pos += 8;
	}

	if(info.nNewFlags & 0x40) {
		*(uint16_t *)&info_cache[pos] = info.iHLTVUDPPort;
		pos += 2;

		memcpy(&info_cache[pos], info.aHLTVName, info.aHLTVNameLen + 1);
		pos += info.aHLTVNameLen + 1;
	}

	if(info.nNewFlags & 0x20) {
		memcpy(&info_cache[pos], info.aKeywords, info.aKeywordsLen + 1);
		pos += info.aKeywordsLen + 1;
	}

	if(info.nNewFlags & 0x01) {
		*(uint64_t *)&info_cache[pos] = info.iGameID;
		pos += 8;
	}

	g_QueryCache.info_cache_len = pos;


	// A2S_PLAYER
	uint8_t *players_cache = g_QueryCache.players_cache;
	pos = 6; // header: FF FF FF FF D 0[numplayers]
	for(int i = 1; i <= SM_MAXPLAYERS; i++)
	{
		const CQueryCache::CPlayer &player = g_QueryCache.players[i];
		if(!player.active || (player.fake && !g_SvCountBotsPlayer->GetInt()))
			continue;

		players_cache[pos++] = players_cache[5]; // Index | byte | Index of player chunk starting from 0.
		players_cache[5]++; // Players | byte | Number of players whose information was gathered.
		memcpy(&players_cache[pos], player.name, player.nameLen + 1); // Name | string | Name of the player.
		pos += player.nameLen + 1;
		*(int32_t *)&players_cache[pos] = player.score; // Score | long | Player's score (usually "frags" or "kills".)
		pos += 4;
		*(float *)&players_cache[pos] = *net_time - player.time; // Duration | float | Time (in seconds) player has been connected to the server.
		pos += 4;
	}

	g_QueryCache.players_cache_len = pos;
}

bool Hook_ProcessConnectionlessPacket(netpacket_t * packet)
{
	if(packet->size >= 25 && packet->data[4] == 'T')
	{
		if(!CIPRateLimit__CheckIP(s_queryRateChecker, packet->from))
		{
			RETURN_META_VALUE(MRES_SUPERCEDE, false);
		}

		sockaddr_in to;
		to.sin_family = AF_INET;
		to.sin_port = packet->from.port;
		to.sin_addr.s_addr = *(int32_t *)&packet->from.ip;

		sendto(g_ServerUDPSocket, g_QueryCache.info_cache, g_QueryCache.info_cache_len, 0, (sockaddr *)&to, sizeof(to));

		RETURN_META_VALUE(MRES_SUPERCEDE, true);
	}

	if((packet->size == 5 || packet->size == 9) && packet->data[4] == 'U')
	{
		if(!CIPRateLimit__CheckIP(s_queryRateChecker, packet->from))
		{
			RETURN_META_VALUE(MRES_SUPERCEDE, false);
		}

		sockaddr_in to;
		to.sin_family = AF_INET;
		to.sin_port = packet->from.port;
		to.sin_addr.s_addr = *(int32_t *)&packet->from.ip;

		int32_t challengeNr = -1;
		if(packet->size == 9)
			challengeNr = *(int32_t *)&packet->data[5];

		/* TODO
		 * This is a complete nonsense challenge as it doesn't offer any protection at all.
		 * The point of this challenge is to stop spoofed source UDP DDoS reflection attacks,
		 * so it doesn't really matter if one single server out of thousands doesn't
		 * implement this correctly. If you do happen to use this on thousands of servers
		 * though then please do implement it correctly.
		 */
		int32_t realChallengeNr = *(int32_t *)&packet->from.ip ^ 0x55AADD88;
		if(challengeNr != realChallengeNr)
		{
			uint8_t response[9] = {0xFF, 0xFF, 0xFF, 0xFF, 'A'};
			*(int32_t *)&response[5] = realChallengeNr;
			sendto(g_ServerUDPSocket, response, sizeof(response), 0, (sockaddr *)&to, sizeof(to));
			RETURN_META_VALUE(MRES_SUPERCEDE, true);
		}

		sendto(g_ServerUDPSocket, g_QueryCache.players_cache, g_QueryCache.players_cache_len, 0, (sockaddr *)&to, sizeof(to));

		RETURN_META_VALUE(MRES_SUPERCEDE, true);
	}

	RETURN_META_VALUE(MRES_IGNORED, false);
}

DETOUR_DECL_MEMBER0(CBaseServer__InactivateClients, void)
{
	for(int slot = 0; slot < iserver->GetClientCount(); slot++)
	{
		int client = slot + 1;
		IClient *pClient = iserver->GetClient(slot);
		if(!pClient)
			continue;

		// Disconnect all fake clients manually before the engine just nukes them.
		if(pClient->IsFakeClient() && !pClient->IsHLTV())
		{
			pClient->Disconnect("");
		}
	}

	return DETOUR_MEMBER_CALL(CBaseServer__InactivateClients)();
}

bool A2SQCache::SDK_OnLoad(char *error, size_t maxlen, bool late)
{
	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("a2sqcache.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
		{
			snprintf(error, maxlen, "Could not read a2sqcache.games.txt: %s\n", conf_error);
		}
		return false;
	}

	if(!g_pGameConf->GetMemSig("CIPRateLimit__CheckIP", (void **)&CIPRateLimit__CheckIP) || !CIPRateLimit__CheckIP)
	{
		snprintf(error, maxlen, "Failed to find CIPRateLimit::CheckIP address.\n");
		return false;
	}

	if(!g_pGameConf->GetAddress("s_queryRateChecker", &s_queryRateChecker) || !s_queryRateChecker)
	{
		snprintf(error, maxlen, "Failed to find s_queryRateChecker address.\n");
		return false;
	}

	if(!g_pGameConf->GetAddress("net_sockets", (void **)&net_sockets) || !net_sockets)
	{
		snprintf(error, maxlen, "Failed to find net_sockets address.\n");
		return false;
	}

	if(!g_pGameConf->GetAddress("net_time", (void **)&net_time) || !net_time)
	{
		snprintf(error, maxlen, "Failed to find net_time address.\n");
		return false;
	}

#if SOURCE_ENGINE == SE_CSGO
	if(!g_pGameConf->GetAddress("g_sVersionString", (void **)&g_sVersionString) || !g_sVersionString)
	{
		snprintf(error, maxlen, "Failed to find g_sVersionString address.\n");
		return false;
	}
#endif

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_Detour_CBaseServer__InactivateClients = DETOUR_CREATE_MEMBER(CBaseServer__InactivateClients, "CBaseServer__InactivateClients");
	if(!g_Detour_CBaseServer__InactivateClients)
	{
		snprintf(error, maxlen, "Failed to detour CBaseServer__InactivateClients.\n");
		return false;
	}
	g_Detour_CBaseServer__InactivateClients->EnableDetour();

	g_pGameEvents->AddListener(&g_A2SQCacheEvents, "player_connect", true);
	g_pGameEvents->AddListener(&g_A2SQCacheEvents, "player_disconnect", true);
	g_pGameEvents->AddListener(&g_A2SQCacheEvents, "player_changename", true);

	playerhelpers->AddClientListener(this);

	AutoExecConfig(g_pCVar, true);

	return true;
}

bool A2SQCache::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_ANY(GetServerFactory, gamedll, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameEvents, IGameEventManager2, INTERFACEVERSION_GAMEEVENTSMANAGER2);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, hltvdirector, IHLTVDirector, INTERFACEVERSION_HLTVDIRECTOR);

	gpGlobals = ismm->GetCGlobals();
	ConVar_Register(0, this);

	g_pSvVisibleMaxPlayers = g_pCVar->FindVar("sv_visiblemaxplayers");
	g_pSvTags = g_pCVar->FindVar("sv_tags");

	return true;
}

void A2SQCache::SDK_OnUnload()
{
	if(g_Detour_CBaseServer__InactivateClients)
	{
		g_Detour_CBaseServer__InactivateClients->Destroy();
		g_Detour_CBaseServer__InactivateClients = NULL;
	}

	g_pGameEvents->RemoveListener(&g_A2SQCacheEvents);

	playerhelpers->RemoveClientListener(this);

	if(g_pA2SQCacheTimer)
		timersys->KillTimer(g_pA2SQCacheTimer);

	gameconfs->CloseGameConfigFile(g_pGameConf);
}

bool A2SQCache::RegisterConCommandBase(ConCommandBase *pVar)
{
	/* Always call META_REGCVAR instead of going through the engine. */
	return META_REGCVAR(pVar);
}

void A2SQCache::SDK_OnAllLoaded()
{
	SM_GET_LATE_IFACE(SDKTOOLS, g_pSDKTools);

	iserver = g_pSDKTools->GetIServer();
	if (!iserver) {
		smutils->LogError(myself, "Failed to get IServer interface from SDKTools!");
		return;
	}

	int socknum = 1; // NS_SERVER
	g_ServerUDPSocket = (*net_sockets)[socknum].hUDP;

	if(!g_ServerUDPSocket)
	{
		smutils->LogError(myself, "Failed to find server UDP socket.");
		return;
	}

	int offset;
	if (g_pGameConf->GetOffset("IServer__ProcessConnectionlessPacket", &offset))
	{
		SH_MANUALHOOK_RECONFIGURE(ProcessConnectionlessPacket, offset, 0, 0);
		SH_ADD_MANUALHOOK(ProcessConnectionlessPacket, iserver, SH_STATIC(Hook_ProcessConnectionlessPacket), false);
	}
	else
	{
		smutils->LogError(myself, "Failed to find IServer::ProcessConnectionlessPacket offset.");
		return;
	}

	g_pA2SQCacheTimer = timersys->CreateTimer(&g_A2SQCacheTimer, 1.0, NULL, TIMER_FLAG_REPEAT);

	// A2S_INFO
	CQueryCache::CInfo &info = g_QueryCache.info;
	info.aGameDirLen = strlcpy(info.aGameDir, smutils->GetGameFolderName(), sizeof(info.aGameDir));

#if SOURCE_ENGINE == SE_CSGO
	info.iSteamAppID = 730; // wtf valve
	info.aVersionLen = snprintf(info.aVersion, sizeof(info.aVersion), "%s", *g_sVersionString);
#else
	info.iSteamAppID = engine->GetAppID();
	info.aVersionLen = snprintf(info.aVersion, sizeof(info.aVersion), "%d", engine->GetServerVersion());
#endif

	info.iUDPPort = iserver->GetUDPPort();
	info.nNewFlags |= 0x80;

	info.iGameID = info.iSteamAppID;
	info.nNewFlags |= 0x01;

	UpdateQueryCache();

	// A2S_PLAYER
	for(int slot = 0; slot < iserver->GetClientCount(); slot++)
	{
		int client = slot + 1;
		IClient *pClient = iserver->GetClient(slot);
		if(!pClient || !pClient->IsConnected())
			continue;

		CQueryCache::CPlayer &player = g_QueryCache.players[client];
		IGamePlayer *gplayer = playerhelpers->GetGamePlayer(client);

		if(!player.active)
		{
			g_QueryCache.info.nNumClients++;
			if(pClient->IsFakeClient() && !pClient->IsHLTV() && (!gplayer || (gplayer->IsConnected() && !gplayer->IsSourceTV())))
			{
				g_QueryCache.info.nFakeClients++;
				player.fake = true;
			}
		}

		player.active = true;
		player.pClient = pClient;
		player.nameLen = strlcpy(player.name, pClient->GetClientName(), sizeof(player.name));

		INetChannelInfo *netinfo = (INetChannelInfo *)player.pClient->GetNetChannel();
		if(netinfo)
			player.time = *net_time - netinfo->GetTimeConnected();
		else
			player.time = 0;

		if(gplayer && gplayer->IsConnected())
		{
			IPlayerInfo *info = gplayer->GetPlayerInfo();
			if(info)
				player.score = info->GetFragCount();
			else
				player.score = 0;
		}

		g_UserIDtoClientMap[pClient->GetUserID()] = client;
	}
}

void A2SQCache::OnClientSettingsChanged(int client)
{
	if(client >= 1 && client <= SM_MAXPLAYERS)
	{
		CQueryCache::CPlayer &player = g_QueryCache.players[client];
		if(player.active && player.pClient)
			player.nameLen = strlcpy(player.name, player.pClient->GetClientName(), sizeof(player.name));
	}
}

void A2SQCache::OnClientPutInServer(int client)
{
	if(client >= 1 && client <= SM_MAXPLAYERS)
	{
		CQueryCache::CPlayer &player = g_QueryCache.players[client];
		IGamePlayer *gplayer = playerhelpers->GetGamePlayer(client);
		if(player.active && player.fake && gplayer->IsSourceTV())
		{
			player.fake = false;
			g_QueryCache.info.nFakeClients--;
		}
	}
}

void A2SQCache::OnTimer()
{
	for(int client = 1; client <= SM_MAXPLAYERS; client++)
	{
		CQueryCache::CPlayer &player = g_QueryCache.players[client];
		if(!player.active)
			continue;

		IGamePlayer *gplayer = playerhelpers->GetGamePlayer(client);
		if(!gplayer || !gplayer->IsConnected())
			continue;

		IPlayerInfo *info = gplayer->GetPlayerInfo();
		if(info)
			player.score = info->GetFragCount();
	}

	UpdateQueryCache();
}

void A2SQCacheEvents::FireGameEvent(IGameEvent *event)
{
	const char *name = event->GetName();

	if(strcmp(name, "player_connect") == 0)
	{
		const int client = event->GetInt("index") + 1;
		const int userid = event->GetInt("userid");
		const bool bot = event->GetBool("bot");
		const char *name = event->GetString("name");

		if (g_SvLogging->GetInt())
			g_pSM->LogMessage(myself, "player_connect(client=%d, userid=%d, bot=%d, name=%s)", client, userid, bot, name);

		if(client >= 1 && client <= SM_MAXPLAYERS)
		{
			CQueryCache::CPlayer &player = g_QueryCache.players[client];

			player.active = true;
			player.fake = false;
			player.pClient = iserver->GetClient(client - 1);
			g_QueryCache.info.nNumClients++;
			if(bot)
			{
				player.fake = true;
				g_QueryCache.info.nFakeClients++;
			}
			player.time = *net_time;
			player.score = 0;
			player.nameLen = strlcpy(player.name, player.pClient->GetClientName(), sizeof(player.name));

			g_UserIDtoClientMap[userid] = client;

			if (g_SvLogging->GetInt())
				g_pSM->LogMessage(myself, "\tCPlayer(active=%d, fake=%d, pClient=%p, name=%s)", player.active, player.fake, player.pClient, player.name);
		}

	}
	else if(strcmp(name, "player_disconnect") == 0)
	{
		const int userid = event->GetInt("userid");
		const int client = g_UserIDtoClientMap[userid];
		g_UserIDtoClientMap[userid] = 0;

		if (g_SvLogging->GetInt())
			g_pSM->LogMessage(myself, "player_disconnect(userid=%d, client=%d)", userid, client);

		if(client >= 1 && client <= SM_MAXPLAYERS)
		{
			CQueryCache::CPlayer &player = g_QueryCache.players[client];
			if (g_SvLogging->GetInt())
				g_pSM->LogMessage(myself, "\tCPlayer(active=%d, fake=%d, pClient=%p, name=%s)", player.active, player.fake, player.pClient, player.name);

			if(player.active)
			{
				g_QueryCache.info.nNumClients--;
				if(player.fake)
					g_QueryCache.info.nFakeClients--;
			}
			player.active = false;
			player.pClient = NULL;
		}
	}
	else if(strcmp(name, "player_changename") == 0)
	{
		const int userid = event->GetInt("userid");
		const int client = g_UserIDtoClientMap[userid];

		g_A2SQCache.OnClientSettingsChanged(client);
	}
}

ResultType A2SQCacheTimer::OnTimer(ITimer *pTimer, void *pData)
{
	g_A2SQCache.OnTimer();
	return Pl_Continue;
}
void A2SQCacheTimer::OnTimerEnd(ITimer *pTimer, void *pData) {}
