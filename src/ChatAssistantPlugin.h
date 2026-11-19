#pragma once

#include <QWidget>
#include <ViewPlugin.h>

class ChatPanel;

/** A dataset-independent local AI chat view for ManiVault Studio. */
class ChatAssistantPlugin final : public mv::plugin::ViewPlugin {
    Q_OBJECT
  public:
    explicit ChatAssistantPlugin(const mv::plugin::PluginFactory *factory);
    ~ChatAssistantPlugin() override = default;
    void init() override;

  private:
    ChatPanel *chat_ = nullptr;
};

/** Exported, application-owned plugin factory. */
class ChatAssistantPluginFactory final : public mv::plugin::ViewPluginFactory {
    Q_OBJECT
    Q_INTERFACES(mv::plugin::ViewPluginFactory mv::plugin::PluginFactory)
    Q_PLUGIN_METADATA(IID "studio.manivault.ChatAssistantPlugin" FILE "../PluginInfo.json")
  public:
    ChatAssistantPluginFactory();
    mv::plugin::ViewPlugin *produce() override;
    mv::DataTypes supportedDataTypes() const override;
    mv::gui::PluginTriggerActions getPluginTriggerActions(const mv::Datasets &datasets) const override;
};
