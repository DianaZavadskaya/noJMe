/*
 * SDL2 Graphics Backend — JS ESM stub
 * Migrated from src/sdl/sdl_graphics.c (1731 lines)
 *
 * SDL2 API calls are delegated to injected renderer callbacks
 * (set via set_sdl_renderer_callbacks). Audio is delegated to
 * queued callbacks. Input events are delivered via inject_input_event.
 * pthreads mutexes are omitted (single-threaded JS event loop).
 */

// ---------------------------------------------------------------------------
// MIDP key code constants (from include/sdl_backend.h)
// ---------------------------------------------------------------------------

export const MIDP_KEY_0          =  48;
export const MIDP_KEY_1          =  49;
export const MIDP_KEY_2          =  50;
export const MIDP_KEY_3          =  51;
export const MIDP_KEY_4          =  52;
export const MIDP_KEY_5          =  53;
export const MIDP_KEY_6          =  54;
export const MIDP_KEY_7          =  55;
export const MIDP_KEY_8          =  56;
export const MIDP_KEY_9          =  57;
export const MIDP_KEY_STAR       =  42;
export const MIDP_KEY_POUND      =  35;
export const MIDP_KEY_UP         =  -1;
export const MIDP_KEY_DOWN       =  -2;
export const MIDP_KEY_LEFT       =  -3;
export const MIDP_KEY_RIGHT      =  -4;
export const MIDP_KEY_SELECT     =  -5;
export const MIDP_KEY_SOFT_LEFT  =  -6;
export const MIDP_KEY_SOFT_RIGHT =  -7;
export const MIDP_KEY_CLEAR      =  -8;
export const MIDP_KEY_SEND       =  -9;
export const MIDP_KEY_END        = -10;

// ---------------------------------------------------------------------------
// Game action constants (from include/midp.h)
// ---------------------------------------------------------------------------

export const GAME_UP    = 1;
export const GAME_DOWN  = 6;
export const GAME_LEFT  = 2;
export const GAME_RIGHT = 5;
export const GAME_FIRE  = 8;
export const GAME_A     = 9;
export const GAME_B     = 10;
export const GAME_C     = 11;
export const GAME_D     = 12;

// ---------------------------------------------------------------------------
// Key mapping table (mirrors key_map[] in sdl_graphics.c)
// sdl_key values use Web KeyboardEvent.code / key strings (for JS callers)
// The numeric sdl_key values here mirror the SDL_Keycode identifiers used
// internally — callers can pass whatever key ID scheme they prefer via
// inject_input_event() as long as they also update KEY_MAP accordingly.
// ---------------------------------------------------------------------------

// SDL_Keycode numeric constants (subset used in the original key_map)
const SDLK_0       = 48;
const SDLK_1       = 49;
const SDLK_2       = 50;
const SDLK_3       = 51;
const SDLK_4       = 52;
const SDLK_5       = 53;
const SDLK_6       = 54;
const SDLK_7       = 55;
const SDLK_8       = 56;
const SDLK_9       = 57;
const SDLK_ASTERISK= 42;
const SDLK_HASH    = 35;
const SDLK_UP      = 1073741906;
const SDLK_DOWN    = 1073741905;
const SDLK_LEFT    = 1073741904;
const SDLK_RIGHT   = 1073741903;
const SDLK_RETURN  = 13;
const SDLK_SPACE   = 32;
const SDLK_F1      = 1073741882;
const SDLK_F2      = 1073741883;
const SDLK_F12     = 1073741893;
const SDLK_a       = 97;
const SDLK_b       = 98;
const SDLK_c       = 99;
const SDLK_d       = 100;
const SDLK_ESCAPE  = 27;

// { sdl_key, midp_key, game_action }
const KEY_MAP = [
    { sdl_key: SDLK_0,        midp_key: 48,  game_action: 0 },
    { sdl_key: SDLK_1,        midp_key: 49,  game_action: 0 },
    { sdl_key: SDLK_2,        midp_key: 50,  game_action: 0 },
    { sdl_key: SDLK_3,        midp_key: 51,  game_action: 0 },
    { sdl_key: SDLK_4,        midp_key: 52,  game_action: 0 },
    { sdl_key: SDLK_5,        midp_key: 53,  game_action: 0 },
    { sdl_key: SDLK_6,        midp_key: 54,  game_action: 0 },
    { sdl_key: SDLK_7,        midp_key: 55,  game_action: 0 },
    { sdl_key: SDLK_8,        midp_key: 56,  game_action: 0 },
    { sdl_key: SDLK_9,        midp_key: 57,  game_action: 0 },
    { sdl_key: SDLK_ASTERISK, midp_key: 42,  game_action: 0 },
    { sdl_key: SDLK_HASH,     midp_key: 35,  game_action: 0 },
    { sdl_key: SDLK_UP,       midp_key: -1,  game_action: GAME_UP   },
    { sdl_key: SDLK_DOWN,     midp_key: -2,  game_action: GAME_DOWN },
    { sdl_key: SDLK_LEFT,     midp_key: -3,  game_action: GAME_LEFT },
    { sdl_key: SDLK_RIGHT,    midp_key: -4,  game_action: GAME_RIGHT},
    { sdl_key: SDLK_RETURN,   midp_key: -5,  game_action: GAME_FIRE },
    { sdl_key: SDLK_SPACE,    midp_key: -5,  game_action: GAME_FIRE },
    { sdl_key: SDLK_F1,       midp_key: -6,  game_action: 0 },
    { sdl_key: SDLK_F2,       midp_key: -7,  game_action: 0 },
    { sdl_key: SDLK_a,        midp_key: -6,  game_action: GAME_A },
    { sdl_key: SDLK_b,        midp_key: -7,  game_action: GAME_B },
    { sdl_key: SDLK_c,        midp_key: -8,  game_action: GAME_C },
    { sdl_key: SDLK_d,        midp_key: -9,  game_action: GAME_D },
    { sdl_key: SDLK_ESCAPE,   midp_key: 0,   game_action: 0 },
];

// ---------------------------------------------------------------------------
// Software key repeat constants (mirrors C #defines)
// ---------------------------------------------------------------------------

const SDL_KEY_REPEAT_DELAY_MS    = 400;
const SDL_KEY_REPEAT_INTERVAL_MS = 80;
const SDL_KEY_REPEAT_MAX_KEYS    = 256;

// ---------------------------------------------------------------------------
// Bitmap font data (from src/midp/bitmap_font.h — mirrored from graphics.mjs)
// ---------------------------------------------------------------------------

const FONT_WIDTH  = 5;
const FONT_HEIGHT = 7;

const FONT_RANGE1_START = 0x20;
const FONT_RANGE1_END   = 0x7E;
const FONT_RANGE1_COUNT = 95;

const FONT_RANGE2_START = 0xA0;
const FONT_RANGE2_END   = 0xFF;
const FONT_RANGE2_COUNT = 96;

const FONT_RANGE3_START = 0x400;
const FONT_RANGE3_END   = 0x45F;

