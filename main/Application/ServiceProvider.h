#pragma once

class CommandManager;
class ConsoleManager;
class DeviceManager;
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
    virtual CommandManager& getCommandManager() = 0;
    virtual ConsoleManager& getConsoleManager() = 0;
    virtual DeviceManager& getDeviceManager() = 0;
    virtual HomeAssistantManager& getHomeAssistantManager() = 0;
    virtual MqttManager& getMqttManager() = 0;
    virtual NetworkManager& getNetworkManager() = 0;
    virtual SettingsManager& getSettingsManager() = 0;
    virtual SystemManager& getSystemManager() = 0;
    virtual TimeManager& getTimeManager() = 0;
    virtual UpdateManager& getUpdateManager() = 0;
    virtual WebServerManager& getWebServerManager() = 0;
};
