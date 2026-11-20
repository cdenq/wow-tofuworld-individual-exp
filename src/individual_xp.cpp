#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Object.h"
#include "DataMap.h"
#include <sstream>
#include <vector>

using namespace Acore::ChatCommands;

/*
Coded by Talamortis - For Azerothcore
Thanks to Rochet for the assistance
*/

struct IndividualXpModule
{
    bool Enabled, AnnounceModule, AnnounceRatesOnLogin;
    float MaxRate, DefaultRate;
    // Tofu Worldview: challenge settings (mod-challenge-modes) that set and lock the personal rate. One Rate wins over Quest Only, whatever order they were taken in.
    std::string LockSettingSource;
    int32 OneRateIndex, QuestOnlyIndex;
    float OneRateXP, QuestOnlyXP;
};

IndividualXpModule individualXp;

enum IndividualXPAcoreString
{
    ACORE_STRING_CREDIT = 100000,
    ACORE_STRING_MODULE_DISABLED,
    ACORE_STRING_RATES_DISABLED,
    ACORE_STRING_COMMAND_VIEW,
    ACORE_STRING_MAX_RATE,
    ACORE_STRING_MIN_RATE,
    ACORE_STRING_COMMAND_SET,
    ACORE_STRING_COMMAND_DISABLED,
    ACORE_STRING_COMMAND_ENABLED,
    ACORE_STRING_COMMAND_DEFAULT
};

class IndividualXPConf : public WorldScript
{
public:
    IndividualXPConf() : WorldScript("IndividualXPConf") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        individualXp.Enabled = sConfigMgr->GetOption<bool>("IndividualXp.Enabled", true);
        individualXp.AnnounceModule = sConfigMgr->GetOption<bool>("IndividualXp.Announce", true);
        individualXp.AnnounceRatesOnLogin = sConfigMgr->GetOption<bool>("IndividualXp.AnnounceRatesOnLogin", true);
        individualXp.MaxRate = sConfigMgr->GetOption<float>("IndividualXp.MaxXPRate", 10.0f);
        individualXp.DefaultRate = sConfigMgr->GetOption<float>("IndividualXp.DefaultXPRate", 1.0f);
        individualXp.LockSettingSource = sConfigMgr->GetOption<std::string>("IndividualXp.Tofu.LockSettingSource", "mod-challenge-modes");
        individualXp.OneRateIndex = sConfigMgr->GetOption<int32>("IndividualXp.Tofu.OneRateIndex", 9);
        individualXp.QuestOnlyIndex = sConfigMgr->GetOption<int32>("IndividualXp.Tofu.QuestOnlyIndex", 6);
        individualXp.OneRateXP = sConfigMgr->GetOption<float>("IndividualXp.Tofu.OneRateXP", 1.0f);
        individualXp.QuestOnlyXP = sConfigMgr->GetOption<float>("IndividualXp.Tofu.QuestOnlyXP", 2.0f);
    }
};

static bool ChallengeActive(Player* player, int32 index)
{
    return index >= 0 && player->GetPlayerSetting(individualXp.LockSettingSource, uint32(index)).value != 0;
}

// Tofu Worldview: the rate a challenge forces on this character, or 0 when no challenge locks it. One Rate (x1) wins over Quest Only (x2), so the order the two were taken in does not matter.
static float ForcedRate(Player* player)
{
    if (!player || individualXp.LockSettingSource.empty())
        return 0.0f;
    if (ChallengeActive(player, individualXp.OneRateIndex))
        return individualXp.OneRateXP;
    if (ChallengeActive(player, individualXp.QuestOnlyIndex))
        return individualXp.QuestOnlyXP;
    return 0.0f;
}

// "locked at x1 by your One Rate challenge", "locked at x1 by your One Rate and Quest Only challenges", or "locked at x2 by your Quest Only challenge"
static std::string LockReason(Player* player)
{
    bool const oneRate = ChallengeActive(player, individualXp.OneRateIndex);
    bool const questOnly = ChallengeActive(player, individualXp.QuestOnlyIndex);
    std::ostringstream text;
    text << "locked at x" << ForcedRate(player) << " by your ";
    if (oneRate && questOnly)
        text << "One Rate and Quest Only challenges";
    else if (oneRate)
        text << "One Rate challenge";
    else
        text << "Quest Only challenge";
    text << " until level 80.";
    return text.str();
}

static std::string ForcedRateText(Player* player)
{
    return "[XP] Your XP rate is " + LockReason(player);
}

static bool RefuseIfLocked(ChatHandler* handler, Player* player)
{
    float const forced = ForcedRate(player);
    if (forced <= 0.0f)
        return false;
    handler->SendSysMessage("[XP] You can't change your XP rate: it is " + LockReason(player));
    handler->SetSentErrorMessage(true);
    return true;
}

