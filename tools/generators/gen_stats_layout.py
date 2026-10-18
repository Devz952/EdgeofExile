# Generates src/stats_panel_layout.h: the HD layout of the Edge of Exile advanced stat page (one TextBoxWidget per
# row, so no line-spacing guesses), embedded into the plugin as a raw string.
import json, os

ROWS = 33            # value rows filled by the plugin (L<n> label, V<n> value)
SECTIONS = [         # (header widget, first row, row count)
    ("H0", 0, 7),    # breakpoints
    ("H1", 7, 6),    # resistances
    ("H2", 13, 5),   # defense
    ("H3", 18, 3),   # find
    ("H4", 21, 7),   # offense
    ("H5", 28, 5),   # sustain
]
ROW_H = 28
HEAD_H = 34
GAP = 6
X0, W = 124, 632
LABEL_W = 214

def style(color, size, h):
    return {"fontColor": color, "pointSize": size, "alignment": {"h": h, "v": "center"}}

# Stock Advanced Stats panel (D2R 3.3.0 advancedstatspanelhd.json), kept whole so the panel code finds every widget
# it expects. The plugin hides ListContainer / ScrollBar / ScrollBackground once it has filled the EoE rows.
stock = {
    "type": "AdvancedStatsPanel", "name": "AdvancedStatsPanel",
    "fields": {"rect": {"x": 1398, "y": -651}, "anchor": "$LeftPanelAnchor", "defaultWidget": "ScrollBar",
               "textPadding": 16},
    "children": [
        {"type": "ImageWidget", "name": "Background",
         "fields": {"filename": "PANEL\\AdvancedCharacterSheet\\AdvancedCharacterSheet_BG"}},
        {"type": "ClickCatcherWidget", "name": "ClickCatcher", "fields": {"rect": {"width": 875, "height": 1500}}},
        {"type": "ImageWidget", "name": "ScrollBackground",
         "fields": {"rect": {"x": 765, "y": 150},
                    "filename": "PANEL\\AdvancedCharacterSheet\\AdvancedCharacterSheet_ScrollBar"}},
        {"type": "ScrollControllerWidget", "name": "ScrollBar",
         "fields": {"rect": {"x": 765, "y": 160, "width": 36, "height": 1233}, "upArrowFilepath": "",
                    "downArrowFilepath": "",
                    "barFilepath": "PANEL\\AdvancedCharacterSheet\\AdvancedCharacterSheet_ScrollIndicator",
                    "viewName": "ListContainer"}},
        {"type": "TextBoxWidget", "name": "Title",
         "fields": {"text": "@AdvancedStats", "rect": {"x": 120, "y": 61, "width": 640, "height": 85},
                    "style": "$StyleTitleBlock"}},
        {"type": "ListWidget", "name": "ListContainer",
         "fields": {"rect": {"x": 148, "y": 212, "width": 576, "height": 1231}, "direction": "vertical",
                    "scrollControllerName": "ScrollBar"},
         "children": [
             {"type": "ListItemWidget", "name": "OptionTemplate",
              "fields": {"rect": {"x": 0, "y": 10, "width": 576, "height": 70}},
              "children": [
                  {"type": "NineTileImageWidget", "name": "TextContainer",
                   "fields": {"rect": {"x": 0, "y": 0, "width": 576, "height": 0},
                              "filename": "PANEL\\AdvancedCharacterSheet\\AdvancedCharacterSheet_Cell",
                              "tileScale": 1.0, "allowScissorResizing": False}},
                  {"type": "TextBoxWidget", "name": "StatText",
                   "fields": {"rect": {"x": 20, "y": 0, "width": 516, "height": 50},
                              "style": {"fontColor": "$FontColorWhite", "pointSize": "$SmallFontSize",
                                        "alignment": {"h": "center", "v": "center"},
                                        "options": {"lineWrap": True, "pipeCharacterBehavior": "handle"}}}},
              ]}
         ]},
        {"type": "ButtonWidget", "name": "CloseButton",
         "fields": {"rect": {"x": 791, "y": 10}, "filename": "PANEL\\closebtn_4x", "hoveredFrame": 3,
                    "onClickMessage": "PanelManager:ClosePanel:AdvancedStatsPanel", "tooltipString": "@strClose",
                    "sound": "cursor_close_window_hd"}},
    ],
}
children = stock["children"]

y = 160
children.append({"type": "TextBoxWidget", "name": "EoESummary",
                 "fields": {"text": "", "rect": {"x": X0, "y": y, "width": W, "height": ROW_H + 4},
                            "style": style("$FontColorLightGold", "$SmallFontSize", "center")}})
y += ROW_H + 4 + GAP
for head, first, count in SECTIONS:
    children.append({"type": "TextBoxWidget", "name": "EoE" + head,
                     "fields": {"text": "", "rect": {"x": X0, "y": y, "width": W, "height": HEAD_H},
                                "style": style("$FontColorGoldYellow", "$MediumFontSize", "left")}})
    y += HEAD_H
    for r in range(first, first + count):
        children.append({"type": "TextBoxWidget", "name": f"EoEL{r}",
                         "fields": {"text": "", "rect": {"x": X0 + 6, "y": y, "width": LABEL_W, "height": ROW_H},
                                    "style": style("$FontColorBeige", "$SmallFontSize", "left")}})
        children.append({"type": "TextBoxWidget", "name": f"EoEV{r}",
                         "fields": {"text": "", "rect": {"x": X0 + 6 + LABEL_W, "y": y,
                                                         "width": W - 6 - LABEL_W, "height": ROW_H},
                                    "style": style("$FontColorWhite", "$SmallFontSize", "right")}})
        y += ROW_H
    y += GAP
children.append({"type": "TextBoxWidget", "name": "EoEFooter",
                 "fields": {"text": "", "rect": {"x": X0, "y": y, "width": W, "height": ROW_H},
                            "style": style("$FontColorGray", "$SmallFontSize", "center")}})
assert y + ROW_H <= 1450, y
assert sum(c for _, _, c in SECTIONS) == ROWS

layout = stock
text = json.dumps(layout, indent=1)
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(os.path.join(here, "out"), exist_ok=True)
with open(os.path.join(here, "out", "advancedstatspanelhd.json"), "w", newline="\n") as f:
    f.write(text + "\n")
with open(os.path.join(here, "src", "stats_panel_layout.h"), "w", newline="\n") as f:
    f.write("// Generated by gen_stats_layout.py. Do not edit by hand.\n#pragma once\n\n")
    f.write(f"inline constexpr int StatsRows = {ROWS};\n")
    f.write(f"inline constexpr int StatsSections = {len(SECTIONS)};\n")
print("rows end at y", y + ROW_H, "bytes", len(text))