// Each character: FONT_HEIGHT bytes, one per row, MSB = leftmost pixel.
const BITMAP_FONT = new Uint8Array([
  // Range 1: Basic ASCII (0x20-0x7E)
  0x00,0x00,0x00,0x00,0x00,0x00,0x00, // Space (0x20)
  0x04,0x04,0x04,0x04,0x04,0x00,0x04, // !
  0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00, // "
  0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A, // #
  0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04, // $
  0x18,0x19,0x02,0x04,0x08,0x13,0x03, // %
  0x08,0x14,0x14,0x08,0x15,0x12,0x0D, // &
  0x04,0x04,0x08,0x00,0x00,0x00,0x00, // '
  0x02,0x04,0x08,0x08,0x08,0x04,0x02, // (
  0x08,0x04,0x02,0x02,0x02,0x04,0x08, // )
  0x04,0x15,0x0E,0x1F,0x0E,0x15,0x04, // *
  0x00,0x04,0x04,0x1F,0x04,0x04,0x00, // +
  0x00,0x00,0x00,0x00,0x00,0x04,0x08, // ,
  0x00,0x00,0x00,0x1F,0x00,0x00,0x00, // -
  0x00,0x00,0x00,0x00,0x00,0x04,0x04, // .
  0x00,0x01,0x02,0x04,0x08,0x10,0x00, // /
  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, // 0
  0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, // 1
  0x0E,0x11,0x01,0x06,0x08,0x10,0x1F, // 2
  0x0E,0x11,0x01,0x06,0x01,0x11,0x0E, // 3
  0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, // 4
  0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E, // 5
  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, // 6
  0x1F,0x01,0x02,0x04,0x08,0x08,0x08, // 7
  0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E, // 8
  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, // 9
  0x00,0x04,0x04,0x00,0x04,0x04,0x00, // :
  0x00,0x04,0x04,0x00,0x04,0x08,0x00, // ;
  0x02,0x04,0x08,0x10,0x08,0x04,0x02, // <
  0x00,0x00,0x1F,0x00,0x1F,0x00,0x00, // =
  0x08,0x04,0x02,0x01,0x02,0x04,0x08, // >
  0x0E,0x11,0x01,0x02,0x04,0x00,0x04, // ?
  0x0E,0x11,0x17,0x15,0x17,0x10,0x0E, // @
  0x0E,0x11,0x11,0x11,0x1F,0x11,0x11, // A
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, // B
  0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, // C
  0x1E,0x11,0x11,0x11,0x11,0x11,0x1E, // D
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, // E
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, // F
  0x0E,0x11,0x10,0x17,0x11,0x11,0x0F, // G
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // H
  0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, // I
  0x01,0x01,0x01,0x01,0x01,0x11,0x0E, // J
  0x11,0x12,0x14,0x18,0x14,0x12,0x11, // K
  0x10,0x10,0x10,0x10,0x10,0x10,0x1F, // L
  0x11,0x1B,0x15,0x15,0x11,0x11,0x11, // M
  0x11,0x19,0x15,0x13,0x11,0x11,0x11, // N
  0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // O
  0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, // P
  0x0E,0x11,0x11,0x11,0x15,0x12,0x0D, // Q
  0x1E,0x11,0x11,0x1E,0x14,0x12,0x11, // R
  0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E, // S
  0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // T
  0x11,0x11,0x11,0x11,0x11,0x11,0x0E, // U
  0x11,0x11,0x11,0x11,0x11,0x0A,0x04, // V
  0x11,0x11,0x11,0x15,0x15,0x1B,0x11, // W
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // X
  0x11,0x11,0x0A,0x04,0x04,0x04,0x04, // Y
  0x1F,0x01,0x02,0x04,0x08,0x10,0x1F, // Z
  0x0E,0x08,0x08,0x08,0x08,0x08,0x0E, // [
  0x00,0x10,0x08,0x04,0x02,0x01,0x00, // backslash
  0x0E,0x02,0x02,0x02,0x02,0x02,0x0E, // ]
  0x04,0x0A,0x11,0x00,0x00,0x00,0x00, // ^
  0x00,0x00,0x00,0x00,0x00,0x00,0x1F, // _
  0x08,0x04,0x02,0x00,0x00,0x00,0x00, // `
  0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F, // a
  0x10,0x10,0x1E,0x11,0x11,0x11,0x1E, // b
  0x00,0x00,0x0E,0x11,0x10,0x11,0x0E, // c
  0x01,0x01,0x0F,0x11,0x11,0x11,0x0F, // d
  0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E, // e
  0x06,0x08,0x08,0x1E,0x08,0x08,0x08, // f
  0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, // g
  0x10,0x10,0x1E,0x11,0x11,0x11,0x11, // h
  0x04,0x00,0x0C,0x04,0x04,0x04,0x0E, // i
  0x02,0x00,0x06,0x02,0x02,0x02,0x12, // j
  0x10,0x10,0x12,0x14,0x18,0x14,0x12, // k
  0x0C,0x04,0x04,0x04,0x04,0x04,0x0E, // l
  0x00,0x00,0x1A,0x15,0x15,0x11,0x11, // m
  0x00,0x00,0x16,0x19,0x11,0x11,0x11, // n
  0x00,0x00,0x0E,0x11,0x11,0x11,0x0E, // o
  0x00,0x00,0x1E,0x11,0x11,0x1E,0x10, // p
  0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, // q
  0x00,0x00,0x16,0x09,0x08,0x08,0x08, // r
  0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E, // s
  0x08,0x08,0x1C,0x08,0x08,0x09,0x06, // t
  0x00,0x00,0x11,0x11,0x11,0x13,0x0D, // u
  0x00,0x00,0x11,0x11,0x11,0x0A,0x04, // v
  0x00,0x00,0x11,0x11,0x15,0x1F,0x0A, // w
  0x00,0x00,0x11,0x0A,0x04,0x0A,0x11, // x
  0x00,0x00,0x11,0x11,0x0F,0x01,0x0E, // y
  0x00,0x00,0x1F,0x02,0x04,0x08,0x1F, // z
  0x06,0x08,0x08,0x18,0x08,0x08,0x06, // {
  0x04,0x04,0x04,0x00,0x04,0x04,0x04, // |
  0x0C,0x02,0x02,0x03,0x02,0x02,0x0C, // }
  0x08,0x15,0x02,0x00,0x00,0x00,0x00, // ~
  // Range 2: Latin-1 Supplement (0xA0-0xFF) — 96 chars, mostly stubs
  0x00,0x00,0x00,0x00,0x00,0x00,0x00, // 0xA0 NBSP
  0x04,0x04,0x04,0x04,0x04,0x00,0x04, // 0xA1 ¡
  0x0A,0x0A,0x1F,0x0A,0x0A,0x00,0x00, // 0xA2 ¢
  0x06,0x04,0x0E,0x04,0x04,0x04,0x1E, // 0xA3 £
  0x00,0x11,0x0E,0x00,0x0E,0x11,0x00, // 0xA4 ¤
  0x11,0x0A,0x1F,0x04,0x1F,0x04,0x04, // 0xA5 ¥
  0x04,0x04,0x04,0x00,0x04,0x04,0x04, // 0xA6 ¦
  0x06,0x09,0x0C,0x06,0x03,0x09,0x06, // 0xA7 §
  0x0A,0x0A,0x00,0x00,0x00,0x00,0x00, // 0xA8 ¨
  0x0E,0x11,0x13,0x15,0x13,0x11,0x0E, // 0xA9 ©
  0x06,0x09,0x0B,0x05,0x00,0x0F,0x00, // 0xAA ª
  0x00,0x05,0x0A,0x14,0x0A,0x05,0x00, // 0xAB «
  0x00,0x00,0x1F,0x01,0x01,0x00,0x00, // 0xAC ¬
  0x00,0x00,0x1F,0x00,0x00,0x00,0x00, // 0xAD SHY
  0x0E,0x11,0x11,0x1D,0x11,0x11,0x1E, // 0xAE ®
  0x1F,0x00,0x00,0x00,0x00,0x00,0x00, // 0xAF ¯
  0x06,0x09,0x09,0x06,0x00,0x00,0x00, // 0xB0 °
  0x04,0x04,0x1F,0x04,0x04,0x00,0x1F, // 0xB1 ±
  0x0C,0x02,0x04,0x08,0x0E,0x00,0x00, // 0xB2 ²
  0x0E,0x02,0x04,0x02,0x0E,0x00,0x00, // 0xB3 ³
  0x04,0x08,0x00,0x00,0x00,0x00,0x00, // 0xB4 ´
  0x11,0x11,0x11,0x11,0x0F,0x01,0x0E, // 0xB5 µ
  0x0F,0x15,0x15,0x0F,0x05,0x05,0x05, // 0xB6 ¶
  0x00,0x00,0x00,0x04,0x04,0x00,0x00, // 0xB7 ·
  0x00,0x00,0x00,0x00,0x04,0x04,0x08, // 0xB8 ¸
  0x04,0x0C,0x04,0x04,0x04,0x00,0x00, // 0xB9 ¹
  0x06,0x09,0x09,0x06,0x00,0x0F,0x00, // 0xBA º
  0x00,0x0A,0x05,0x02,0x05,0x0A,0x00, // 0xBB »
  0x10,0x10,0x10,0x16,0x09,0x0A,0x07, // 0xBC ¼
  0x10,0x10,0x10,0x15,0x09,0x0A,0x07, // 0xBD ½
  0x18,0x04,0x08,0x16,0x09,0x0A,0x07, // 0xBE ¾
  0x0E,0x11,0x01,0x02,0x04,0x00,0x04, // 0xBF ¿
  0x02,0x04,0x0E,0x11,0x1F,0x11,0x11, // 0xC0 À
  0x08,0x04,0x0E,0x11,0x1F,0x11,0x11, // 0xC1 Á
  0x04,0x0A,0x0E,0x11,0x1F,0x11,0x11, // 0xC2 Â
  0x0D,0x12,0x0E,0x11,0x1F,0x11,0x11, // 0xC3 Ã
  0x0A,0x00,0x0E,0x11,0x1F,0x11,0x11, // 0xC4 Ä
  0x04,0x0A,0x04,0x0E,0x1F,0x11,0x11, // 0xC5 Å
  0x07,0x0C,0x14,0x1F,0x10,0x10,0x1F, // 0xC6 Æ
  0x0E,0x11,0x10,0x10,0x11,0x0E,0x04, // 0xC7 Ç
  0x02,0x04,0x1F,0x10,0x1E,0x10,0x1F, // 0xC8 È
  0x08,0x04,0x1F,0x10,0x1E,0x10,0x1F, // 0xC9 É
  0x04,0x0A,0x1F,0x10,0x1E,0x10,0x1F, // 0xCA Ê
  0x0A,0x00,0x1F,0x10,0x1E,0x10,0x1F, // 0xCB Ë
  0x02,0x04,0x0E,0x04,0x04,0x04,0x0E, // 0xCC Ì
  0x08,0x04,0x0E,0x04,0x04,0x04,0x0E, // 0xCD Í
  0x04,0x0A,0x0E,0x04,0x04,0x04,0x0E, // 0xCE Î
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // 0xCF Ï
  0x1C,0x12,0x11,0x1D,0x11,0x12,0x1C, // 0xD0 Ð
  0x0D,0x12,0x11,0x19,0x15,0x13,0x11, // 0xD1 Ñ
  0x02,0x04,0x0E,0x11,0x11,0x11,0x0E, // 0xD2 Ò
  0x08,0x04,0x0E,0x11,0x11,0x11,0x0E, // 0xD3 Ó
  0x04,0x0A,0x0E,0x11,0x11,0x11,0x0E, // 0xD4 Ô
  0x0D,0x12,0x0E,0x11,0x11,0x11,0x0E, // 0xD5 Õ
  0x0A,0x00,0x0E,0x11,0x11,0x11,0x0E, // 0xD6 Ö
  0x00,0x0A,0x04,0x0A,0x00,0x00,0x00, // 0xD7 ×
  0x01,0x0F,0x12,0x15,0x12,0x0F,0x10, // 0xD8 Ø
  0x02,0x04,0x11,0x11,0x11,0x11,0x0E, // 0xD9 Ù
  0x08,0x04,0x11,0x11,0x11,0x11,0x0E, // 0xDA Ú
  0x04,0x0A,0x11,0x11,0x11,0x11,0x0E, // 0xDB Û
  0x0A,0x00,0x11,0x11,0x11,0x11,0x0E, // 0xDC Ü
  0x08,0x04,0x11,0x11,0x0A,0x04,0x04, // 0xDD Ý
  0x10,0x1E,0x11,0x1E,0x11,0x11,0x1E, // 0xDE Þ
  0x0C,0x12,0x12,0x1C,0x12,0x12,0x1D, // 0xDF ß
  0x02,0x04,0x00,0x0E,0x01,0x0F,0x11, // 0xE0 à
  0x08,0x04,0x00,0x0E,0x01,0x0F,0x11, // 0xE1 á
  0x04,0x0A,0x00,0x0E,0x01,0x0F,0x11, // 0xE2 â
  0x0D,0x12,0x00,0x0E,0x01,0x0F,0x11, // 0xE3 ã
  0x0A,0x00,0x00,0x0E,0x01,0x0F,0x11, // 0xE4 ä
  0x06,0x06,0x00,0x0E,0x01,0x0F,0x11, // 0xE5 å
  0x00,0x00,0x07,0x09,0x0F,0x18,0x0F, // 0xE6 æ
  0x00,0x00,0x0E,0x11,0x10,0x11,0x0E, // 0xE7 ç (reuse c)
  0x02,0x04,0x00,0x0E,0x1F,0x10,0x0E, // 0xE8 è
  0x08,0x04,0x00,0x0E,0x1F,0x10,0x0E, // 0xE9 é
  0x04,0x0A,0x00,0x0E,0x1F,0x10,0x0E, // 0xEA ê
  0x0A,0x00,0x00,0x0E,0x1F,0x10,0x0E, // 0xEB ë
  0x02,0x04,0x00,0x0C,0x04,0x04,0x0E, // 0xEC ì
  0x08,0x04,0x00,0x0C,0x04,0x04,0x0E, // 0xED í
  0x04,0x0A,0x00,0x0C,0x04,0x04,0x0E, // 0xEE î
  0x0A,0x00,0x00,0x0C,0x04,0x04,0x0E, // 0xEF ï
  0x06,0x08,0x0E,0x09,0x0E,0x08,0x06, // 0xF0 ð
  0x0D,0x12,0x00,0x16,0x19,0x11,0x11, // 0xF1 ñ
  0x02,0x04,0x00,0x0E,0x11,0x11,0x0E, // 0xF2 ò
  0x08,0x04,0x00,0x0E,0x11,0x11,0x0E, // 0xF3 ó
  0x04,0x0A,0x00,0x0E,0x11,0x11,0x0E, // 0xF4 ô
  0x0D,0x12,0x00,0x0E,0x11,0x11,0x0E, // 0xF5 õ
  0x0A,0x00,0x00,0x0E,0x11,0x11,0x0E, // 0xF6 ö
  0x00,0x04,0x00,0x1F,0x00,0x04,0x00, // 0xF7 ÷
  0x01,0x0F,0x12,0x15,0x12,0x0F,0x10, // 0xF8 ø
  0x02,0x04,0x11,0x11,0x11,0x13,0x0D, // 0xF9 ù
  0x08,0x04,0x11,0x11,0x11,0x13,0x0D, // 0xFA ú
  0x04,0x0A,0x11,0x11,0x11,0x13,0x0D, // 0xFB û
  0x0A,0x00,0x11,0x11,0x11,0x13,0x0D, // 0xFC ü
  0x08,0x04,0x11,0x11,0x0F,0x01,0x0E, // 0xFD ý
  0x10,0x1E,0x11,0x1E,0x11,0x11,0x1E, // 0xFE þ
  0x0A,0x00,0x11,0x11,0x0F,0x01,0x0E, // 0xFF ÿ
  // Range 3: Cyrillic (0x400-0x45F) — 96 chars, basic block
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x11, // 0x400 Ѐ (use Б shape)
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x401 Ё
  0x07,0x04,0x04,0x06,0x05,0x04,0x04, // 0x402 Ђ
  0x06,0x04,0x04,0x04,0x04,0x04,0x1F, // 0x403 Ѓ
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // 0x404 Є
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x405 Ѕ
  0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, // 0x406 І
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // 0x407 Ї
  0x07,0x04,0x04,0x06,0x04,0x04,0x07, // 0x408 Ј
  0x11,0x11,0x11,0x19,0x16,0x10,0x10, // 0x409 Љ (approx)
  0x11,0x11,0x11,0x11,0x1F,0x11,0x11, // 0x40A Њ (approx)
  0x0E,0x11,0x11,0x11,0x1F,0x11,0x11, // 0x40B Ћ
  0x06,0x04,0x04,0x04,0x04,0x04,0x1F, // 0x40C Ќ
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x40D Ѝ
  0x11,0x11,0x11,0x11,0x11,0x11,0x1F, // 0x40E Ў
  0x1F,0x10,0x10,0x1F,0x10,0x10,0x1F, // 0x40F Џ
  0x1F,0x11,0x11,0x11,0x11,0x11,0x11, // 0x410 А
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x411 Б
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, // 0x412 В
  0x1F,0x10,0x10,0x10,0x10,0x10,0x10, // 0x413 Г
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x414 Д
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, // 0x415 Е
  0x11,0x0A,0x1F,0x04,0x1F,0x0A,0x11, // 0x416 Ж
  0x0E,0x11,0x01,0x06,0x01,0x11,0x0E, // 0x417 З
  0x11,0x11,0x11,0x1F,0x15,0x15,0x11, // 0x418 И
  0x0A,0x04,0x11,0x11,0x1F,0x11,0x11, // 0x419 Й
  0x11,0x12,0x14,0x1C,0x14,0x12,0x11, // 0x41A К
  0x07,0x09,0x11,0x11,0x11,0x11,0x11, // 0x41B Л
  0x11,0x1B,0x15,0x11,0x11,0x11,0x11, // 0x41C М
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // 0x41D Н
  0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // 0x41E О
  0x1F,0x11,0x11,0x11,0x11,0x11,0x1F, // 0x41F П
  0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, // 0x420 Р
  0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, // 0x421 С
  0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // 0x422 Т
  0x11,0x11,0x11,0x0F,0x01,0x11,0x0E, // 0x423 У
  0x04,0x0E,0x15,0x15,0x0E,0x04,0x00, // 0x424 Ф
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // 0x425 Х
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // 0x426 Ц (approx)
  0x11,0x11,0x11,0x0F,0x01,0x01,0x01, // 0x427 Ч
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x428 Ш
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x429 Щ
  0x18,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x42A Ъ
  0x11,0x11,0x11,0x1B,0x15,0x15,0x1B, // 0x42B Ы
  0x10,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x42C Ь
  0x0E,0x11,0x01,0x07,0x01,0x11,0x0E, // 0x42D Э
  0x11,0x11,0x19,0x15,0x1B,0x11,0x11, // 0x42E Ю
  0x0F,0x11,0x11,0x0F,0x09,0x11,0x11, // 0x42F Я
  0x1F,0x11,0x11,0x11,0x11,0x11,0x11, // 0x430 а (use А shape)
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x431 б
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, // 0x432 в
  0x1F,0x10,0x10,0x10,0x10,0x10,0x10, // 0x433 г
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x434 д
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, // 0x435 е
  0x11,0x0A,0x1F,0x04,0x1F,0x0A,0x11, // 0x436 ж
  0x0E,0x11,0x01,0x06,0x01,0x11,0x0E, // 0x437 з
  0x11,0x11,0x11,0x1F,0x15,0x15,0x11, // 0x438 и
  0x0A,0x04,0x11,0x11,0x1F,0x11,0x11, // 0x439 й
  0x11,0x12,0x14,0x1C,0x14,0x12,0x11, // 0x43A к
  0x07,0x09,0x11,0x11,0x11,0x11,0x11, // 0x43B л
  0x11,0x1B,0x15,0x11,0x11,0x11,0x11, // 0x43C м
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // 0x43D н
  0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // 0x43E о
  0x1F,0x11,0x11,0x11,0x11,0x11,0x1F, // 0x43F п
  0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, // 0x440 р
  0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, // 0x441 с
  0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // 0x442 т
  0x11,0x11,0x11,0x0F,0x01,0x11,0x0E, // 0x443 у
  0x04,0x0E,0x15,0x15,0x0E,0x04,0x00, // 0x444 ф
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // 0x445 х
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // 0x446 ц
  0x11,0x11,0x11,0x0F,0x01,0x01,0x01, // 0x447 ч
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x448 ш
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // 0x449 щ
  0x18,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x44A ъ
  0x11,0x11,0x11,0x1B,0x15,0x15,0x1B, // 0x44B ы
  0x10,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x44C ь
  0x0E,0x11,0x01,0x07,0x01,0x11,0x0E, // 0x44D э
  0x11,0x11,0x19,0x15,0x1B,0x11,0x11, // 0x44E ю
  0x0F,0x11,0x11,0x0F,0x09,0x11,0x11, // 0x44F я
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x450 ѐ
  0x0A,0x00,0x1F,0x10,0x1E,0x10,0x1F, // 0x451 ё
  0x07,0x04,0x04,0x06,0x05,0x04,0x04, // 0x452 ђ
  0x06,0x04,0x04,0x04,0x04,0x04,0x1F, // 0x453 ѓ
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // 0x454 є
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x455 ѕ
  0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, // 0x456 і
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // 0x457 ї
  0x07,0x04,0x04,0x06,0x04,0x04,0x07, // 0x458 ј
  0x11,0x11,0x11,0x19,0x16,0x10,0x10, // 0x459 љ
  0x11,0x11,0x11,0x11,0x1F,0x11,0x11, // 0x45A њ
  0x0E,0x11,0x11,0x11,0x1F,0x11,0x11, // 0x45B ћ
  0x06,0x04,0x04,0x04,0x04,0x04,0x1F, // 0x45C ќ
  0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E, // 0x45D ѝ
  0x11,0x11,0x11,0x11,0x11,0x11,0x1F, // 0x45E ў
  0x1F,0x10,0x10,0x1F,0x10,0x10,0x1F, // 0x45F џ
]);

