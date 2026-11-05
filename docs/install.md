# Install

## Site plan

The site plan is an SVG (`www/irrigation/siteplan.svg`) shown by the HACS card
[ha-floorplan](https://github.com/ExperienceLovelace/ha-floorplan). The template has one controller
(`controller-1`), zones `zone-01` to `zone-06` and two sample heads per zone. Placing devices on the
plan means editing the SVG; nothing on the ESP changes.

1. Take a Google Maps satellite screenshot of the property and save it as `satellite.jpg`
   (about 1600x1000 px, next to the SVG). It is not committed to the repo.
2. Open `siteplan.svg` in Inkscape. The `layer-background` layer already points at `satellite.jpg`;
   keep the file beside the SVG, or import the screenshot into that layer.
3. On layer `layer-zones`, draw one polygon per irrigated area. Open the XML editor (Ctrl+Shift+X)
   and set `id` to `zone-NN` (two digits, matching the zone number), `class` to `zone-idle` and add
   the attribute `data-zone` with the zone number.
4. On layer `layer-heads`, duplicate a head from the template (Ctrl+D), move it into place, and set
   `id` to `head-NN-MM` (zone, head number) and `class` to one of `head-spray`, `head-rotor`,
   `head-rotator`, `head-drip`, `head-bubbler`. The legend explains the symbols.
5. For zones beyond 06, copy a rule block in `floorplan/irrigation-floorplan.yaml` and change the
   zone number in the element id, entity id and `tap_action`.
6. Copy `www/irrigation/` (SVG, CSS and `satellite.jpg`) to `/config/www/irrigation/` on Home
   Assistant and add `floorplan/irrigation-floorplan.yaml` as a card on the Site plan view.

Zone colours: blue pulse = running, faint blue = idle, grey hatch = disabled/unavailable,
red = running with the flow alarm active. Tapping a zone asks for confirmation, then runs it for 5 minutes.
