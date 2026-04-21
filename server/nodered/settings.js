module.exports = {
    flowFile: 'flows.json',
    flowFilePretty: true,
    autoInstallModules: true,

    uiPort: process.env.PORT || 1880,

    diagnostics: {
        enabled: true,
        ui: true,
    },

    runtimeState: {
        enabled: false,
        ui: false,
    },

    logging: {
        console: {
            level: "info",
            metrics: false,
            audit: false
        }
    },

    exportGlobalContextKeys: false,

    externalModules: {},

    editorTheme: {
        projects: {
            enabled: false,
            workflow: {
                mode: "manual"
            }
        },
        codeEditor: {
            lib: "monaco",
        },
        markdownEditor: {
            mermaid: {
                enabled: true
            }
        },
        multiplayer: {
            enabled: false
        },
    },

    functionExternalModules: true,
    globalFunctionTimeout: 0,
    functionTimeout: 0,
    functionGlobalContext: {},
    debugMaxLength: 1000,
    mqttReconnectTime: 15000,
    serialReconnectTime: 15000,
}