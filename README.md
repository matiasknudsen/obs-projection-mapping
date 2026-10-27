# OBS Projection Mapping

Free and simple projection mapping directly inside OBS Studio (macOS first, Windows later).

## How to use

1. Create a scene with the content you want to project (videos, images, camera …), e.g. "Content".
2. Create a new scene, e.g. "Projector", and add the **Projection Mapping** source.
3. In its properties: pick "Content" and set your projector's resolution. Click OK.
4. Right-click the source → **Open Source Projector** → choose your projector.
5. Edit directly in the OBS preview, or in the **Projection Mapping** dock (Docks menu). Press **Tab** to switch mode:
   - **Move** (yellow): drag a **corner** for perspective, an **edge** handle to move one side, or **inside** a surface to move all of it.
   - **Mask** (red): double-click the red outline to add a point, double-click a point to remove it.
     Hold **Alt/⌥** and draw to add to the mask, **Alt/⌥ + Shift** and draw to erase.
   - **Done**: all guides hidden.
   - **Cmd/Ctrl+Z** undoes, **Cmd/Ctrl+Shift+Z** redoes.
   - The dock also has **+ Square / + Round**, a **corner rounding** slider, **Clear mask** and **Crop** mode
     (choose which part of the content each surface shows).
   - Editing directly in the preview requires the preview scaling to be **Scale to window**.
6. Switch to **Done** before the show. Untick **Show guides on projector** if the audience should never see them.

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
