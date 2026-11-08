# OBS Projection Mapping

Free and simple projection mapping directly inside OBS Studio (macOS first, Windows later).

## How to use

1. Create a scene with the content you want to project (videos, images, camera …), e.g. "Content".
2. Create a new scene, e.g. "Projector", and add the **Projection Mapping** source.
3. In its properties: pick "Content" and set your projector's resolution. Click OK.
4. Right-click the source → **Open Source Projector** → choose your projector.
5. Edit directly in the OBS preview (any zoom level) or in the **Projection Mapping** dock (Docks menu). Press **Tab** to switch mode:
   - **Move** (yellow): drag a **corner** for perspective, an **edge** handle to move one side, or **inside** a surface to move it.
     **Shift + corner** scales proportionally with the opposite corner fixed. **Right-click** a surface to flip or rotate its content.
   - **Mask** (red): double-click the red outline to add a point, double-click a point to remove it.
     Double-click **inside** a surface to cut a **hole**: click points, then click the first point (or press Enter). Esc cancels.
     Hold **Alt/⌥** and draw to add to the mask, **Alt/⌥ + Shift** and draw to erase.
   - **Done**: all guides hidden.
   - **Snap** (dock) makes corners and points stick to other surfaces and the frame edge.
   - **Cmd/Ctrl+Z** undoes, **Cmd/Ctrl+Shift+Z** redoes.
   - While editing, the Projection Mapping item is locked in OBS so the whole source can't be moved by accident.
6. Switch to **Done** before the show. Untick **Show guides on projector** if the audience should never see them.

**Content:** pick any source or scene, or **★ Active scene (program)** to follow whatever scene is live in OBS.
The Projection Mapping source must not be inside the scene it shows – keep it in its own scene and show it with
**Open Source Projector**.

The mapping is saved in your OBS scene collection. Changing what plays in the "Content" scene keeps the mapping intact.

## Installation (macOS)

1. Open the **Actions** tab of this repository and click the latest run with a green check mark.
2. Under **Artifacts**, download the file ending in **macos-universal**.
3. Unzip it and double-click the **.pkg** file.
4. If macOS blocks it: **System Settings → Privacy & Security → Open Anyway**.
5. Restart OBS.

## Building

GitHub Actions builds a macOS installer (.pkg) on every push to `main`.

Based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate). License: GPL-2.0.
