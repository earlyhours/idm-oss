# Native interface review

The app uses the classic IDM layout: menus, labeled toolbar buttons, category tree, detailed download table and transfer dialog. Add URL opens file information before Start Download or Download Later. Options and Scheduler contain the working native settings.

The reference is the recovered historical help, labelled IDM 5.11. It establishes the layout and workflow, not pixel equivalence with IDM 6.43. The implementation uses independent Qt controls and icons.

`assets/screenshot.png` is an actual Qt render with isolated fixture downloads. Its progress and transfer rate came from a paced local HTTP server. It contains no personal history or credentials.

The real-QML suite checks the download dialogs, settings, shortcuts, language changes, dark preference and native transfer flow. Browser and engine behavior have separate end-to-end checks. Full Windows pixel equivalence, every desktop scale and live Firefox remain outside the evidence.

The minimum-window review used the real QML at 820×500 with 80 paused downloads, long filenames and descriptions. Keyboard selection kept the last row visible, and horizontal scrolling reached the description column. Elided cells now reveal their full text on hover. The Sites hostname stays above the scrolling settings, and the transfer connection list shrinks to keep Resume/Pause, Cancel and Close inside the window. Options also fits the available content height. The follow-up `bin/test-ui` run passed seven cases with no QML warnings.

Before/after native captures are retained locally as `/tmp/idm-native-minimum-sites-before.png` and `/tmp/idm-native-minimum-sites.png`, plus `/tmp/idm-native-long-name-before.png` and `/tmp/idm-native-long-name.png`. The full-name hover capture is `/tmp/idm-native-long-name-tooltip.png`. These are isolated fixtures; no ordinary downloads or desktop focus were changed.
