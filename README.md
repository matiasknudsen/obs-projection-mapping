# OBS Projection Mapping

Free and simple projection mapping directly inside OBS Studio (macOS first, Windows later).

## How to use

1. Create a scene with the content you want to project (videos, images, camera …), e.g. "Content".
2. Create a new scene, e.g. "Projector", and add the **Projection Mapping** source.
3. In its properties: pick "Content" and set your projector's resolution. Click OK.
4. Right-click the source → **Fullscreen Projector (Source)** → choose your projector.
5. Right-click the source → **Interact** and drag the corners until they match the surfaces in the room.
   - Drag inside a surface to move the whole surface.
   - Mode "Select part of the content" lets you choose which part of the content each surface shows.
   - "+ Add surface" creates a new surface.
6. Set the mode to "Done" before the show to hide the guides.

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
