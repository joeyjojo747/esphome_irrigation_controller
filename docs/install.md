# Install

The detailed companion to the short install checklist. Do the steps in order. Nothing here needs a
reflash after the first USB flash: programs, zones, outputs and every setting are edited in the
dashboard.

## 0. Rotate secrets

If any API key or OTA password for this device was ever shared (pasted in a chat, an issue, a
screenshot or a commit), generate new ones in Device Builder's Secrets before the first flash. Never
reuse values from an old config. A fresh API key is `openssl rand -base64 32`. Real secrets live only
in Device Builder's `secrets.yaml`; the repo only has `esphome/secrets.yaml.example` with dummy
values.

## 1. Device Builder

1. Open the ESPHome Device Builder add-on and create a new device, then replace its YAML with the
   wrapper `esphome/irrigation-controller.yaml`. Copy the whole `esphome/irrigation-controller/`
   folder next to it (it holds the packages and the `irrigation_core` component).
2. Click **Secrets** and add `wifi_ssid`, `wifi_password`, `ap_password`, `ota_password` and
   `api_key`. Use the names in `esphome/secrets.yaml.example` and your own values.
3. Edit the wrapper header: set `timezone` (an IANA name), check the pin substitutions for your
   board and, if you fit an MCP23017, DS3231 or status LED, enable the matching `packages:` lines.
   Do not change `name` or `friendly_name`: every entity id (`irrigation_*`) and action name
   (`esphome.irrigation_controller_*`) depends on them.
4. First flash over USB: click **Install**, choose **Plug into this computer** and follow the
   browser prompts. Later updates go over Wi-Fi.
5. In Home Assistant open **Settings, Devices & services**, find the ESPHome device and, when it is
   discovered, add it with the API key. Then open the device's configure dialog and enable
   **Allow the device to perform Home Assistant actions**. Without this the device cannot send its
   events (`esphome.irrigation_event`) to Home Assistant.

## 2. Packages

1. Copy `packages/irrigation/` to `/config/packages/irrigation/`.
2. In `configuration.yaml` add:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages/irrigation
   ```

   If you already have a `homeassistant:` block, add only the `packages:` line to it.
3. Run **Developer tools, Check configuration**, then restart Home Assistant once. This is the only
   restart the install needs; settings are helpers edited in the dashboard.

On the first start the package fills every helper with its default (`script.irrigation_apply_defaults`).
You can run that script later to reset all settings.

## 3. Calendar

Create a Local Calendar: **Settings, Devices & services, Add integration, Local Calendar**, with the
name `Irrigation`, so the entity is `calendar.irrigation`. The package writes finished and skipped
runs to it. Future runs are shown from `sensor.irrigation_upcoming`.

## 4. HACS

Install these from HACS, then reload the browser:

- **BOM** (Bureau of Meteorology) integration, for the weather entity used by the daily adjust.
  Set up its location, then note the weather entity id (for example `weather.home`).
- **Ultra Card** (frontend).
- **ha-floorplan** (frontend), for the site plan.

## 5. AI (optional)

Weather rules work with AI off. To add AI advice, in the dashboard Settings view set **AI mode**:

- **AI Task**: use any integration that provides an AI Task entity and is already installed, for
  example OpenAI, Google Gemini, Anthropic or Ollama. Set **AI entity** to its `ai_task.*` id. To
  find it: **Settings, Devices & services, Entities**, search `ai_task.`, or **Developer tools,
  States**, filter `ai_task`.
- **Conversation agent**: use any conversation agent. Set **AI entity** to the agent id, for example
  `conversation.openai_conversation`. Find it under **Developer tools, States**, filter
  `conversation.`.
- **Off**: no AI; rule-based weather adjustment still runs.

## 6. Dashboard

1. Open **Settings, Dashboards, Add dashboard**, create one titled `Irrigation`, open it, then use
   the three-dot menu, **Edit dashboard**, three-dot menu again, **Raw configuration editor**.
2. Paste the contents of `dashboards/irrigation-dashboard.yaml` and save.
3. The overview summary, the programs editor and the zones editor are native Home Assistant cards
   marked `# ULTRA CARD:` in the file. Rebuild each in the Ultra Card visual editor and paste its
   export over the marked card if you want the Ultra Card look. The native version works as is.
4. Open the **Settings** view and fill in the weather entity, notify targets and any thresholds.
5. Press **Backup config** (Settings view, bottom). This stores the controller's programs, zones and
   settings in `sensor.irrigation_config_backup`, so **Restore config** can rebuild a replaced device.

Then work through `docs/acceptance.md` on the installed system.

## Site plan

The site plan is an SVG (`www/irrigation/siteplan.svg`) shown by the HACS card
[ha-floorplan](https://github.com/ExperienceLovelace/ha-floorplan). The template has one controller
(`controller-1`), zones `zone-01` to `zone-06` and two sample heads per zone. Placing devices on the
plan means editing the SVG; nothing on the ESP changes.

1. Take a Google Maps satellite screenshot of the property and save it as `satellite.jpg`
   (about 1600x1000 px, in the same folder as the SVG). It is not committed to the repo. The SVG
   references it as `/local/irrigation/satellite.jpg` (an absolute path, because ha-floorplan inlines
   the SVG), so Inkscape will not show it unless you also copy it to `/config/www/irrigation/`.
2. Open `siteplan.svg` in Inkscape. The `layer-background` layer already points at `satellite.jpg`
   (see above); alternatively import the screenshot into that layer.
3. On layer `layer-zones`, draw one polygon per irrigated area. Open the XML editor (Ctrl+Shift+X)
   and set `id` to `zone-NN` (two digits, matching the zone number), `class` to `zone-idle` and add
   the attribute `data-zone` with the zone number.
4. On layer `layer-heads`, duplicate a head from the template (Ctrl+D), move it into place, and set
   `id` to `head-NN-MM` (zone, head number) and `class` to one of `head-spray`, `head-rotor`,
   `head-rotator`, `head-drip`, `head-bubbler`. The legend explains the symbols.
5. For zones beyond 06, copy a rule block in `floorplan/irrigation-floorplan.yaml` and change the
   zone number in the element id, entity id and `tap_action`.
6. Copy `www/irrigation/` (SVG, CSS and `satellite.jpg`) to `/config/www/irrigation/` on Home
   Assistant. The dashboard's Site plan view already contains the card from
   `floorplan/irrigation-floorplan.yaml`; if you change that file, paste the new content over the card.

Zone colours: blue pulse = running, faint blue = idle, grey hatch = disabled/unavailable,
red = running with the flow alarm active. Tapping a zone asks for confirmation, then runs it for 5 minutes.

Tapping a zone calls `script.irrigation_siteplan_run_zone` (package `irrigation_siteplan.yaml`), which
forwards only `zone` and `minutes` to the device, because ha-floorplan adds `entity_id` to service data.

Note: the card wrapper `type: custom:floorplan-card` and the tap confirmation are unverified until
Task 19 tests them on a real Home Assistant.
