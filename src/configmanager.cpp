// Copyright 2024 Black Tek Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include <algorithm>
#if __has_include("luajit/lua.hpp")
#include <luajit/lua.hpp>
#else
#include <lua.hpp>
#endif

#include "configmanager.h"
#include "game.h"
#include "monster.h"
#include "pugicast.h"
#include <toml++/toml.h>

#if LUA_VERSION_NUM >= 502
#undef lua_strlen
#define lua_strlen lua_rawlen
#endif

extern Game g_game;

namespace {

std::string getGlobalString(lua_State* L, const char* identifier, const char* defaultValue)
{
	lua_getglobal(L, identifier);
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return defaultValue;
	}

	size_t len = lua_strlen(L, -1);
	std::string ret(lua_tostring(L, -1), len);
	lua_pop(L, 1);
	return ret;
}

int32_t getGlobalNumber(lua_State* L, const char* identifier, const int32_t defaultValue = 0)
{
	lua_getglobal(L, identifier);
	if (!lua_isnumber(L, -1)) {
		lua_pop(L, 1);
		return defaultValue;
	}

	int32_t val = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return val;
}

bool getGlobalBoolean(lua_State* L, const char* identifier, const bool defaultValue)
{
	lua_getglobal(L, identifier);
	if (!lua_isboolean(L, -1)) {
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 1);
			return defaultValue;
		}

		size_t len = lua_strlen(L, -1);
		std::string ret(lua_tostring(L, -1), len);
		lua_pop(L, 1);
		return booleanString(ret);
	}

	int val = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return val != 0;
}

float getGlobalFloat(lua_State* L, const char* identifier, const float defaultValue = 0.0f)
{
	lua_getglobal(L, identifier);
	if (!lua_isnumber(L, -1)) {
		lua_pop(L, 1);
		return defaultValue;
	}

	float val = static_cast<float>(lua_tonumber(L, -1));
	lua_pop(L, 1);
	return val;
}

}

ConfigManager::ConfigManager()
{
	string[CONFIG_FILE] = "config.lua";
}

namespace {

ExperienceStages loadLuaStages(lua_State* L)
{
	ExperienceStages stages;

	lua_getglobal(L, "experienceStages");
	if (!lua_istable(L, -1)) {
		return {};
	}

	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		const auto tableIndex = lua_gettop(L);
		auto minLevel = LuaScriptInterface::getField<uint32_t>(L, tableIndex, "minlevel", 1);
		auto maxLevel =	LuaScriptInterface::getField<uint32_t>(L, tableIndex, "maxlevel", std::numeric_limits<uint32_t>::max());
		auto multiplier = LuaScriptInterface::getField<float>(L, tableIndex, "multiplier", 1);
		stages.emplace_back(minLevel, maxLevel, multiplier);
		lua_pop(L, 4);
	}
	lua_pop(L, 1);

	std::sort(stages.begin(), stages.end());
	return stages;
}


ExperienceStages loadTOMLStages()
{
	const std::string filePath = "config/stages.toml";

	ExperienceStages stages;

	try {
		auto tbl = toml::parse_file(filePath);

		// Handle config.enabled
		if (auto config = tbl["config"].as_table()) {
			if (auto enabledNode = config->get("enabled"); enabledNode && enabledNode->is_boolean()) {
				if (!enabledNode->as_boolean()->get()) {
					return {};
				}
			}
		}

		// Handle stage array
		if (auto stageArray = tbl["stage"].as_array()) {
			for (const auto& elem : *stageArray) {
				if (!elem.is_table()) {
					continue;
				}

				const auto& stage = *elem.as_table();
				uint32_t minLevel = stage["minlevel"].value_or(1u);
				uint32_t maxLevel = stage["maxlevel"].value_or(std::numeric_limits<uint32_t>::max());
				uint32_t multiplier = stage["multiplier"].value_or(1u);

				stages.emplace_back(minLevel, maxLevel, multiplier);
			}
		}
	}
	catch (const toml::parse_error& err) {
		std::cerr << "TOML parse error in " << filePath << ": " << err << "\n";
		return {};
	}

	std::sort(stages.begin(), stages.end());
	return stages;
}
}

