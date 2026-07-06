#pragma once
#include "ServiceProvider.h"
#include "Board.h"
#include "CommandManager/CommandManager.h"
#include "ConsoleManager/ConsoleManager.h"
#include "NetworkManager/NetworkManager.h"
#include "SettingsManager/SettingsManager.h"
#include "SystemManager/SystemManager.h"
#include "TimeManager/TimeManager.h"
#include "UpdateManager/UpdateManager.h"
#include "WebServerManager/WebServerManager.h"

class ApplicationContext : public ServiceProvider
{
public:
    ApplicationContext() = default;
    ~ApplicationContext() = default;
    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;

    Board& getBoard() override { return m_board; }
    CommandManager& getCommandManager() override { return m_commandManager; }
    ConsoleManager& getConsoleManager() override { return m_consoleManager; }
    NetworkManager& getNetworkManager() override { return m_networkManager; }
    SettingsManager& getSettingsManager() override { return m_settingsManager; }
    SystemManager& getSystemManager() override { return m_systemManager; }
    TimeManager& getTimeManager() override { return m_timeManager; }
    UpdateManager& getUpdateManager() override { return m_updateManager; }
    WebServerManager& getWebServerManager() override { return m_webServerManager; }

private:
    ConsoleManager m_consoleManager{*this};
    SettingsManager m_settingsManager{*this};
    SystemManager m_systemManager{*this};
    NetworkManager m_networkManager{*this};
    TimeManager m_timeManager{*this};
    CommandManager m_commandManager{*this};
    Board m_board{*this};
    UpdateManager m_updateManager{*this};
    WebServerManager m_webServerManager{*this};
};
