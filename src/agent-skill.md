---
name: idm-native
description: Download HTTP(S) files with the native Omarchy download manager.
---
Use `idm-native download URL --directory /absolute/destination --connections 4 --json`.
The command exits 0 only when the file has completed; errors exit 1. Destination files are never overwritten. Use `idm-native open` for pause/resume and the saved queue. Closing the app preserves partial downloads. Do not start concurrent instances against the same queue.
