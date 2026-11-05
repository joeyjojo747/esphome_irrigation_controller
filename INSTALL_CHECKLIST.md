# Install checklist

A short, ordered checklist. The long version with explanations is [docs/install.md](https://github.com/joeyjojo747/esphome_irrigation_controller/blob/main/docs/install.md) on GitHub.
Every step ends with a **Check:** line: do not continue until it is true.

**Rotate secrets before the first flash.** If an API key or OTA password for this device was ever
shared (a chat, an issue, a screenshot, a commit), make new ones in Device Builder's Secrets before
you flash. Never reuse values from an old config. The repo only contains dummy values in
`esphome/secrets.yaml.example`.

## Step 1: Download the release

Download `esphome_irrigation_controller-<version>.zip` from the latest GitHub release and unzip it.

**Check:** the unzipped folder contains `esphome/`, `packages/`, `www/`, `dashboards/`, `floorplan/`,
`INSTALL_CHECKLIST.md` and `LICENSE`.

## Step 2: Copy the folders to Home Assistant

Use the File editor or Samba add-on:

- `packages/irrigation/` to `/config/packages/irrigation/`
- `www/irrigation/` to `/config/www/irrigation/`
- For a local firmware install, also `esphome/irrigation-controller/` to `/config/esphome/irrigation-controller/`

The result under `/config`:

```text
/config
  configuration.yaml
  packages/
    irrigation/
      irrigation_ai.yaml
      irrigation_calendar.yaml
      irrigation_helpers.yaml
      irrigation_notify.yaml
      irrigation_siteplan.yaml
      irrigation_sync.yaml
      irrigation_usage.yaml
      irrigation_weather.yaml
      irrigation_zone_advisor.yaml
  www/
    irrigation/
      siteplan.svg
      siteplan.css
  esphome/
    irrigation-controller/        (local firmware install only)
```

**Check:** `/config/packages/irrigation/` holds 9 `.yaml` files and `/config/www/irrigation/` holds
`siteplan.svg` and `siteplan.css`.

## Step 3: Load the packages and restart once

Add this to `configuration.yaml` (if you already have a `homeassistant:` block, add only the
`packages:` line to it):

```yaml
homeassistant:
  packages: !include_dir_named packages/irrigation
```

Run Developer tools, Check configuration, then restart Home Assistant. This is the only restart the
install needs; later changes are made in the dashboard.

**Check:** after the restart, Developer tools, States, filter `irrigation_` shows the helpers
(for example `input_text.irrigation_package_version`).

## Step 4: Create the Irrigation calendar

Settings, Devices & services, Add integration, Local Calendar. Name it `Irrigation`.

**Check:** the entity `calendar.irrigation` exists.

## Step 5: Build and flash the controller

In the ESPHome Device Builder add-on:

1. Create a new device and paste in the contents of `esphome/irrigation-controller.yaml`. Copy
   `esphome/irrigation-controller/` next to it (Step 2). Set `timezone` in the wrapper and do not
   change `name` or `friendly_name`.
2. Open Secrets and fill it in from `esphome/secrets.yaml.example`, using new values you made in the
   rotate-secrets note above.
3. Click Install, then "Plug into this computer" for the first USB flash. After that the controller
   updates wirelessly.
4. In Home Assistant, adopt the discovered device with the API key, open its configure dialog and
   enable "Allow the device to perform Home Assistant actions".

**Check:** the device shows as online in Home Assistant, and `switch.irrigation_zone_1` exists.

## Step 6: Install the HACS frontend and weather pieces

In HACS install the BOM integration, Ultra Card and ha-floorplan, then reload the browser. Set up
BOM for your location.

**Check:** a `weather.*` entity from BOM exists, and Ultra Card and ha-floorplan appear under HACS
installed items.

## Step 7: Optional, choose your AI

In the dashboard's Settings view set `AI mode` to `AI Task` or `Conversation agent` and `AI entity`
to any AI Task entity or conversation agent you already have. Leave it `Off` for rule-based weather
adjustment only.

**Check:** `AI mode` and `AI entity` in Settings show your choice (or `Off`), and the entity you picked
exists under Developer tools, States.

## Step 8: Import the dashboard and back up

Settings, Dashboards, Add dashboard, open it, Edit dashboard, Raw configuration editor, and paste in
`dashboards/irrigation-dashboard.yaml`. In the Settings view set the weather entity, the notify
targets and the relay outputs, then press Backup config.

**Check:** `sensor.irrigation_config_backup` shows a state other than `unknown` or `unavailable`, and
the dashboard opens with all its views (Overview, Site plan, Calendar, Programs, Zones, Water, Settings).
