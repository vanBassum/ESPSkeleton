#pragma once

class Board;
class CommandManager;
class ConsoleManager;
class NetworkManager;
class RelayManager;
class SettingsManager;
class SystemManager;
class TimeManager;
class UpdateManager;
class WebServerManager;

class ServiceProvider
{
public:
    virtual Board& getBoard() = 0;
    virtual CommandManager& getCommandManager() = 0;
    virtual ConsoleManager& getConsoleManager() = 0;
    virtual NetworkManager& getNetworkManager() = 0;
    virtual RelayManager& getRelayManager() = 0;
    virtual SettingsManager& getSettingsManager() = 0;
    virtual SystemManager& getSystemManager() = 0;
    virtual TimeManager& getTimeManager() = 0;
    virtual UpdateManager& getUpdateManager() = 0;
    virtual WebServerManager& getWebServerManager() = 0;
};
