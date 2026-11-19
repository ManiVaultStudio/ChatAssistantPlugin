#include "ChatAssistantPlugin.h"
#include "ChatPanel.h"

#include <QVBoxLayout>

ChatAssistantPlugin::ChatAssistantPlugin(const mv::plugin::PluginFactory *factory) : ViewPlugin(factory) {}

void ChatAssistantPlugin::init() {
    auto *layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    chat_ = new ChatPanel(&getWidget());
    layout->addWidget(chat_);
    getWidget().setLayout(layout);
}

ChatAssistantPluginFactory::ChatAssistantPluginFactory() : ViewPluginFactory(false) {
    setGuiName("Knowledge Assistant");
    setIconByName("comments");
    auto &metadata = getPluginMetadata();
    metadata.setDescription("Knowledge Assistant — completely offline AI chat");
    metadata.setSummary(
        "Ask natural-language questions about software, indexed documentation and code. "
        "Answers are generated on-device using a local GGUF model and SQLite knowledge index.");
    metadata.setLicenseText("Plugin source code is distributed under the MIT License. "
                            "Model weights and indexed source content retain their own licenses.");
}

mv::plugin::ViewPlugin *ChatAssistantPluginFactory::produce() {
    return new ChatAssistantPlugin(this);
}

mv::DataTypes ChatAssistantPluginFactory::supportedDataTypes() const {
    return {}; // This view is available without a selected dataset.
}

mv::gui::PluginTriggerActions
ChatAssistantPluginFactory::getPluginTriggerActions(const mv::Datasets &datasets) const {
    Q_UNUSED(datasets);
    return {}; // Created from the application's View menu, independent of datasets.
}