bool ConfigManager::load()
{
	lua_State* L = luaL_newstate();
	if (!L) {
		throw std::runtime_error("Failed to allocate memory");
	}

	luaL_openlibs(L);

	if (luaL_dofile(L, getString(CONFIG_FILE).c_str())) {
		std::cout << "[Error - ConfigManager::load] " << lua_tostring(L, -1) << std::endl;
		lua_close(L);
		return false;
	}

	//parse config
	if (!loaded) { //info that must be loaded one time (unless we reset the modules involved)
		boolean[BIND_ONLY_GLOBAL_ADDRESS] = getGlobalBoolean(L, "bindOnlyGlobalAddress", false);
		boolean[OPTIMIZE_DATABASE] = getGlobalBoolean(L, "startupDatabaseOptimization", true);

		if (string[IP] == "") {
			string[IP] = getGlobalString(L, "ip", "127.0.0.1");
		}

		string[MAP_NAME] = getGlobalString(L, "mapName", "forgotten");
		string[MAP_AUTHOR] = getGlobalString(L, "mapAuthor", "Unknown");
		string[HOUSE_RENT_PERIOD] = getGlobalString(L, "houseRentPeriod", "never");
		string[MYSQL_HOST] = getGlobalString(L, "mysqlHost", "127.0.0.1");
		string[MYSQL_USER] = getGlobalString(L, "mysqlUser", "forgottenserver");
		string[MYSQL_PASS] = getGlobalString(L, "mysqlPass", "");
		string[MYSQL_DB] = getGlobalString(L, "mysqlDatabase", "forgottenserver");
		string[MYSQL_SOCK] = getGlobalString(L, "mysqlSock", "");

		integer[SQL_PORT] = getGlobalNumber(L, "mysqlPort", 3306);

		if (integer[GAME_PORT] == 0) {
			integer[GAME_PORT] = getGlobalNumber(L, "gameProtocolPort", 7172);
		}

		if (integer[LOGIN_PORT] == 0) {
			integer[LOGIN_PORT] = getGlobalNumber(L, "loginProtocolPort", 7171);
		}

		integer[STATUS_PORT] = getGlobalNumber(L, "statusProtocolPort", 7171);

		integer[MARKET_OFFER_DURATION] = getGlobalNumber(L, "marketOfferDuration", 30 * 24 * 60 * 60);
	}

	boolean[ALLOW_CHANGEOUTFIT] = getGlobalBoolean(L, "allowChangeOutfit", true);
	boolean[ONE_PLAYER_ON_ACCOUNT] = getGlobalBoolean(L, "onePlayerOnlinePerAccount", true);
	boolean[AIMBOT_HOTKEY_ENABLED] = getGlobalBoolean(L, "hotkeyAimbotEnabled", true);
	boolean[REMOVE_RUNE_CHARGES] = getGlobalBoolean(L, "removeChargesFromRunes", true);
	boolean[REMOVE_WEAPON_AMMO] = getGlobalBoolean(L, "removeWeaponAmmunition", true);
	boolean[REMOVE_WEAPON_CHARGES] = getGlobalBoolean(L, "removeWeaponCharges", true);
	boolean[REMOVE_POTION_CHARGES] = getGlobalBoolean(L, "removeChargesFromPotions", true);
	boolean[EXPERIENCE_FROM_PLAYERS] = getGlobalBoolean(L, "experienceByKillingPlayers", false);
	boolean[FREE_PREMIUM] = getGlobalBoolean(L, "freePremium", false);
	boolean[REPLACE_KICK_ON_LOGIN] = getGlobalBoolean(L, "replaceKickOnLogin", true);
	boolean[ALLOW_CLONES] = getGlobalBoolean(L, "allowClones", false);
	boolean[ALLOW_WALKTHROUGH] = getGlobalBoolean(L, "allowWalkthrough", true);
	boolean[MARKET_PREMIUM] = getGlobalBoolean(L, "premiumToCreateMarketOffer", true);
	boolean[EMOTE_SPELLS] = getGlobalBoolean(L, "emoteSpells", false);
	boolean[STAMINA_SYSTEM] = getGlobalBoolean(L, "staminaSystem", true);
	boolean[WARN_UNSAFE_SCRIPTS] = getGlobalBoolean(L, "warnUnsafeScripts", true);
	boolean[CONVERT_UNSAFE_SCRIPTS] = getGlobalBoolean(L, "convertUnsafeScripts", true);
	boolean[CLASSIC_EQUIPMENT_SLOTS] = getGlobalBoolean(L, "classicEquipmentSlots", false);
	boolean[CLASSIC_ATTACK_SPEED] = getGlobalBoolean(L, "classicAttackSpeed", false);
	boolean[SCRIPTS_CONSOLE_LOGS] = getGlobalBoolean(L, "showScriptsLogInConsole", true);
	boolean[SERVER_SAVE_NOTIFY_MESSAGE] = getGlobalBoolean(L, "serverSaveNotifyMessage", true);
	boolean[SERVER_SAVE_CLEAN_MAP] = getGlobalBoolean(L, "serverSaveCleanMap", false);
	boolean[SERVER_SAVE_CLOSE] = getGlobalBoolean(L, "serverSaveClose", false);
	boolean[SERVER_SAVE_SHUTDOWN] = getGlobalBoolean(L, "serverSaveShutdown", true);
	boolean[ONLINE_OFFLINE_CHARLIST] = getGlobalBoolean(L, "showOnlineStatusInCharlist", false);
	boolean[YELL_ALLOW_PREMIUM] = getGlobalBoolean(L, "yellAlwaysAllowPremium", false);
	boolean[PREMIUM_TO_SEND_PRIVATE] = getGlobalBoolean(L, "premiumToSendPrivate", false);
	boolean[FORCE_MONSTERTYPE_LOAD] = getGlobalBoolean(L, "forceMonsterTypesOnLoad", true);
	boolean[DEFAULT_WORLD_LIGHT] = getGlobalBoolean(L, "defaultWorldLight", true);
	boolean[HOUSE_OWNED_BY_ACCOUNT] = getGlobalBoolean(L, "houseOwnedByAccount", false);
	boolean[LUA_ITEM_DESC] = getGlobalBoolean(L, "luaItemDesc", false);
	boolean[CLEAN_PROTECTION_ZONES] = getGlobalBoolean(L, "cleanProtectionZones", false);
	boolean[HOUSE_DOOR_SHOW_PRICE] = getGlobalBoolean(L, "houseDoorShowPrice", true);
	boolean[ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS] = getGlobalBoolean(L, "onlyInvitedCanMoveHouseItems", true);
	boolean[REMOVE_ON_DESPAWN] = getGlobalBoolean(L, "removeOnDespawn", true);
	boolean[PLAYER_CONSOLE_LOGS] = getGlobalBoolean(L, "showPlayerLogInConsole", true);
	boolean[CHECK_DUPLICATE_STORAGE_KEYS] = getGlobalBoolean(L, "checkDuplicateStorageKeys", false);
	boolean[BED_OFFLINE_TRAINING] = getGlobalBoolean(L, "bedOfflineTraining", true);
	boolean[AUGMENT_SLOT_PROTECTION] = getGlobalBoolean(L, "augmentSlotProtection", true);
	boolean[AUGMENT_STAMINA_RULE] = getGlobalBoolean(L, "augmentStaminInMinutes", false);
	boolean[AUGMENT_CRITICAL_ANIMATION] = getGlobalBoolean(L, "showAnimationOnCritHitFromAugment", true);
	boolean[NPC_PZ_WALKTHROUGH] = getGlobalBoolean(L, "allowNpcWalkthroughInPz", false);

	// Account manager
	boolean[ENABLE_ACCOUNT_MANAGER] = getGlobalBoolean(L, "useIngameAccountManager", true);
	boolean[ENABLE_NO_PASS_LOGIN] = getGlobalBoolean(L, "allowNoPassLogin", true);
	string[ACCOUNT_MANAGER_AUTH] = getGlobalString(L, "accountManagerPassword", "1");
	integer[ACCOUNT_MANAGER_POS_X] = getGlobalNumber(L, "managerPositionX", 0);
	integer[ACCOUNT_MANAGER_POS_Y] = getGlobalNumber(L, "managerPositionY", 0);
	integer[ACCOUNT_MANAGER_POS_Z] = getGlobalNumber(L, "managerPositionZ", 0);

	string[DEFAULT_PRIORITY] = getGlobalString(L, "defaultPriority", "high");
	string[SERVER_NAME] = getGlobalString(L, "serverName", "");
	string[OWNER_NAME] = getGlobalString(L, "ownerName", "");
	string[OWNER_EMAIL] = getGlobalString(L, "ownerEmail", "");
	string[URL] = getGlobalString(L, "url", "");
	string[LOCATION] = getGlobalString(L, "location", "");
	string[MOTD] = getGlobalString(L, "motd", "");
	string[WORLD_TYPE] = getGlobalString(L, "worldType", "pvp");

	integer[MAX_PLAYERS] = getGlobalNumber(L, "maxPlayers");
	integer[PZ_LOCKED] = getGlobalNumber(L, "pzLocked", 60000);
	integer[DEFAULT_DESPAWNRANGE] = Monster::despawnRange = getGlobalNumber(L, "deSpawnRange", 2);
	integer[DEFAULT_DESPAWNRADIUS] = Monster::despawnRadius = getGlobalNumber(L, "deSpawnRadius", 50);
	integer[DEFAULT_WALKTOSPAWNRADIUS] = getGlobalNumber(L, "walkToSpawnRadius", 15);
	integer[RATE_EXPERIENCE] = getGlobalNumber(L, "rateExp", 5);
	integer[RATE_SKILL] = getGlobalNumber(L, "rateSkill", 3);
	integer[RATE_LOOT] = getGlobalNumber(L, "rateLoot", 2);
	integer[RATE_MAGIC] = getGlobalNumber(L, "rateMagic", 3);
	integer[RATE_SPAWN] = getGlobalNumber(L, "rateSpawn", 1);
	integer[HOUSE_PRICE] = getGlobalNumber(L, "housePriceEachSQM", 1000);
	integer[KILLS_TO_RED] = getGlobalNumber(L, "killsToRedSkull", 3);
	integer[KILLS_TO_BLACK] = getGlobalNumber(L, "killsToBlackSkull", 6);
	integer[ACTIONS_DELAY_INTERVAL] = getGlobalNumber(L, "timeBetweenActions", 200);
	integer[EX_ACTIONS_DELAY_INTERVAL] = getGlobalNumber(L, "timeBetweenExActions", 1000);
	integer[MAX_MESSAGEBUFFER] = getGlobalNumber(L, "maxMessageBuffer", 4);
	integer[KICK_AFTER_MINUTES] = getGlobalNumber(L, "kickIdlePlayerAfterMinutes", 15);
	integer[PROTECTION_LEVEL] = getGlobalNumber(L, "protectionLevel", 1);
	integer[DEATH_LOSE_PERCENT] = getGlobalNumber(L, "deathLosePercent", -1);
	integer[STATUSQUERY_TIMEOUT] = getGlobalNumber(L, "statusTimeout", 5000);
	integer[FRAG_TIME] = getGlobalNumber(L, "timeToDecreaseFrags", 24 * 60 * 60);
	integer[WHITE_SKULL_TIME] = getGlobalNumber(L, "whiteSkullTime", 15 * 60);
	integer[STAIRHOP_DELAY] = getGlobalNumber(L, "stairJumpExhaustion", 2000);
	integer[EXP_FROM_PLAYERS_LEVEL_RANGE] = getGlobalNumber(L, "expFromPlayersLevelRange", 75);
	integer[CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES] = getGlobalNumber(L, "checkExpiredMarketOffersEachMinutes", 60);
	integer[MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER] = getGlobalNumber(L, "maxMarketOffersAtATimePerPlayer", 100);
	integer[MAX_PACKETS_PER_SECOND] = getGlobalNumber(L, "maxPacketsPerSecond", 25);
	integer[SERVER_SAVE_NOTIFY_DURATION] = getGlobalNumber(L, "serverSaveNotifyDuration", 5);
	integer[YELL_MINIMUM_LEVEL] = getGlobalNumber(L, "yellMinimumLevel", 2);
	integer[MINIMUM_LEVEL_TO_SEND_PRIVATE] = getGlobalNumber(L, "minimumLevelToSendPrivate", 1);
	integer[VIP_FREE_LIMIT] = getGlobalNumber(L, "vipFreeLimit", 20);
	integer[VIP_PREMIUM_LIMIT] = getGlobalNumber(L, "vipPremiumLimit", 100);
	integer[DEPOT_FREE_LIMIT] = getGlobalNumber(L, "depotFreeLimit", 2000);
	integer[DEPOT_PREMIUM_LIMIT] = getGlobalNumber(L, "depotPremiumLimit", 10000);

	floats[REWARD_BASE_RATE] = getGlobalFloat(L, "rewardBaseRate", 1.0f);
	floats[REWARD_RATE_DAMAGE_DONE] = getGlobalFloat(L, "rewardRateDamageDone", 1.0f);
	floats[REWARD_RATE_DAMAGE_TAKEN] = getGlobalFloat(L, "rewardRateDamageTaken", 1.0f);
	floats[REWARD_RATE_HEALING_DONE] = getGlobalFloat(L, "rewardRateHealingDone", 1.0f);

	expStages = loadTOMLStages();
	if (expStages.empty()) {
		expStages = loadLuaStages(L);
	}
	expStages.shrink_to_fit();

	loaded = true;
	lua_close(L);

	return true;
}

