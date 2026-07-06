#pragma once

class Board;
class CommandManager;
class ConsoleManager;
class HomeAssistantManager;
class MqttManager;
class NetworkManager;
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
    virtual HomeAssistantManager& getHomeAssistantManager() = 0;
    virtual MqttManager& getMqttManager() = 0;
    virtual NetworkManager& getNetworkManager() = 0;
    virtual SettingsManager& getSettingsManager() = 0;
    virtual SystemManager& getSystemManager() = 0;
    virtual TimeManager& getTimeManager() = 0;
    virtual UpdateManager& getUpdateManager() = 0;
    virtual WebServerManager& getWebServerManager() = 0;
};