// ---------------------------------------------------------------------------
// Module-level globals
// ---------------------------------------------------------------------------

/** @type {object|null} */
let g_sdl_ctx_ptr = null;

// Renderer callbacks (injected by set_sdl_renderer_callbacks)
let g_renderer_callbacks = null;

// Error display state
let g_error_title   = '';
let g_error_message = '';
let g_error_stack   = '';
let g_error_extra   = '';
let g_has_error     = false;

// Key repeat state (mirrors g_sdl_key_repeat struct)
const g_sdl_key_repeat = {
    press_time:       new Float64Array(SDL_KEY_REPEAT_MAX_KEYS),
    last_repeat_time: new Float64Array(SDL_KEY_REPEAT_MAX_KEYS),
    is_held:          new Uint8Array(SDL_KEY_REPEAT_MAX_KEYS),
};

// Screen graphics (plain object reused across calls)
const g_sdl_graphics = {
    pixels:       null,
    width:        0,
    height:       0,
    clip_x:       0,
    clip_y:       0,
    clip_width:   0,
    clip_height:  0,
    translate_x:  0,
    translate_y:  0,
    rgb_color:    0,
    alpha:        255,
    stroke_style: 0,
    font:         0,
};

// ---------------------------------------------------------------------------
// Renderer callback injection
// ---------------------------------------------------------------------------