class PlayerXpRate : public DataMap::Base
{
public:
    PlayerXpRate() {}
    PlayerXpRate(float XPRate) : XPRate(XPRate) {}
    float XPRate = 1.0f;
};

// Tofu Worldview: overwrite the stored rate with the challenge's rate (the same as the player running .xp set). Returns true when it changed. After level 80 the challenge ends and the player can change the rate again.
static bool ApplyForcedRate(Player* player)
{
    float const forced = ForcedRate(player);
    if (forced <= 0.0f)
        return false;
    PlayerXpRate* data = player->CustomData.GetDefault<PlayerXpRate>("IndividualXP");
    if (data->XPRate == forced)
        return false;
    data->XPRate = forced;
    CharacterDatabase.Execute("REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}');", player->GetGUID().GetCounter(), forced);
    return true;
}

class IndividualXP : public PlayerScript
{
public:
    IndividualXP() : PlayerScript("IndividualXP") {}

    void OnPlayerLogin(Player* player) override
    {
        QueryResult result = CharacterDatabase.Query("SELECT `XPRate` FROM `individualxp` WHERE `CharacterGUID`='{}'", player->GetGUID().GetCounter());

        if (!result)
        {
            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = individualXp.DefaultRate;
        }
        else
        {
            Field* fields = result->Fetch();
            player->CustomData.Set("IndividualXP", new PlayerXpRate(fields[0].Get<float>()));
        }

        ApplyForcedRate(player);

        if (individualXp.Enabled)
        {
            // Announce Module
            if (individualXp.AnnounceModule)
            {
                ChatHandler(player->GetSession()).SendSysMessage(ACORE_STRING_CREDIT);
            }

            // Announce Rates
            if (individualXp.AnnounceRatesOnLogin)
            {
                if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
                }
                else if (ForcedRate(player) > 0.0f)
                {
                    ChatHandler(player->GetSession()).SendSysMessage(ForcedRateText(player));
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_VIEW, player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate);
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
                }
            }
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (PlayerXpRate* data = player->CustomData.Get<PlayerXpRate>("IndividualXP"))
        {
            CharacterDatabase.DirectExecute("REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}');", player->GetGUID().GetCounter(), data->XPRate);
        }
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (individualXp.Enabled)
        {
            // A challenge taken since login (at the shrine) sets the rate before this XP is counted
            if (ApplyForcedRate(player))
                ChatHandler(player->GetSession()).SendSysMessage(ForcedRateText(player));

            if (PlayerXpRate* data = player->CustomData.Get<PlayerXpRate>("IndividualXP"))
            {
                amount = static_cast<uint32>(std::round(static_cast<float>(amount) * data->XPRate));
            }
        }
    }
};

class IndividualXPCommand : public CommandScript
{
public:
    IndividualXPCommand() : CommandScript("IndividualXPCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable IndividualXPCommandTable =
        {
            { "enable",  HandleEnableCommand, SEC_PLAYER, Console::No },
            { "disable",  HandleDisableCommand, SEC_PLAYER, Console::No },
            { "view",  HandleViewCommand, SEC_PLAYER, Console::No },
            { "set",  HandleSetCommand, SEC_PLAYER, Console::No },
            { "default",  HandleDefaultCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable IndividualXPBaseTable =
        {
            { "xp",  IndividualXPCommandTable }
        };

        return IndividualXPBaseTable;
    }

    static bool HandleViewCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else if (ForcedRate(player) > 0.0f)
        {
            ApplyForcedRate(player);
            handler->SendSysMessage(ForcedRateText(player));
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_VIEW, player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate);
        }
        return true;
    }

    static bool HandleSetCommand(ChatHandler* handler, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (RefuseIfLocked(handler, player))
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            if (rate > individualXp.MaxRate)
            {
                handler->PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (rate < 0.1f)
            {
                handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
                handler->SetSentErrorMessage(true);
                return false;
            }

            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = rate;
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_SET, rate);
            return true;
        }
    }

    static bool HandleDisableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (RefuseIfLocked(handler, player))
            return false;

        if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            // Turn Disabled On But Don't Change Value...
            player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DISABLED);
            return true;
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            return false;
        }
    }

    static bool HandleEnableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_ENABLED);
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
        }

        return true;
    }

    static bool HandleDefaultCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (RefuseIfLocked(handler, player))
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = individualXp.DefaultRate;
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DEFAULT, individualXp.DefaultRate);
            return true;
        }
    }
};

void AddIndividualXPScripts()
{
    new IndividualXPConf();
    new IndividualXP();
    new IndividualXPCommand();
}
