# ApplepieManager

<p align="center">
  <img src="assets/icons/applepie-manager.png" alt="ApplepieManager" width="160">
</p>

English | [中文](README.md)

A DLL plugin manager for *Arknights: Endfield*. Manager includes a Core for plugin loading, exclusive and typed shared hooks, managed pointer slots, window routing, asynchronous logical enable/disable, and independent animation/camera write ownership. It also provides UID hiding. See the [SDK](sdk/README.md) for plugin integration.

## Key Features

- **Plugin Management**: Scans `plugin\` after CoreReady, checks manifests, ABI, duplicate IDs and required capabilities, and distinguishes loaded/attached/active states
- **Dynamic Hooks**: Exclusive and typed shared targets, two-stage activation, address deduplication, exact native signatures and SEH entry contracts; SDK in `sdk/`
- **Shared Services**: Conditional PointerPatch restoration, one game-window chain and minimal runtime/target-generation tickets
- **Lifecycle and Ownership**: Runtime buttons asynchronously load, enable and disable adapted clients; Disabled requires confirmed restoration, Enable grants no animation/camera ownership, and DLLs remain resident
- **Unified Enable Switch**: Requests the session state and automatically saves the next-launch preference; rejected requests preserve the previous preference
- **Basic Settings**: Plugins can optionally register boolean, integer, floating-point, enum, short-text and key fields. Keys show their names and support press-to-bind input. Manager provides editing, validation, automatic saving after changes and completion notifications. Fields are allocated dynamically; complex configuration remains plugin-owned
- **UID Hiding**: Hide in-game UID, ping display, menu UID/name, and business card UID/name
- **Multilingual UI**: Chinese/English toggle for the Manager panel

## User Agreement & Disclaimer

<details>
<summary>Please read this agreement carefully before downloading, installing, or using this plugin. <b>By using this plugin, you acknowledge that you have fully read, understood, and agreed to all of the following terms.</b></summary>

### 1. Open Source License & End User Rights
- This plugin is fully open-sourced on GitHub under the **AGPL-3.0** license. Users may freely use, modify, and distribute the source code of this plugin in compliance with the license.
- End Users may use and distribute this plugin **without any restrictions**, provided they do not modify it. This right is not affected by whether the user violates this agreement.

### 2. Anti-Fraud Statement
- You **must not** openly sell this plugin **itself** on online retail platforms without providing the GitHub repository address and after-sales service.
- This plugin is entirely free and open-source on GitHub. If you obtained it through a paid purchase, please be aware that it is freely available on GitHub.

### 3. Content Compliance & Conduct
- This plugin does not contain any game art assets. Users acknowledge and agree that the official animations, scenes, models, and other assets built into *Arknights: Endfield* are copyrighted by Hypergryph and are not covered by the AGPL-3.0 license. You **should not** use this plugin or the game's official animations, scenes, models, or other assets to create, play, or distribute inappropriate motions or animations, including sexual, violent, politically sensitive, or other content that violates laws or regulations or causes discomfort in the community. Users are responsible for ensuring that they hold the rights to any imported resources and comply with their licenses and terms of use.

### 4. Risk & Disclaimer
- This project is for educational, technical research, and communication purposes only. The author is not responsible for how users use this plugin, including any misuse. All Arknights game data assets used in this plugin are copyrighted by Hypergryph. Using this tool may violate the game's terms of service and carries a risk of account suspension. For any loss directly or indirectly caused by using this plugin (including but not limited to account bans, game data corruption, etc.), **this project assumes no legal or financial liability**. Users bear all risks and are strongly advised to use it on a test account.

</details>

## Installation

Download the latest release from [Releases](https://github.com/zonemodAE/ApplepieManager/releases), or build it from source.
Each release provides an Applepie Manager desktop installer (`*-setup.exe`), a portable executable (`ApplepieManager.exe`), and individual DLLs.

Open the Applepie Manager desktop application, select the game folder, and click **Install / update to game**.

You can also manually place the three DLLs at the following locations (the game directory is the folder containing `Endfield.exe`). When building from source, these files are generated in `bin/`:

```text
applepie_manager.dll   → Game Directory/plugin/applepie_manager.dll
vulkan-1.dll           → Game Directory/vulkan-1.dll
d3dcompiler_47.dll     → Game Directory/d3dcompiler_47.dll
```

> `d3dcompiler_47.dll` (DirectX) and `vulkan-1.dll` (Vulkan) only bootstrap Manager. Either or both can be used.

> If you have **never installed a plugin of this type before**, you may need to create the `plugin` folder yourself.

## Usage

1. After installing as described above, click **Connect to Talos-II** to launch the game, or launch it manually.
2. Open the game's settings menu and select the added icon at the end of the tab list. The selected page is titled **Applepie Manager**.
3. Click **Configure** beside a plugin to open its settings, then **Details** to inspect its state and errors. Active is green; inactive states are red. **Enable plugin** controls the current session and automatically saves the next-launch preference.
4. Basic settings save automatically. Press a key to record a binding, or Esc to cancel.
5. Open **Manager settings** for language and UID/ping/menu/business-card visibility options.

Loaded plugin DLLs remain resident. To replace Manager or a loaded plugin DLL, exit the game, replace the file, and restart.

## Plugin Installation

Open the **Plugin downloads** tab to download plugins from the [GitHub plugin catalog](https://github.com/zonemodAE/ApplepieManager/blob/master/catalog/plugins.json). You can also install plugins manually on the plugin management page.

## Configuration and Upgrades

| Settings | Location |
| --- | --- |
| Manager language, UID visibility, and plugin startup preferences | `plugin/config/applepie.manager.ini` under the game directory |
| Basic settings registered through the SDK | `config/applepie.<pluginID>.ini` beside the corresponding plugin DLL |
| Field descriptions for desktop editing | `config/applepie.<pluginID>.schema.json`, written by Core when settings are registered |
| Default Manager configuration template | [templates/config/applepie.manager.ini](templates/config/applepie.manager.ini) in this repository |

Configuration uses flat UTF-8 `key=value` entries and `true/false` switches, without `[general]`, `[uid]`, or `[plugins]` sections. For example:

```ini
language=zh
hide_uid=true
hide_ping=false
# To disable a plugin, uncomment and replace the name with its actual lowercase DLL filename.
# plugin.example.dll=false
```

On a fresh installation, Manager creates its default configuration automatically. To supply initial preferences, copy the template to `plugin/config/applepie.manager.ini`. Unlisted plugins default to enabled; the in-game **Enable plugin** switch automatically saves their startup preferences. Complex configuration remains managed by each plugin.

When upgrading, if the new file is absent, Manager imports language, UID, and plugin enable settings from `plugin/applepie_manager_config.txt`. The old file is left unchanged. Once the new file exists, subsequent reads and writes use only that file.

In-game changes save automatically. External file edits are read on the next launch. A failed save preserves the existing file, and errors are available in the UI and logs.

The desktop editor preserves comments and unknown keys and checks for external edits before saving to avoid overwriting a newer file. Enable a plugin once to generate its field description. When a description is absent or no longer matches the DLL, existing key/value entries remain editable. Complex settings not registered through the SDK are managed through the plugin's own interface.

## Plugin Development

Plugins can use the [C++ client](sdk/applepie_client.h) and include the [optional adapters](sdk/applepie_endfield_adapters.h) when they need common Endfield shared hooks; the underlying [C API](sdk/applepie.h) remains available. See the [SDK guide](sdk/README.md) for integration and a buildable example. Run `sdk\examples\build.bat` to build the minimal plugin, or add `/verify` for standalone offline checks. Neither command launches or deploys to the game.

For new plugins or features, use the [Applepie plugin development skill](skills/applepie-plugin-development/SKILL.md) to select SDK interfaces, verify game interfaces, and plan validation for the required functionality.

For existing plugins, use the [Applepie plugin migration skill](skills/applepie-plugin-migration/SKILL.md) with your coding agent. It selects the required integration paths while preserving custom hooks, pointer-slot wrappers and business restoration.

Install the Visual Studio C++ desktop workload, then run `build.bat` from the project root. The production build generates `bin/applepie_manager.dll`, `bin/d3dcompiler_47.dll` and `bin/vulkan-1.dll`.
