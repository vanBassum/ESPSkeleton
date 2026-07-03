#pragma once
#include "ServiceProvider.h"
#include "CommandManager/CommandManager.h"
#include "ConsoleManager/ConsoleManager.h"
#include "DeviceManager/DeviceManager.h"
#include "HomeAssistantManager/HomeAssistantManager.h"
#include "MqttManager/MqttManager.h"
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

    CommandManager& getCommandManager() override { return m_commandManager; }
    ConsoleManager& getConsoleManager() override { return m_consoleManager; }
    DeviceManager& getDeviceManager() override { return m_deviceManager; }
    HomeAssistantManager& getHomeAssistantManager() override { return m_homeAssistantManager; }
    MqttManager& getMqttManager() override { return m_mqttManager; }
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
    MqttManager m_mqttManager{*this};
    DeviceManager m_deviceManager{*this};
    HomeAssistantManager m_homeAssistantManager{*this};
    UpdateManager m_updateManager{*this};
    WebServerManager m_webServerManager{*this};
};
