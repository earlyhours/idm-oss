# Native interface design

The actual Qt application now uses the requested a.yash0.in visual direction: graphite surfaces, warm text, an orange primary action and restrained dividers. The app retains IDM's menus, categories, sortable download table, confirmation flow, queues and transfer controls.

The previous Win32-style rendering had multicolored icons, 11px rows and equally prominent toolbar actions. The redesign replaces those with one consistent SVG icon set, 42px rows, a clear Add URL action, aligned numeric columns, visible progress within the Status column, category counts and a useful empty state. The sidebar and table remain the main navigation and work area.

Dialogs share the same palette, fields, focus treatment, checkboxes, tab indicators and button hierarchy. The transfer dialog groups speed, remaining time and received bytes above its progress and connection details. Dark mode is the default; the warm light mode follows the same layout. Existing explicitly saved light preferences are respected by the application. The user's installed app was switched to the requested dark presentation as part of the redesign.

The visual direction comes from `/home/yvm/.codex/skills/a-yash0-site/SKILL.md` and its local template. The native implementation applies the relevant guidance from `redesign-existing-projects`. No report or website is required for the app design. No paid tool, new framework, font download or network asset is used. Adwaita Sans and Mono use the desktop's installed fonts with Qt fallback. Control corners follow Omarchy and keyboard focus follows its live accent; the orange brand action remains stable.

The screenshots in `assets/` are actual Qt renders of isolated fixture downloads and dialogs. Progress and transfer rates come from a paced local HTTP server. No synthetic counters, personal history or credentials appear in those images.

Validation includes the real-QML workflow suite, dark/light text contrast, actual pointer activation of Add URL, live focus-color changes, empty-to-populated state, a queue of 80 jobs, long-name hover text, horizontal scrolling and minimum-window dialog bounds. The automated checks supplement visual inspection of the actual dark, light, transfer and settings screens. Full Windows pixel equivalence, every desktop scale and live Firefox remain outside the evidence.