bool ConfigManager::reload()
{
	bool result = load();
	if (transformToSHA1(getString(ConfigManager::MOTD)) != g_game.getMotdHash()) {
		g_game.incrementMotdNum();
	}
	return result;
}

static std::string dummyStr;

const std::string& ConfigManager::getString(string_config_t what) const
{
	if (what >= LAST_STRING_CONFIG) {
		std::cout << "[Warning - ConfigManager::getString] Accessing invalid index: " << what << std::endl;
		return dummyStr;
	}
	return string[what];
}

int32_t ConfigManager::getNumber(integer_config_t what) const
{
	if (what >= LAST_INTEGER_CONFIG) {
		std::cout << "[Warning - ConfigManager::getNumber] Accessing invalid index: " << what << std::endl;
		return 0;
	}
	return integer[what];
}

bool ConfigManager::getBoolean(boolean_config_t what) const
{
	if (what >= LAST_BOOLEAN_CONFIG) {
		std::cout << "[Warning - ConfigManager::getBoolean] Accessing invalid index: " << what << std::endl;
		return false;
	}
	return boolean[what];
}

float ConfigManager::getExperienceStage(uint32_t level) const
{
	auto it = std::find_if(expStages.begin(), expStages.end(), [level](auto&& stage) {
		auto&& [minLevel, maxLevel, _] = stage;
		return level >= minLevel && level <= maxLevel;
	});

	if (it == expStages.end()) {
		return getNumber(ConfigManager::RATE_EXPERIENCE);
	}

	return std::get<2>(*it);
}

