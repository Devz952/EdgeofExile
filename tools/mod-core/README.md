# mod-core (Edge of Exile D2RLoader plugin)

Source of `EdgeofExile/d2rloader/plugins/mod-core.dll` (D2R 3.3.0 build 93847, D2RLoader 1.3.1, PluginSDK ABI 4).

Build without Visual Studio (Linux or Windows, Python + zig):

```sh
pip install ziglang
SDK=PluginSDK-master/include
python -m ziglang rc /i $SDK src/plugin.rc plugin.res
python -m ziglang c++ -target x86_64-windows-gnu -std=c++20 -O2 -shared \
  -Wno-missing-designated-field-initializers -I $SDK \
  -DMODKIT_PLUGIN_NAME='"mod-core"' -DMODKIT_PLUGIN_VERSION='"0.2.5"' \
  src/plugin.cpp src/d2r.cpp src/skill_functions.cpp src/hook_example.cpp src/stats_panel.cpp plugin.res -o mod-core.dll
```

Features (switch any off by listing its word in `mod-core-off.txt` next to the DLL): `skills` (srvdofunc 172 missile
fan, 173 Arcane Warp teleport nova), `hook` (example), `stats` (advanced stat page in the Advanced Stats panel).
The generators in `../generators` produce the panel layout and the table rows for the elemental/splash properties.
