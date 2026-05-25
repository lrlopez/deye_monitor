#pragma once

// ── Iconos FontAwesome 5 Free adicionales ─────────────────────────────────
// Estos codepoints NO forman parte del subset built-in de LVGL.
// Para que se rendericen hay que incluirlos en la fuente personalizada
// generada con lv_font_conv (o la herramienta online de LVGL):
//
//   Codepoints a añadir al generar cada tamaño de fuente:
//     0xF073  fa-calendar-alt    → LV_SYMBOL_CALENDAR
//     0xF080  fa-chart-bar       → LV_SYMBOL_CHART
//     0xF185  fa-sun             → LV_SYMBOL_SUN
//     0xF186  fa-moon            → LV_SYMBOL_MOON
//     0xF1E6  fa-plug            → LV_SYMBOL_PLUG
//
// UTF-8 para codepoints U+F000..U+FFFF:  EF 8x xx → 0xEF | (0x80|(cp>>6)) | (0x80|(cp&0x3F))

#define LV_SYMBOL_CALENDAR  "\xEF\x81\xB3"   // U+F073  fa-calendar-alt
#define LV_SYMBOL_CHART     "\xEF\x82\x80"   // U+F080  fa-chart-bar
#define LV_SYMBOL_SUN       "\xEF\x86\x85"   // U+F185  fa-sun
#define LV_SYMBOL_MOON      "\xEF\x86\x86"   // U+F186  fa-moon
#define LV_SYMBOL_PLUG      "\xEF\x87\xA6"   // U+F1E6  fa-plug
