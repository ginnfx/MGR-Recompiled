#pragma once
#include <filesystem>
struct ImFont;
struct ImFontAtlas;

void MgrConfigureMenuFonts(ImFontAtlas* atlas, const std::filesystem::path& game_root);
ImFont* MgrMenuLabelFont();
ImFont* MgrMenuValueFont();