/**
 * Inject rendering and audio callbacks.
 * Any subset of the following properties may be provided:
 *   presentFrame(framebuffer, width, height)
 *   queueAudio(samples: Int16Array, count: number)
 *   getQueuedAudioSize() → number
 *   clearAudio()
 *   initAudio(sampleRate, channels, bufferSamples) → number
 *   shutdownAudio()
 * @param {object} callbacks
 */
export function set_sdl_renderer_callbacks(callbacks) {
    g_renderer_callbacks = callbacks || null;
}

// ---------------------------------------------------------------------------
// SdlContext factory
// ---------------------------------------------------------------------------

function makeSdlContext() {
    return {
        window:      null,
        renderer:    null,
        texture:     null,
        framebuffer: null,
        width:       0,
        height:      0,
        scale:       1,
        input: {
            key_states:      new Array(256).fill(false),
            pointer_x:       0,
            pointer_y:       0,
            pointer_pressed: false,
        },
        audio: {
            device:   0,
            frequency: 0,
            channels:  0,
            samples:   0,
            enabled:   false,
        },
        start_time:   0,
        frame_time:   0,
        target_fps:   60,
        vsync:        false,
        running:      false,
        minimized:    false,
        fullscreen:   false,
        headless:     false,
        needs_redraw: false,
        jvm:          null,
    };
}

