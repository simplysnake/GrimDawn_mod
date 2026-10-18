# Third-party software

Grim Dawn VR is built with the following libraries, compiled into `gdvr.dll`. Their license texts ship
in the `licenses` folder of every release.

| Library | Use | License |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.3 | Function hooking | BSD 2-Clause (`licenses/MinHook.txt`) |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.5 | The in-VR settings window | MIT (`licenses/DearImGui.txt`) |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.60 (loader, with jsoncpp) | Talking to the VR runtime | Apache 2.0 (`licenses/OpenXR-SDK.txt`), jsoncpp: MIT / public domain (`licenses/jsoncpp.txt`) |

Grim Dawn is © Crate Entertainment. This mod is not affiliated with or endorsed by Crate Entertainment.
It ships no game files: the Devotion constellations are read at run time from the game installed on the
player's PC.