bool ConfigManager::setString(string_config_t what, std::string_view value)
{
	if (what >= LAST_STRING_CONFIG) {
		std::cout << "[Warning - ConfigManager::setString] Accessing invalid index: " << what << std::endl;
		return false;
	}

	string[what] = value;
	return true;
}

bool ConfigManager::setNumber(integer_config_t what, int32_t value)
{
	if (what >= LAST_INTEGER_CONFIG) {
		std::cout << "[Warning - ConfigManager::setNumber] Accessing invalid index: " << what << std::endl;
		return false;
	}

	integer[what] = value;
	return true;
}

bool ConfigManager::setBoolean(boolean_config_t what, bool value)
{
	if (what >= LAST_BOOLEAN_CONFIG) {
		std::cout << "[Warning - ConfigManager::setBoolean] Accessing invalid index: " << what << std::endl;
		return false;
	}

	boolean[what] = value;
	return true;
}

float ConfigManager::getFloat(float_config_t what) const
{
	if (what >= LAST_FLOAT_CONFIG) {
		std::cout << "[Warning - ConfigManager::getFloat] Accessing invalid index: " << what << std::endl;
		return 0.0f;
	}
	return floats[what];
}

bool ConfigManager::setFloat(float_config_t what, float value)
{
	if (what >= LAST_FLOAT_CONFIG) {
		std::cout << "[Warning - ConfigManager::setFloat] Accessing invalid index: " << what << std::endl;
		return false;
	}
	floats[what] = value;
	return true;
}