// ---------------------------------------------------------------------------
// Context accessors
// ---------------------------------------------------------------------------

export function sdl_set_global_context(ctx) {
    g_sdl_ctx_ptr = ctx;
}

export function sdl_get_global_context() {
    return g_sdl_ctx_ptr;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

/**
 * @param {object}  jvm
 * @param {number}  width
 * @param {number}  height
 * @param {number}  scale
 * @param {boolean} headless
 * @returns {number}
 */
export function sdl_init(jvm, width, height, scale, headless) {
    if (!g_sdl_ctx_ptr) {
        process.stderr.write('[SDL] ERROR: g_sdl_ctx_ptr is NULL - must be set before sdl_init\n');
        return -1;
    }

    const ctx = g_sdl_ctx_ptr;
    ctx.width         = width;
    ctx.height        = height;
    ctx.scale         = scale > 0 ? scale : 1;
    ctx.jvm           = jvm;
    ctx.running       = true;
    ctx.target_fps    = 60;
    ctx.headless      = !!headless;
    ctx.needs_redraw  = false;
    ctx.input.pointer_x       = 0;
    ctx.input.pointer_y       = 0;
    ctx.input.pointer_pressed = false;
    ctx.input.key_states.fill(false);

    try {
        ctx.framebuffer = new Uint32Array(width * height);
        ctx.framebuffer.fill(0xFF000000);
    } catch (e) {
        process.stderr.write(`[SDL] Failed to allocate framebuffer\n`);
        return -1;
    }

    process.stderr.write(`[SDL] Initialized ${width}x${height} scale=${ctx.scale} headless=${ctx.headless}\n`);
    return 0;
}

/**
 * @param {object|null} ctx
 */
export function sdl_destroy(ctx) {
    if (ctx && ctx.framebuffer) {
        ctx.framebuffer = null;
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

/**
 * Run the SDL main loop.
 * Caller must supply a callbacks object with the same optional methods as
 * sdl_headless.mjs sdl_run(), plus an optional onFrame(ctx) hook.
 * @param {object|null} ctx
 * @param {object} [callbacks={}]
 * @returns {Promise<void>}
 */
export async function sdl_run(ctx, callbacks = {}) {
    if (!ctx) return;

    ctx.running = true;
    if (ctx.jvm) ctx.jvm.running = true;

    const cb = callbacks;

    const frame_ms  = 1000 / (ctx.target_fps || 60);
    let last_auto_redraw = Date.now();
    const auto_redraw_interval = 16;
    let loop_count = 0;

    while (ctx.running && ctx.jvm && ctx.jvm.running) {
        loop_count++;

        if (sdl_has_error()) {
            sdl_draw_error_screen(ctx);
            if (g_renderer_callbacks && g_renderer_callbacks.presentFrame) {
                g_renderer_callbacks.presentFrame(ctx.framebuffer, ctx.width, ctx.height);
            }
            await new Promise(r => setTimeout(r, 16));
            continue;
        }

        if (cb.jvm_process_timers) cb.jvm_process_timers(ctx.jvm);
        if (cb.midp_process_call_serially_queue) cb.midp_process_call_serially_queue(ctx.jvm);
        if (cb.midp_check_alert_timeout) cb.midp_check_alert_timeout(ctx.jvm);

        const now = Date.now();
        if (!ctx.needs_redraw && (now - last_auto_redraw) >= auto_redraw_interval) {
            ctx.needs_redraw = true;
            last_auto_redraw = now;
        }

        if (ctx.needs_redraw) {
            ctx.needs_redraw = false;
            if (cb.midp_process_repaints) cb.midp_process_repaints(ctx.jvm);
            if (cb.midp_clear_pending_repaint) cb.midp_clear_pending_repaint();
        }

        sdl_present(ctx);

        await new Promise(r => setTimeout(r, 1));
    }
}

export function sdl_stop(ctx) {
    if (ctx) ctx.running = false;
}

// ---------------------------------------------------------------------------
// Framebuffer operations
// ---------------------------------------------------------------------------

export function sdl_get_framebuffer(ctx) {
    return ctx ? ctx.framebuffer : null;
}

export function sdl_clear(ctx, color) {
    if (!ctx || !ctx.framebuffer) return;
    ctx.framebuffer.fill(color >>> 0);
}

export function sdl_present(ctx) {
    if (!ctx || !ctx.framebuffer) return;
    if (g_renderer_callbacks && g_renderer_callbacks.presentFrame) {
        g_renderer_callbacks.presentFrame(ctx.framebuffer, ctx.width, ctx.height);
    }
}

export function sdl_update_texture(ctx) {
    sdl_present(ctx);
}

export function sdl_resize(ctx, width, height) {
    if (!ctx || width <= 0 || height <= 0) return -1;
    try {
        const newFb = new Uint32Array(width * height);
        newFb.fill(0xFF000000);
        ctx.framebuffer = newFb;
        ctx.width  = width;
        ctx.height = height;
        return 0;
    } catch (e) {
        return -1;
    }
}

export function sdl_screenshot(ctx, filename) {
    return -1;
}

// ---------------------------------------------------------------------------
// Redraw flag
// ---------------------------------------------------------------------------

export function sdl_request_redraw() {
    if (g_sdl_ctx_ptr) {
        g_sdl_ctx_ptr.needs_redraw = true;
    }
}

export function sdl_needs_redraw(ctx) {
    return ctx ? ctx.needs_redraw : false;
}

export function sdl_clear_redraw(ctx) {
    if (ctx) ctx.needs_redraw = false;
}

// ---------------------------------------------------------------------------
// Key repeat helpers
// ---------------------------------------------------------------------------

function sdl_key_to_repeat_idx(sdl_key) {
    let idx = (sdl_key | 0) + 128;
    if (idx < 0) idx = 0;
    if (idx >= SDL_KEY_REPEAT_MAX_KEYS) idx = SDL_KEY_REPEAT_MAX_KEYS - 1;
    return idx;
}

function sdl_get_time_ms() {
    return Date.now();
}

function sdl_process_key_repeat(ctx, cb) {
    if (!ctx || !ctx.jvm) return;
    const now = sdl_get_time_ms();
    for (let i = 0; i < KEY_MAP.length; i++) {
        const entry = KEY_MAP[i];
        const key   = entry.sdl_key;
        if (key === SDLK_F1 || key === SDLK_F2 || key === SDLK_ESCAPE || key === SDLK_F12) continue;
        if (!entry.midp_key && !entry.game_action) continue;
        const idx = sdl_key_to_repeat_idx(key);
        if (g_sdl_key_repeat.is_held[idx]) {
            const hold_duration = now - g_sdl_key_repeat.press_time[idx];
            const since_last    = now - g_sdl_key_repeat.last_repeat_time[idx];
            if (hold_duration >= SDL_KEY_REPEAT_DELAY_MS && since_last >= SDL_KEY_REPEAT_INTERVAL_MS) {
                g_sdl_key_repeat.last_repeat_time[idx] = now;
                const keycode = entry.midp_key ? entry.midp_key : entry.game_action;
                if (cb && cb.midp_call_keyPressed) cb.midp_call_keyPressed(ctx.jvm, keycode);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Input event injection
// ---------------------------------------------------------------------------

/**
 * Inject a synthetic input event from an external source (browser, test, etc.).
 * event shape:
 *   { type: 'keydown'|'keyup', sdl_key: number }
 *   { type: 'mousedown'|'mouseup', x: number, y: number }
 *   { type: 'mousemove', x: number, y: number }
 *   { type: 'quit' }
 *
 * @param {object} event
 * @param {object} [callbacks={}]
 */
export function inject_input_event(event, callbacks = {}) {
    const ctx = g_sdl_ctx_ptr;
    if (!ctx) return;
    const cb = callbacks;

    switch (event.type) {
        case 'quit':
            ctx.running = false;
            if (ctx.jvm) ctx.jvm.running = false;
            break;

        case 'keydown':
        case 'keyup': {
            const pressed  = (event.type === 'keydown');
            const sdl_key  = event.sdl_key | 0;
            const repeat_idx = sdl_key_to_repeat_idx(sdl_key);

            if (pressed) {
                const now_ts = sdl_get_time_ms();
                g_sdl_key_repeat.is_held[repeat_idx]          = 1;
                g_sdl_key_repeat.press_time[repeat_idx]       = now_ts;
                g_sdl_key_repeat.last_repeat_time[repeat_idx] = now_ts;
            } else {
                g_sdl_key_repeat.is_held[repeat_idx] = 0;
            }

            let midp_key    = 0;
            let game_action = 0;
            for (let i = 0; i < KEY_MAP.length; i++) {
                if (KEY_MAP[i].sdl_key === sdl_key) {
                    midp_key    = KEY_MAP[i].midp_key;
                    game_action = KEY_MAP[i].game_action;
                    if (game_action > 0 && game_action < 256) {
                        ctx.input.key_states[game_action] = pressed;
                    }
                    break;
                }
            }

            if (pressed && ctx.jvm && (sdl_key === SDLK_F1 || sdl_key === SDLK_F2)) {
                const soft_button  = (sdl_key === SDLK_F1) ? 0 : 1;
                const soft_keycode = (sdl_key === SDLK_F1) ? -6 : -7;
                if (cb.midp_handle_soft_button) {
                    if (!cb.midp_handle_soft_button(ctx.jvm, soft_button)) {
                        if (cb.midp_call_keyPressed) cb.midp_call_keyPressed(ctx.jvm, soft_keycode);
                    }
                }
                break;
            }

            if (ctx.jvm && (midp_key || game_action)) {
                if (pressed && game_action && cb.midp_is_command_menu_open && cb.midp_is_command_menu_open()) {
                    if (game_action === GAME_UP) {
                        if (cb.midp_handle_menu_navigation) cb.midp_handle_menu_navigation(-1);
                        break;
                    } else if (game_action === GAME_DOWN) {
                        if (cb.midp_handle_menu_navigation) cb.midp_handle_menu_navigation(1);
                        break;
                    } else if (game_action === GAME_FIRE) {
                        if (cb.midp_handle_soft_button) cb.midp_handle_soft_button(ctx.jvm, 1);
                        break;
                    }
                }

                const keycode = midp_key ? midp_key : game_action;
                if (pressed) {
                    if (cb.midp_call_keyPressed) cb.midp_call_keyPressed(ctx.jvm, keycode);
                } else {
                    if (cb.midp_call_keyReleased) cb.midp_call_keyReleased(ctx.jvm, keycode);
                }
            }

            if (sdl_key === SDLK_ESCAPE && pressed) {
                ctx.running = false;
                if (ctx.jvm) ctx.jvm.running = false;
            }

            if (sdl_key === SDLK_F12 && pressed) {
                process.stderr.write('[SDL] F12 pressed - debug toggle\n');
            }
            break;
        }

        case 'mousedown':
        case 'mouseup': {
            const pressed = (event.type === 'mousedown');
            const x = Math.floor((event.x || 0) / ctx.scale);
            const y = Math.floor((event.y || 0) / ctx.scale);
            const prev_pressed = ctx.input.pointer_pressed;
            ctx.input.pointer_x       = x;
            ctx.input.pointer_y       = y;
            ctx.input.pointer_pressed = pressed;
            if (ctx.jvm) {
                if (pressed && !prev_pressed) {
                    if (cb.midp_call_pointerPressed) cb.midp_call_pointerPressed(ctx.jvm, x, y);
                } else if (!pressed && prev_pressed) {
                    if (cb.midp_call_pointerReleased) cb.midp_call_pointerReleased(ctx.jvm, x, y);
                }
            }
            break;
        }

        case 'mousemove': {
            const x = Math.floor((event.x || 0) / ctx.scale);
            const y = Math.floor((event.y || 0) / ctx.scale);
            const prev_x = ctx.input.pointer_x;
            const prev_y = ctx.input.pointer_y;
            ctx.input.pointer_x = x;
            ctx.input.pointer_y = y;
            if (ctx.jvm && ctx.input.pointer_pressed && (x !== prev_x || y !== prev_y)) {
                if (cb.midp_call_pointerDragged) cb.midp_call_pointerDragged(ctx.jvm, x, y);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Event processing stubs (SDL events are injected via inject_input_event)
// ---------------------------------------------------------------------------

export function sdl_process_events(ctx) { /* no-op — use inject_input_event */ }

export function sdl_process_events_minimal(callbacks) {
    const ctx = g_sdl_ctx_ptr;
    if (!ctx || !ctx.jvm) return;
    if (callbacks) sdl_process_key_repeat(ctx, callbacks);
}

// ---------------------------------------------------------------------------
// Key state queries
// ---------------------------------------------------------------------------

export function sdl_key_to_midp(sdl_key) {
    for (let i = 0; i < KEY_MAP.length; i++) {
        if (KEY_MAP[i].sdl_key === sdl_key) return KEY_MAP[i].midp_key;
    }
    return sdl_key;
}

export function sdl_key_to_game_action(sdl_key) {
    for (let i = 0; i < KEY_MAP.length; i++) {
        if (KEY_MAP[i].sdl_key === sdl_key) return KEY_MAP[i].game_action;
    }
    return 0;
}

export function sdl_key_pressed(ctx, keycode) {
    if (!ctx) return false;
    switch (keycode) {
        case -1: case GAME_UP:    return ctx.input.key_states[GAME_UP]    || false;
        case -2: case GAME_DOWN:  return ctx.input.key_states[GAME_DOWN]  || false;
        case -3: case GAME_LEFT:  return ctx.input.key_states[GAME_LEFT]  || false;
        case -4: case GAME_RIGHT: return ctx.input.key_states[GAME_RIGHT] || false;
        case -5: case GAME_FIRE:  return ctx.input.key_states[GAME_FIRE]  || false;
        case -6: case GAME_A:     return ctx.input.key_states[GAME_A]     || false;
        case -7: case GAME_B:     return ctx.input.key_states[GAME_B]     || false;
        case -8: case GAME_C:     return ctx.input.key_states[GAME_C]     || false;
        case -9: case GAME_D:     return ctx.input.key_states[GAME_D]     || false;
        default: {
            if (keycode >= 48 && keycode <= 57) {
                const idx = SDLK_0 + (keycode - 48);
                return ctx.input.key_states[idx] || false;
            }
            if (keycode >= 97 && keycode <= 122) {
                return ctx.input.key_states[keycode] || false;
            }
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------

export function sdl_get_pointer(ctx, xOut, yOut) {
    const x = ctx ? ctx.input.pointer_x : 0;
    const y = ctx ? ctx.input.pointer_y : 0;
    if (xOut != null) xOut.value = x;
    if (yOut != null) yOut.value = y;
    return [x, y];
}

export function sdl_pointer_pressed(ctx) {
    return ctx ? ctx.input.pointer_pressed : false;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

export function sdl_audio_init(ctx, frequency, channels, samples) {
    if (g_renderer_callbacks && g_renderer_callbacks.initAudio) {
        return g_renderer_callbacks.initAudio(frequency, channels, samples);
    }
    return 0;
}

export function sdl_audio_init_simple(sample_rate) {
    if (g_renderer_callbacks && g_renderer_callbacks.initAudio) {
        return g_renderer_callbacks.initAudio(sample_rate, 2, 2048);
    }
    return 0;
}

export function sdl_audio_queue(ctx, data, length) {
    if (g_renderer_callbacks && g_renderer_callbacks.queueAudio) {
        g_renderer_callbacks.queueAudio(data, length);
        return 0;
    }
    return -1;
}

export function sdl_audio_queued_size(ctx) {
    if (g_renderer_callbacks && g_renderer_callbacks.getQueuedAudioSize) {
        return g_renderer_callbacks.getQueuedAudioSize();
    }
    return 0;
}

export function sdl_audio_clear(ctx) {
    if (g_renderer_callbacks && g_renderer_callbacks.clearAudio) {
        g_renderer_callbacks.clearAudio();
    }
}

export function sdl_audio_shutdown() {
    if (g_renderer_callbacks && g_renderer_callbacks.shutdownAudio) {
        g_renderer_callbacks.shutdownAudio();
    }
}

export function sdl_audio_queue_samples(samples, count) {
    if (g_renderer_callbacks && g_renderer_callbacks.queueAudio) {
        g_renderer_callbacks.queueAudio(samples, count);
    }
}

export function sdl_audio_get_queued_size() {
    if (g_renderer_callbacks && g_renderer_callbacks.getQueuedAudioSize) {
        return g_renderer_callbacks.getQueuedAudioSize();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

export function sdl_get_ticks(ctx) {
    return BigInt(Date.now());
}

export function sdl_frame_end(ctx) { /* no-op */ }

export function sdl_sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

// ---------------------------------------------------------------------------
// Window operations
// ---------------------------------------------------------------------------

export function sdl_set_title(ctx, title) { /* no-op */ }

export function sdl_set_fullscreen(ctx, fullscreen) { /* no-op */ }

export function sdl_toggle_fullscreen(ctx) { /* no-op */ }

export function sdl_get_window_size(ctx, widthOut, heightOut) {
    const w = ctx ? ctx.width  : 0;
    const h = ctx ? ctx.height : 0;
    if (widthOut  != null) widthOut.value  = w;
    if (heightOut != null) heightOut.value = h;
    return [w, h];
}

// ---------------------------------------------------------------------------
// Platform integration
// ---------------------------------------------------------------------------

export function sdl_get_platform_callbacks(ctx) { return null; }

export function sdl_get_graphics(ctx) {
    if (!ctx || !ctx.framebuffer) return null;
    if (g_sdl_graphics.width !== ctx.width ||
        g_sdl_graphics.height !== ctx.height ||
        g_sdl_graphics.pixels !== ctx.framebuffer) {
        g_sdl_graphics.pixels      = ctx.framebuffer;
        g_sdl_graphics.width       = ctx.width;
        g_sdl_graphics.height      = ctx.height;
        g_sdl_graphics.clip_x      = 0;
        g_sdl_graphics.clip_y      = 0;
        g_sdl_graphics.clip_width  = ctx.width;
        g_sdl_graphics.clip_height = ctx.height;
        g_sdl_graphics.translate_x = 0;
        g_sdl_graphics.translate_y = 0;
        g_sdl_graphics.rgb_color   = 0;
        g_sdl_graphics.alpha       = 255;
        g_sdl_graphics.stroke_style = 0;
        g_sdl_graphics.font        = 0;
    }
    return g_sdl_graphics;
}

export function sdl_is_screen_graphics(gfx) {
    return gfx === g_sdl_graphics;
}

export function sdl_dump_info(ctx) {
    if (!ctx) return;
    process.stderr.write(`[SDL] Window: ${ctx.width}x${ctx.height} scale=${ctx.scale}\n`);
}

// ---------------------------------------------------------------------------
// Framebuffer PPM save
// ---------------------------------------------------------------------------

export function sdl_save_framebuffer_to_file(ctx, filename) {
    if (!ctx || !ctx.framebuffer || !filename) return;
    const header   = `P6\n${ctx.width} ${ctx.height}\n255\n`;
    const headerBuf = Buffer.from(header, 'ascii');
    const pixelBuf  = Buffer.allocUnsafe(ctx.width * ctx.height * 3);
    const fb    = ctx.framebuffer;
    const total = ctx.width * ctx.height;
    let off = 0;
    for (let i = 0; i < total; i++) {
        const pixel = fb[i] >>> 0;
        pixelBuf[off++] = (pixel >>> 16) & 0xFF;
        pixelBuf[off++] = (pixel >>>  8) & 0xFF;
        pixelBuf[off++] =  pixel         & 0xFF;
    }
    import('node:fs').then(fs => {
        try {
            fs.writeFileSync(filename, Buffer.concat([headerBuf, pixelBuf]));
        } catch (e) {
            process.stderr.write(`[SDL] Failed to save framebuffer: ${e.message}\n`);
        }
    });
}

// ---------------------------------------------------------------------------
// Error display
// ---------------------------------------------------------------------------

export function sdl_set_error_info(title, message, stack_trace) {
    g_has_error = true;
    g_error_title   = title       != null ? String(title)       : 'Error';
    g_error_message = message     != null ? String(message)     : '';
    g_error_stack   = stack_trace != null ? String(stack_trace) : '';

    process.stderr.write(`\n========================================\n`);
    process.stderr.write(`  [J2ME UNCAUGHT EXCEPTION]\n`);
    process.stderr.write(`========================================\n`);
    process.stderr.write(`  Exception: ${g_error_title}\n`);
    if (g_error_message) process.stderr.write(`  Message:   ${g_error_message}\n`);
    if (g_error_extra)   process.stderr.write(`  Details:   ${g_error_extra}\n`);
    if (g_error_stack)   process.stderr.write(`  Stack:\n${g_error_stack}`);
    process.stderr.write(`========================================\n\n`);
}

export function sdl_set_error_extra(extra) {
    g_error_extra = extra != null ? String(extra) : '';
}

export function sdl_has_error() {
    return g_has_error;
}

export function sdl_clear_error() {
    g_has_error     = false;
    g_error_title   = '';
    g_error_message = '';
    g_error_stack   = '';
    g_error_extra   = '';
}

// ---------------------------------------------------------------------------
// Error screen rendering (using embedded bitmap font)
// ---------------------------------------------------------------------------

function utf8_decode_js(str, indexObj) {
    const len = str.length;
    if (indexObj.i >= len) return -1;
    const code = str.codePointAt(indexObj.i);
    if (code === undefined) return -1;
    indexObj.i += code > 0xFFFF ? 2 : 1;
    return code;
}

function get_char_data_unicode_js(codepoint) {
    if (codepoint >= FONT_RANGE1_START && codepoint <= FONT_RANGE1_END) {
        return (codepoint - FONT_RANGE1_START) * FONT_HEIGHT;
    }
    if (codepoint >= FONT_RANGE2_START && codepoint <= FONT_RANGE2_END) {
        return (FONT_RANGE1_COUNT + (codepoint - FONT_RANGE2_START)) * FONT_HEIGHT;
    }
    if (codepoint >= FONT_RANGE3_START && codepoint <= FONT_RANGE3_END) {
        return (FONT_RANGE1_COUNT + FONT_RANGE2_COUNT + (codepoint - FONT_RANGE3_START)) * FONT_HEIGHT;
    }
    return 0; // space
}

function draw_error_char(fb, fb_width, fb_height, codepoint, x, y, color) {
    if (!fb || x < 0 || y < 0) return;
    if (x + FONT_WIDTH > fb_width || y + FONT_HEIGHT > fb_height) return;
    const offset = get_char_data_unicode_js(codepoint);
    for (let row = 0; row < FONT_HEIGHT; row++) {
        const row_data = BITMAP_FONT[offset + row];
        for (let col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (1 << (4 - col))) {
                const px = x + col;
                const py = y + row;
                if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                    fb[py * fb_width + px] = color >>> 0;
                }
            }
        }
    }
}

function draw_error_string(fb, fb_width, fb_height, str, x, y, color) {
    if (!fb || !str) return;
    let cur_x = x;
    let cur_y = y;
    const idx = { i: 0 };
    while (idx.i < str.length) {
        const cp = utf8_decode_js(str, idx);
        if (cp < 0) break;
        if (cp === 10) { // '\n'
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
            continue;
        }
        if (cp === 9) { // '\t'
            cur_x += (FONT_WIDTH + 1) * 4;
            continue;
        }
        if (cur_x + FONT_WIDTH > fb_width) {
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
        }
        if (cur_y + FONT_HEIGHT > fb_height) break;
        draw_error_char(fb, fb_width, fb_height, cp, cur_x, cur_y, color);
        cur_x += FONT_WIDTH + 1;
    }
}

function wrap_text(text, max_chars) {
    if (!text) return text;
    const words = text.split(' ');
    let result = '';
    let line_len = 0;
    for (let i = 0; i < words.length; i++) {
        const w = words[i];
        if (line_len + w.length > max_chars && line_len > 0) {
            result += '\n';
            line_len = 0;
        }
        if (line_len > 0) { result += ' '; line_len++; }
        result += w;
        line_len += w.length;
    }
    return result;
}

export function sdl_draw_error_screen(ctx) {
    if (!ctx || !ctx.framebuffer) return;
    const fb     = ctx.framebuffer;
    const width  = ctx.width;
    const height = ctx.height;

    const bg_color     = 0xFF800000;
    const title_color  = 0xFFFFFFFF;
    const msg_color    = 0xFFFFFF00;
    const stack_color  = 0xFFCCCCCC;
    const border_color = 0xFFFF0000;

    fb.fill(bg_color >>> 0);

    const bw = 2;
    for (let y = 0; y < bw; y++) {
        for (let x = 0; x < width; x++) {
            fb[y * width + x]              = border_color >>> 0;
            fb[(height - 1 - y) * width + x] = border_color >>> 0;
        }
    }
    for (let x = 0; x < bw; x++) {
        for (let y = 0; y < height; y++) {
            fb[y * width + x]                   = border_color >>> 0;
            fb[y * width + (width - 1 - x)]     = border_color >>> 0;
        }
    }

    const char_width = FONT_WIDTH + 1;
    const max_chars  = Math.max(10, Math.min(60, Math.floor((width - 20) / char_width)));

    const title_line = `[ERROR] ${g_error_title}`;
    draw_error_string(fb, width, height, title_line, 10, 10, title_color >>> 0);

    const sep_y = 10 + FONT_HEIGHT + 4;
    for (let x = 10; x < width - 10; x++) {
        fb[sep_y * width + x] = border_color >>> 0;
    }

    let msg_y = sep_y + 10;
    if (g_error_message) {
        const wrapped = wrap_text(g_error_message, max_chars);
        draw_error_string(fb, width, height, wrapped, 10, msg_y, msg_color >>> 0);
        msg_y += FONT_HEIGHT * 3;
    }

    if (g_error_stack) {
        draw_error_string(fb, width, height, 'Stack trace:', 10, msg_y, title_color >>> 0);
        msg_y += FONT_HEIGHT + 4;
        for (let x = 10; x < width - 10; x++) {
            fb[msg_y * width + x] = border_color >>> 0;
        }
        msg_y += 4;
        const wrapped_stack = wrap_text(g_error_stack, max_chars);
        draw_error_string(fb, width, height, wrapped_stack, 10, msg_y, stack_color >>> 0);
    }

    const inst_y = height - FONT_HEIGHT - 15;
    draw_error_string(fb, width, height, 'Press ESC to exit', 10, inst_y, stack_color >>> 0);

    ctx.needs_redraw = true;
}
