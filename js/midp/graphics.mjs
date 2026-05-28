import { stbi_load_from_memory, stbi_failure_reason } from '../utils/stb_image_impl.mjs';

// ---------------------------------------------------------------------------
// Constants (from midp.h and bitmap_font.h)
// ---------------------------------------------------------------------------

export const MIDP_DEFAULT_WIDTH  = 240;
export const MIDP_DEFAULT_HEIGHT = 320;

export const GFX_ALPHA_OPAQUE      = 255;
export const GFX_ALPHA_TRANSLUCENT = 0;
export const GFX_ALPHA_BITMASK     = 1;

export const GFX_TOP_LEFT         = 0;
export const GFX_TOP_HCENTER      = 1;
export const GFX_TOP_RIGHT        = 2;
export const GFX_VCENTER_LEFT     = 3;
export const GFX_VCENTER_HCENTER  = 4;
export const GFX_VCENTER_RIGHT    = 5;
export const GFX_BOTTOM_LEFT      = 6;
export const GFX_BOTTOM_HCENTER   = 7;
export const GFX_BOTTOM_RIGHT     = 8;
export const GFX_BASELINE_LEFT    = 9;
export const GFX_BASELINE_HCENTER = 10;
export const GFX_BASELINE_RIGHT   = 11;

export const FONT_FACE_SYSTEM       = 0;
export const FONT_FACE_MONOSPACE    = 32;
export const FONT_FACE_PROPORTIONAL = 64;

export const FONT_STYLE_PLAIN      = 0;
export const FONT_STYLE_BOLD       = 1;
export const FONT_STYLE_ITALIC     = 2;
export const FONT_STYLE_UNDERLINED = 4;

export const FONT_SIZE_SMALL  = 8;
export const FONT_SIZE_MEDIUM = 0;
export const FONT_SIZE_LARGE  = 16;

export const GAME_UP    = 1;
export const GAME_DOWN  = 6;
export const GAME_LEFT  = 2;
export const GAME_RIGHT = 5;
export const GAME_FIRE  = 8;
export const GAME_A     = 9;
export const GAME_B     = 10;
export const GAME_C     = 11;
export const GAME_D     = 12;

export const MIDP_EVENT_TYPE = Object.freeze({
  KEY_PRESSED:       0,
  KEY_RELEASED:      1,
  KEY_REPEATED:      2,
  POINTER_PRESSED:   3,
  POINTER_RELEASED:  4,
  POINTER_DRAGGED:   5,
  COMMAND:           6,
  SHOW_NOTIFY:       7,
  HIDE_NOTIFY:       8,
  SIZE_CHANGED:      9,
  REPAINT:           10,
});

// ---------------------------------------------------------------------------
// Bitmap font data (from bitmap_font.h)
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

// Each character is FONT_HEIGHT (7) bytes: one byte per row, MSB = leftmost pixel.
const BITMAP_FONT = new Uint8Array([
  // Range 1: Basic ASCII (0x20-0x7E)
  0x00,0x00,0x00,0x00,0x00,0x00,0x00, // Space (0x20)
  0x04,0x04,0x04,0x04,0x04,0x00,0x04, // ! (0x21)
  0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00, // " (0x22)
  0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A, // # (0x23)
  0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04, // $ (0x24)
  0x18,0x19,0x02,0x04,0x08,0x13,0x03, // % (0x25)
  0x08,0x14,0x14,0x08,0x15,0x12,0x0D, // & (0x26)
  0x04,0x04,0x08,0x00,0x00,0x00,0x00, // ' (0x27)
  0x02,0x04,0x08,0x08,0x08,0x04,0x02, // ( (0x28)
  0x08,0x04,0x02,0x02,0x02,0x04,0x08, // ) (0x29)
  0x04,0x15,0x0E,0x1F,0x0E,0x15,0x04, // * (0x2A)
  0x00,0x04,0x04,0x1F,0x04,0x04,0x00, // + (0x2B)
  0x00,0x00,0x00,0x00,0x00,0x04,0x08, // , (0x2C)
  0x00,0x00,0x00,0x1F,0x00,0x00,0x00, // - (0x2D)
  0x00,0x00,0x00,0x00,0x00,0x04,0x04, // . (0x2E)
  0x00,0x01,0x02,0x04,0x08,0x10,0x00, // / (0x2F)
  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, // 0 (0x30)
  0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, // 1 (0x31)
  0x0E,0x11,0x01,0x06,0x08,0x10,0x1F, // 2 (0x32)
  0x0E,0x11,0x01,0x06,0x01,0x11,0x0E, // 3 (0x33)
  0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, // 4 (0x34)
  0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E, // 5 (0x35)
  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, // 6 (0x36)
  0x1F,0x01,0x02,0x04,0x08,0x08,0x08, // 7 (0x37)
  0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E, // 8 (0x38)
  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, // 9 (0x39)
  0x00,0x04,0x04,0x00,0x04,0x04,0x00, // : (0x3A)
  0x00,0x04,0x04,0x00,0x04,0x08,0x00, // ; (0x3B)
  0x02,0x04,0x08,0x10,0x08,0x04,0x02, // < (0x3C)
  0x00,0x00,0x1F,0x00,0x1F,0x00,0x00, // = (0x3D)
  0x08,0x04,0x02,0x01,0x02,0x04,0x08, // > (0x3E)
  0x0E,0x11,0x01,0x02,0x04,0x00,0x04, // ? (0x3F)
  0x0E,0x11,0x17,0x15,0x17,0x10,0x0E, // @ (0x40)
  0x0E,0x11,0x11,0x11,0x1F,0x11,0x11, // A (0x41)
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, // B (0x42)
  0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, // C (0x43)
  0x1E,0x11,0x11,0x11,0x11,0x11,0x1E, // D (0x44)
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, // E (0x45)
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, // F (0x46)
  0x0E,0x11,0x10,0x17,0x11,0x11,0x0F, // G (0x47)
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // H (0x48)
  0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, // I (0x49)
  0x01,0x01,0x01,0x01,0x01,0x11,0x0E, // J (0x4A)
  0x11,0x12,0x14,0x18,0x14,0x12,0x11, // K (0x4B)
  0x10,0x10,0x10,0x10,0x10,0x10,0x1F, // L (0x4C)
  0x11,0x1B,0x15,0x15,0x11,0x11,0x11, // M (0x4D)
  0x11,0x19,0x15,0x13,0x11,0x11,0x11, // N (0x4E)
  0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // O (0x4F)
  0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, // P (0x50)
  0x0E,0x11,0x11,0x11,0x15,0x12,0x0D, // Q (0x51)
  0x1E,0x11,0x11,0x1E,0x14,0x12,0x11, // R (0x52)
  0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E, // S (0x53)
  0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // T (0x54)
  0x11,0x11,0x11,0x11,0x11,0x11,0x0E, // U (0x55)
  0x11,0x11,0x11,0x11,0x11,0x0A,0x04, // V (0x56)
  0x11,0x11,0x11,0x15,0x15,0x1B,0x11, // W (0x57)
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // X (0x58)
  0x11,0x11,0x0A,0x04,0x04,0x04,0x04, // Y (0x59)
  0x1F,0x01,0x02,0x04,0x08,0x10,0x1F, // Z (0x5A)
  0x0E,0x08,0x08,0x08,0x08,0x08,0x0E, // [ (0x5B)
  0x00,0x10,0x08,0x04,0x02,0x01,0x00, // \ (0x5C)
  0x0E,0x02,0x02,0x02,0x02,0x02,0x0E, // ] (0x5D)
  0x04,0x0A,0x11,0x00,0x00,0x00,0x00, // ^ (0x5E)
  0x00,0x00,0x00,0x00,0x00,0x00,0x1F, // _ (0x5F)
  0x08,0x04,0x02,0x00,0x00,0x00,0x00, // ` (0x60)
  0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F, // a (0x61)
  0x10,0x10,0x1E,0x11,0x11,0x11,0x1E, // b (0x62)
  0x00,0x00,0x0E,0x11,0x10,0x11,0x0E, // c (0x63)
  0x01,0x01,0x0F,0x11,0x11,0x11,0x0F, // d (0x64)
  0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E, // e (0x65)
  0x06,0x08,0x08,0x1E,0x08,0x08,0x08, // f (0x66)
  0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, // g (0x67)
  0x10,0x10,0x1E,0x11,0x11,0x11,0x11, // h (0x68)
  0x04,0x00,0x0C,0x04,0x04,0x04,0x0E, // i (0x69)
  0x02,0x00,0x06,0x02,0x02,0x02,0x12, // j (0x6A)
  0x10,0x10,0x12,0x14,0x18,0x14,0x12, // k (0x6B)
  0x0C,0x04,0x04,0x04,0x04,0x04,0x0E, // l (0x6C)
  0x00,0x00,0x1A,0x15,0x15,0x11,0x11, // m (0x6D)
  0x00,0x00,0x1E,0x11,0x11,0x11,0x11, // n (0x6E)
  0x00,0x00,0x0E,0x11,0x11,0x11,0x0E, // o (0x6F)
  0x00,0x00,0x1E,0x11,0x11,0x1E,0x10, // p (0x70)
  0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, // q (0x71)
  0x00,0x00,0x17,0x08,0x08,0x08,0x08, // r (0x72)
  0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E, // s (0x73)
  0x08,0x08,0x1E,0x08,0x08,0x08,0x06, // t (0x74)
  0x00,0x00,0x11,0x11,0x11,0x11,0x0F, // u (0x75)
  0x00,0x00,0x11,0x11,0x11,0x0A,0x04, // v (0x76)
  0x00,0x00,0x11,0x11,0x15,0x15,0x0A, // w (0x77)
  0x00,0x00,0x11,0x0A,0x04,0x0A,0x11, // x (0x78)
  0x00,0x00,0x11,0x11,0x11,0x0F,0x01, // y (0x79)
  0x00,0x00,0x1F,0x02,0x04,0x08,0x1F, // z (0x7A)
  0x02,0x04,0x04,0x08,0x04,0x04,0x02, // { (0x7B)
  0x04,0x04,0x04,0x04,0x04,0x04,0x04, // | (0x7C)
  0x08,0x04,0x04,0x02,0x04,0x04,0x08, // } (0x7D)
  0x00,0x00,0x08,0x15,0x02,0x00,0x00, // ~ (0x7E)

  // Range 2: Latin-1 Supplement (0xA0-0xFF)
  0x00,0x00,0x00,0x00,0x00,0x00,0x00, // NBSP (0xA0)
  0x04,0x00,0x04,0x04,0x04,0x04,0x04, // ¡ (0xA1)
  0x00,0x04,0x0E,0x14,0x10,0x0E,0x04, // ¢ (0xA2)
  0x06,0x08,0x08,0x1E,0x08,0x08,0x1C, // £ (0xA3)
  0x00,0x11,0x0E,0x0A,0x0E,0x11,0x00, // ¤ (0xA4)
  0x11,0x0A,0x04,0x1F,0x04,0x1F,0x04, // ¥ (0xA5)
  0x04,0x04,0x04,0x00,0x04,0x04,0x04, // ¦ (0xA6)
  0x0E,0x10,0x0E,0x11,0x0E,0x01,0x0E, // § (0xA7)
  0x0A,0x00,0x00,0x00,0x00,0x00,0x00, // ¨ (0xA8)
  0x0E,0x11,0x15,0x15,0x15,0x11,0x0E, // © (0xA9)
  0x06,0x01,0x07,0x00,0x0F,0x00,0x00, // ª (0xAA)
  0x00,0x00,0x0A,0x14,0x0A,0x00,0x00, // « (0xAB)
  0x00,0x00,0x1F,0x01,0x01,0x00,0x00, // ¬ (0xAC)
  0x00,0x00,0x1F,0x00,0x00,0x00,0x00, // SHY (0xAD)
  0x0E,0x11,0x17,0x15,0x13,0x11,0x0E, // ® (0xAE)
  0x1F,0x00,0x00,0x00,0x00,0x00,0x00, // ¯ (0xAF)
  0x0E,0x11,0x11,0x0E,0x00,0x00,0x00, // ° (0xB0)
  0x04,0x04,0x1F,0x04,0x04,0x00,0x1F, // ± (0xB1)
  0x0E,0x01,0x06,0x08,0x0F,0x00,0x00, // ² (0xB2)
  0x0E,0x01,0x06,0x01,0x0E,0x00,0x00, // ³ (0xB3)
  0x02,0x04,0x00,0x00,0x00,0x00,0x00, // ´ (0xB4)
  0x00,0x00,0x12,0x12,0x12,0x1C,0x10, // µ (0xB5)
  0x07,0x0D,0x15,0x15,0x04,0x04,0x04, // ¶ (0xB6)
  0x00,0x00,0x00,0x04,0x00,0x00,0x00, // · (0xB7)
  0x00,0x00,0x00,0x00,0x00,0x04,0x08, // ¸ (0xB8)
  0x04,0x0C,0x04,0x04,0x07,0x00,0x00, // ¹ (0xB9)
  0x0E,0x11,0x11,0x0E,0x00,0x0F,0x00, // º (0xBA)
  0x00,0x00,0x14,0x0A,0x14,0x00,0x00, // » (0xBB)
  0x18,0x08,0x09,0x13,0x04,0x03,0x00, // ¼ (0xBC)
  0x18,0x08,0x05,0x12,0x04,0x0B,0x00, // ½ (0xBD)
  0x0C,0x04,0x09,0x13,0x04,0x03,0x00, // ¾ (0xBE)
  0x04,0x00,0x04,0x08,0x10,0x11,0x0E, // ¿ (0xBF)
  0x08,0x04,0x0E,0x11,0x1F,0x11,0x11, // À (0xC0)
  0x02,0x04,0x0E,0x11,0x1F,0x11,0x11, // Á (0xC1)
  0x04,0x0A,0x0E,0x11,0x1F,0x11,0x11, // Â (0xC2)
  0x0D,0x12,0x0E,0x11,0x1F,0x11,0x11, // Ã (0xC3)
  0x0A,0x00,0x0E,0x11,0x1F,0x11,0x11, // Ä (0xC4)
  0x04,0x0A,0x0E,0x11,0x1F,0x11,0x11, // Å (0xC5)
  0x07,0x08,0x0E,0x0B,0x1E,0x0A,0x1B, // Æ (0xC6)
  0x0E,0x10,0x10,0x10,0x10,0x0E,0x08, // Ç (0xC7)
  0x08,0x04,0x1F,0x10,0x1E,0x10,0x1F, // È (0xC8)
  0x02,0x04,0x1F,0x10,0x1E,0x10,0x1F, // É (0xC9)
  0x04,0x0A,0x1F,0x10,0x1E,0x10,0x1F, // Ê (0xCA)
  0x0A,0x00,0x1F,0x10,0x1E,0x10,0x1F, // Ë (0xCB)
  0x08,0x04,0x0E,0x04,0x04,0x04,0x0E, // Ì (0xCC)
  0x02,0x04,0x0E,0x04,0x04,0x04,0x0E, // Í (0xCD)
  0x04,0x0A,0x0E,0x04,0x04,0x04,0x0E, // Î (0xCE)
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // Ï (0xCF)
  0x0E,0x08,0x08,0x1E,0x08,0x08,0x1C, // Ð (0xD0)
  0x0D,0x12,0x11,0x19,0x15,0x13,0x11, // Ñ (0xD1)
  0x08,0x04,0x0E,0x11,0x11,0x11,0x0E, // Ò (0xD2)
  0x02,0x04,0x0E,0x11,0x11,0x11,0x0E, // Ó (0xD3)
  0x04,0x0A,0x0E,0x11,0x11,0x11,0x0E, // Ô (0xD4)
  0x0D,0x12,0x0E,0x11,0x11,0x11,0x0E, // Õ (0xD5)
  0x0A,0x00,0x0E,0x11,0x11,0x11,0x0E, // Ö (0xD6)
  0x00,0x11,0x0A,0x04,0x0A,0x11,0x00, // × (0xD7)
  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, // Ø (0xD8)
  0x08,0x04,0x11,0x11,0x11,0x11,0x0E, // Ù (0xD9)
  0x02,0x04,0x11,0x11,0x11,0x11,0x0E, // Ú (0xDA)
  0x04,0x0A,0x11,0x11,0x11,0x11,0x0E, // Û (0xDB)
  0x0A,0x00,0x11,0x11,0x11,0x11,0x0E, // Ü (0xDC)
  0x02,0x04,0x11,0x11,0x0A,0x04,0x04, // Ý (0xDD)
  0x10,0x1E,0x11,0x11,0x1E,0x10,0x10, // Þ (0xDE)
  0x06,0x09,0x09,0x0A,0x09,0x09,0x06, // ß (0xDF)
  0x08,0x04,0x0E,0x01,0x0F,0x11,0x0F, // à (0xE0)
  0x02,0x04,0x0E,0x01,0x0F,0x11,0x0F, // á (0xE1)
  0x04,0x0A,0x0E,0x01,0x0F,0x11,0x0F, // â (0xE2)
  0x0D,0x12,0x0E,0x01,0x0F,0x11,0x0F, // ã (0xE3)
  0x0A,0x00,0x0E,0x01,0x0F,0x11,0x0F, // ä (0xE4)
  0x04,0x0A,0x0E,0x01,0x0F,0x11,0x0F, // å (0xE5)
  0x00,0x00,0x1B,0x05,0x1F,0x14,0x1B, // æ (0xE6)
  0x00,0x00,0x0E,0x10,0x10,0x0E,0x08, // ç (0xE7)
  0x08,0x04,0x0E,0x11,0x1F,0x10,0x0E, // è (0xE8)
  0x02,0x04,0x0E,0x11,0x1F,0x10,0x0E, // é (0xE9)
  0x04,0x0A,0x0E,0x11,0x1F,0x10,0x0E, // ê (0xEA)
  0x0A,0x00,0x0E,0x11,0x1F,0x10,0x0E, // ë (0xEB)
  0x08,0x04,0x0C,0x04,0x04,0x04,0x0E, // ì (0xEC)
  0x02,0x04,0x0C,0x04,0x04,0x04,0x0E, // í (0xED)
  0x04,0x0A,0x0C,0x04,0x04,0x04,0x0E, // î (0xEE)
  0x0A,0x00,0x0C,0x04,0x04,0x04,0x0E, // ï (0xEF)
  0x06,0x01,0x07,0x09,0x09,0x09,0x06, // ð (0xF0)
  0x0D,0x12,0x1E,0x11,0x11,0x11,0x11, // ñ (0xF1)
  0x08,0x04,0x0E,0x11,0x11,0x11,0x0E, // ò (0xF2)
  0x02,0x04,0x0E,0x11,0x11,0x11,0x0E, // ó (0xF3)
  0x04,0x0A,0x0E,0x11,0x11,0x11,0x0E, // ô (0xF4)
  0x0D,0x12,0x0E,0x11,0x11,0x11,0x0E, // õ (0xF5)
  0x0A,0x00,0x0E,0x11,0x11,0x11,0x0E, // ö (0xF6)
  0x00,0x04,0x00,0x1F,0x00,0x04,0x00, // ÷ (0xF7)
  0x00,0x00,0x0E,0x13,0x15,0x19,0x0E, // ø (0xF8)
  0x08,0x04,0x11,0x11,0x11,0x11,0x0F, // ù (0xF9)
  0x02,0x04,0x11,0x11,0x11,0x11,0x0F, // ú (0xFA)
  0x04,0x0A,0x11,0x11,0x11,0x11,0x0F, // û (0xFB)
  0x0A,0x00,0x11,0x11,0x11,0x11,0x0F, // ü (0xFC)
  0x02,0x04,0x11,0x11,0x11,0x0F,0x01, // ý (0xFD)
  0x10,0x1E,0x11,0x11,0x1E,0x10,0x10, // þ (0xFE)
  0x0A,0x00,0x11,0x11,0x11,0x0F,0x01, // ÿ (0xFF)

  // Range 3: Cyrillic (0x400-0x45F)
  0x08,0x04,0x1F,0x10,0x1E,0x10,0x1F, // Ѐ (0x400)
  0x0A,0x00,0x1F,0x10,0x1E,0x10,0x1F, // Ё (0x401)
  0x1E,0x08,0x08,0x08,0x08,0x0A,0x04, // Ђ (0x402)
  0x02,0x04,0x1F,0x10,0x10,0x10,0x10, // Ѓ (0x403)
  0x0F,0x10,0x10,0x1E,0x10,0x10,0x0F, // Є (0x404)
  0x0E,0x10,0x10,0x0E,0x01,0x01,0x1E, // Ѕ (0x405)
  0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, // І (0x406)
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // Ї (0x407)
  0x01,0x01,0x01,0x01,0x01,0x11,0x0E, // Ј (0x408)
  0x12,0x14,0x14,0x14,0x14,0x14,0x18, // Љ (0x409)
  0x12,0x12,0x12,0x1A,0x16,0x12,0x12, // Њ (0x40A)
  0x1E,0x08,0x08,0x08,0x08,0x0A,0x04, // Ћ (0x40B)
  0x02,0x04,0x11,0x12,0x1C,0x12,0x11, // Ќ (0x40C)
  0x08,0x04,0x11,0x11,0x11,0x11,0x0E, // Ѝ (0x40D)
  0x04,0x0A,0x11,0x11,0x11,0x13,0x0D, // Ў (0x40E)
  0x11,0x11,0x11,0x13,0x15,0x19,0x11, // Џ (0x40F)
  0x0E,0x11,0x11,0x11,0x1F,0x11,0x11, // А (0x410)
  0x1F,0x10,0x1E,0x11,0x11,0x11,0x1E, // Б (0x411)
  0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, // В (0x412)
  0x1F,0x10,0x10,0x10,0x10,0x10,0x10, // Г (0x413)
  0x07,0x09,0x09,0x09,0x09,0x09,0x1F, // Д (0x414)
  0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, // Е (0x415)
  0x15,0x15,0x0E,0x15,0x0E,0x15,0x15, // Ж (0x416)
  0x0E,0x11,0x01,0x06,0x01,0x11,0x0E, // З (0x417)
  0x11,0x13,0x15,0x19,0x11,0x11,0x11, // И (0x418)
  0x04,0x0A,0x13,0x15,0x19,0x11,0x11, // Й (0x419)
  0x11,0x12,0x14,0x18,0x14,0x12,0x11, // К (0x41A)
  0x0E,0x12,0x12,0x12,0x12,0x12,0x12, // Л (0x41B)
  0x11,0x1B,0x15,0x15,0x11,0x11,0x11, // М (0x41C)
  0x11,0x11,0x11,0x1F,0x11,0x11,0x11, // Н (0x41D)
  0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // О (0x41E)
  0x1F,0x11,0x11,0x11,0x11,0x11,0x11, // П (0x41F)
  0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, // Р (0x420)
  0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, // С (0x421)
  0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // Т (0x422)
  0x11,0x11,0x11,0x0F,0x01,0x11,0x0E, // У (0x423)
  0x04,0x0E,0x15,0x15,0x0E,0x04,0x04, // Ф (0x424)
  0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, // Х (0x425)
  0x12,0x12,0x12,0x12,0x12,0x12,0x1F, // Ц (0x426)
  0x11,0x11,0x11,0x0F,0x01,0x01,0x01, // Ч (0x427)
  0x15,0x15,0x15,0x15,0x15,0x15,0x1F, // Ш (0x428)
  0x15,0x15,0x15,0x17,0x15,0x15,0x19, // Щ (0x429)
  0x18,0x08,0x08,0x0E,0x08,0x08,0x07, // Ъ (0x42A)
  0x12,0x12,0x12,0x1A,0x12,0x12,0x1B, // Ы (0x42B)
  0x10,0x10,0x10,0x1E,0x11,0x11,0x1E, // Ь (0x42C)
  0x0E,0x11,0x01,0x0F,0x01,0x11,0x0E, // Э (0x42D)
  0x16,0x19,0x11,0x1F,0x11,0x11,0x16, // Ю (0x42E)
  0x0F,0x11,0x11,0x0F,0x05,0x09,0x11, // Я (0x42F)
  0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F, // а (0x430)
  0x00,0x00,0x1E,0x11,0x1F,0x10,0x0F, // б (0x431)
  0x00,0x00,0x1E,0x11,0x1E,0x11,0x1E, // в (0x432)
  0x00,0x00,0x1F,0x10,0x10,0x10,0x10, // г (0x433)
  0x00,0x00,0x07,0x09,0x09,0x09,0x1F, // д (0x434)
  0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E, // е (0x435)
  0x00,0x00,0x15,0x0E,0x15,0x0E,0x15, // ж (0x436)
  0x00,0x00,0x0E,0x01,0x06,0x01,0x0E, // з (0x437)
  0x00,0x00,0x11,0x13,0x15,0x19,0x11, // и (0x438)
  0x04,0x0A,0x11,0x13,0x15,0x19,0x11, // й (0x439)
  0x00,0x00,0x11,0x12,0x1C,0x12,0x11, // к (0x43A)
  0x00,0x00,0x0E,0x12,0x12,0x12,0x12, // л (0x43B)
  0x00,0x00,0x11,0x1B,0x15,0x11,0x11, // м (0x43C)
  0x00,0x00,0x11,0x11,0x1F,0x11,0x11, // н (0x43D)
  0x00,0x00,0x0E,0x11,0x11,0x11,0x0E, // о (0x43E)
  0x00,0x00,0x1F,0x11,0x11,0x11,0x11, // п (0x43F)
  0x00,0x00,0x1E,0x11,0x1E,0x10,0x10, // р (0x440)
  0x00,0x00,0x0E,0x10,0x10,0x10,0x0E, // с (0x441)
  0x00,0x00,0x1F,0x04,0x04,0x04,0x04, // т (0x442)
  0x00,0x00,0x11,0x11,0x0F,0x01,0x0E, // у (0x443)
  0x00,0x04,0x0E,0x15,0x0E,0x04,0x00, // ф (0x444)
  0x00,0x00,0x11,0x0A,0x04,0x0A,0x11, // х (0x445)
  0x00,0x00,0x12,0x12,0x12,0x12,0x1F, // ц (0x446)
  0x00,0x00,0x11,0x11,0x0F,0x01,0x01, // ч (0x447)
  0x00,0x00,0x15,0x15,0x15,0x15,0x1F, // ш (0x448)
  0x00,0x00,0x15,0x15,0x17,0x15,0x19, // щ (0x449)
  0x00,0x00,0x18,0x08,0x0E,0x08,0x07, // ъ (0x44A)
  0x00,0x00,0x12,0x12,0x1A,0x12,0x1B, // ы (0x44B)
  0x00,0x00,0x10,0x10,0x1E,0x11,0x1E, // ь (0x44C)
  0x00,0x00,0x0E,0x01,0x0F,0x01,0x0E, // э (0x44D)
  0x00,0x00,0x16,0x19,0x1F,0x11,0x16, // ю (0x44E)
  0x00,0x00,0x0F,0x11,0x0F,0x09,0x11, // я (0x44F)
  0x08,0x04,0x0E,0x11,0x1F,0x10,0x0E, // ѐ (0x450)
  0x0A,0x00,0x0E,0x11,0x1F,0x10,0x0E, // ё (0x451)
  0x10,0x1E,0x08,0x08,0x08,0x0A,0x04, // ђ (0x452)
  0x02,0x04,0x1F,0x10,0x10,0x10,0x10, // ѓ (0x453)
  0x00,0x00,0x0F,0x10,0x1E,0x10,0x0F, // є (0x454)
  0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E, // ѕ (0x455)
  0x00,0x00,0x0E,0x04,0x04,0x04,0x0E, // і (0x456)
  0x0A,0x00,0x0E,0x04,0x04,0x04,0x0E, // ї (0x457)
  0x00,0x00,0x01,0x01,0x01,0x11,0x0E, // ј (0x458)
  0x00,0x00,0x12,0x14,0x14,0x14,0x18, // љ (0x459)
  0x00,0x00,0x12,0x12,0x1A,0x16,0x12, // њ (0x45A)
  0x10,0x1E,0x08,0x08,0x08,0x0A,0x04, // ћ (0x45B)
  0x02,0x04,0x11,0x12,0x1C,0x12,0x11, // ќ (0x45C)
  0x08,0x04,0x11,0x11,0x11,0x11,0x0E, // ѝ (0x45D)
  0x04,0x0A,0x11,0x11,0x13,0x0D,0x01, // ў (0x45E)
  0x00,0x00,0x11,0x11,0x13,0x15,0x11, // џ (0x45F)
]);

// ---------------------------------------------------------------------------
// Bitmap font lookup helpers
// ---------------------------------------------------------------------------

function getCharDataUnicode(codepoint) {
  if (codepoint >= FONT_RANGE1_START && codepoint <= FONT_RANGE1_END) {
    return BITMAP_FONT.subarray((codepoint - FONT_RANGE1_START) * FONT_HEIGHT);
  }
  if (codepoint >= FONT_RANGE2_START && codepoint <= FONT_RANGE2_END) {
    const idx = FONT_RANGE1_COUNT + (codepoint - FONT_RANGE2_START);
    return BITMAP_FONT.subarray(idx * FONT_HEIGHT);
  }
  if (codepoint >= FONT_RANGE3_START && codepoint <= FONT_RANGE3_END) {
    const idx = FONT_RANGE1_COUNT + FONT_RANGE2_COUNT + (codepoint - FONT_RANGE3_START);
    return BITMAP_FONT.subarray(idx * FONT_HEIGHT);
  }
  return BITMAP_FONT.subarray(0); // space glyph fallback
}

// Decode a single UTF-8 codepoint from a string, advancing the index.
// Returns { codepoint, newIndex } or null on error.
function utf8DecodeAt(str, index) {
  if (index >= str.length) return null;
  const c0 = str.charCodeAt(index);
  if (c0 < 0x80) return { codepoint: c0, newIndex: index + 1 };
  if ((c0 & 0xE0) === 0xC0) {
    if (index + 1 >= str.length) return null;
    const c1 = str.charCodeAt(index + 1);
    if ((c1 & 0xC0) !== 0x80) return null;
    return { codepoint: ((c0 & 0x1F) << 6) | (c1 & 0x3F), newIndex: index + 2 };
  }
  if ((c0 & 0xF0) === 0xE0) {
    if (index + 2 >= str.length) return null;
    const c1 = str.charCodeAt(index + 1);
    const c2 = str.charCodeAt(index + 2);
    if ((c1 & 0xC0) !== 0x80 || (c2 & 0xC0) !== 0x80) return null;
    return { codepoint: ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F), newIndex: index + 3 };
  }
  if ((c0 & 0xF8) === 0xF0) {
    if (index + 3 >= str.length) return null;
    const c1 = str.charCodeAt(index + 1);
    const c2 = str.charCodeAt(index + 2);
    const c3 = str.charCodeAt(index + 3);
    if ((c1 & 0xC0) !== 0x80 || (c2 & 0xC0) !== 0x80 || (c3 & 0xC0) !== 0x80) return null;
    return { codepoint: ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F), newIndex: index + 4 };
  }
  return null;
}

// Count Unicode codepoints (not bytes) in a JS string
function utf8Strlen(str) {
  if (!str) return 0;
  let count = 0;
  let i = 0;
  while (i < str.length) {
    const r = utf8DecodeAt(str, i);
    if (!r) break;
    i = r.newIndex;
    count++;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Module-level mutable state
// ---------------------------------------------------------------------------

let current_gfx = null;

let g_midp_screen_width  = MIDP_DEFAULT_WIDTH;
let g_midp_screen_height = MIDP_DEFAULT_HEIGHT;

// ---------------------------------------------------------------------------
// Headless text capture (mirrors J2ME_HEADLESS conditional block)
// ---------------------------------------------------------------------------

const g_headless_text_log = [];
let g_headless_text_frame       = 0;
let g_headless_last_logged_frame = -1;
let g_headless_text_activity    = 0;

export function headless_check_text_activity() {
  const v = g_headless_text_activity;
  g_headless_text_activity = 0;
  return v;
}

function headless_log_text(text, x, y, color) {
  if (!text || text.length === 0) return;
  let printable = 0;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 0x20) printable++;
  }
  if (printable < 2) return;

  g_headless_text_activity = 1;

  const hex = (color & 0xFFFFFF).toString(16).toUpperCase().padStart(6, '0');
  process.stdout.write(`[CANVAS TEXT] (${x},${y}) color=0x${hex}: ${text}\n`);

  if (g_headless_text_log.length < 2048) {
    g_headless_text_log.push({ text, x, y, color: color & 0xFFFFFF });
  }
}

export function headless_print_captured_text() {
  if (g_headless_text_log.length === 0 || g_headless_last_logged_frame === g_headless_text_frame) return;

  const seen = new Set();
  process.stdout.write(`\n=== Screen text (frame ${g_headless_text_frame}) ===\n`);

  for (const entry of g_headless_text_log) {
    if (!seen.has(entry.text)) {
      const r = (entry.color >> 16) & 0xFF;
      const g = (entry.color >> 8)  & 0xFF;
      const b =  entry.color        & 0xFF;
      process.stdout.write(`  [TEXT] (${entry.x},${entry.y}) rgb(${r},${g},${b}): ${entry.text}\n`);
      seen.add(entry.text);
    }
  }

  g_headless_last_logged_frame = g_headless_text_frame;
  process.stdout.write(`=== End screen text ===\n\n`);
}

export function headless_reset_text_capture() {
  g_headless_text_log.length = 0;
  g_headless_text_frame++;
}

// ---------------------------------------------------------------------------
// Screen dimensions
// ---------------------------------------------------------------------------

export function midp_set_screen_dimensions(width, height) {
  g_midp_screen_width  = width;
  g_midp_screen_height = height;
}

// ---------------------------------------------------------------------------
// MidpGraphics factory / operations
// ---------------------------------------------------------------------------

export function midp_graphics_create(pixels, width, height) {
  return {
    pixels,
    width,
    height,
    clip_x: 0,
    clip_y: 0,
    clip_width: width,
    clip_height: height,
    translate_x: 0,
    translate_y: 0,
    rgb_color: 0x000000,
    alpha: 255,
    stroke_style: 0,
    font: 0,
  };
}

export function midp_graphics_init(gfx, pixels, width, height) {
  gfx.pixels      = pixels;
  gfx.width       = width;
  gfx.height      = height;
  gfx.clip_x      = 0;
  gfx.clip_y      = 0;
  gfx.clip_width  = width;
  gfx.clip_height = height;
  gfx.translate_x = 0;
  gfx.translate_y = 0;
  gfx.rgb_color   = 0x000000;
  gfx.alpha       = 255;
  gfx.stroke_style = 0;
  gfx.font        = 0;
}

export function midp_graphics_set_clip(gfx, x, y, width, height) {
  gfx.clip_x      = x;
  gfx.clip_y      = y;
  gfx.clip_width  = width;
  gfx.clip_height = height;
}

export function midp_graphics_clip_rect(gfx, x, y, width, height) {
  const x1 = Math.max(gfx.clip_x, x);
  const y1 = Math.max(gfx.clip_y, y);
  const x2 = Math.min(gfx.clip_x + gfx.clip_width,  x + width);
  const y2 = Math.min(gfx.clip_y + gfx.clip_height, y + height);
  gfx.clip_x      = x1;
  gfx.clip_y      = y1;
  gfx.clip_width  = Math.max(0, x2 - x1);
  gfx.clip_height = Math.max(0, y2 - y1);
}

export function midp_graphics_translate(gfx, x, y) {
  gfx.translate_x += x;
  gfx.translate_y += y;
}

export function midp_graphics_set_color(gfx, rgb, alpha) {
  gfx.rgb_color = rgb & 0xFFFFFF;
  gfx.alpha     = alpha;
}

function setPixel(gfx, x, y) {
  x += gfx.translate_x;
  y += gfx.translate_y;
  if (x < gfx.clip_x || x >= gfx.clip_x + gfx.clip_width ||
      y < gfx.clip_y || y >= gfx.clip_y + gfx.clip_height) return;
  if (x < 0 || x >= gfx.width || y < 0 || y >= gfx.height) return;
  gfx.pixels[y * gfx.width + x] = ((gfx.alpha << 24) | gfx.rgb_color) >>> 0;
}

function drawCharUnicode(gfx, codepoint, x, y) {
  const charData = getCharDataUnicode(codepoint);
  for (let row = 0; row < FONT_HEIGHT; row++) {
    const rowData = charData[row];
    for (let col = 0; col < FONT_WIDTH; col++) {
      if (rowData & (1 << (FONT_WIDTH - 1 - col))) {
        setPixel(gfx, x + col, y + row);
      }
    }
  }
}

export function midp_graphics_draw_line(gfx, x1, y1, x2, y2) {
  x1 += gfx.translate_x;
  y1 += gfx.translate_y;
  x2 += gfx.translate_x;
  y2 += gfx.translate_y;

  const clip_x1 = gfx.clip_x;
  const clip_y1 = gfx.clip_y;
  const clip_x2 = gfx.clip_x + gfx.clip_width;
  const clip_y2 = gfx.clip_y + gfx.clip_height;
  const scr_w   = gfx.width;
  const scr_h   = gfx.height;
  const color   = ((gfx.alpha << 24) | gfx.rgb_color) >>> 0;
  const pixels  = gfx.pixels;

  const dx = Math.abs(x2 - x1);
  const dy = Math.abs(y2 - y1);
  const sx = x1 < x2 ? 1 : -1;
  const sy = y1 < y2 ? 1 : -1;
  let err = dx - dy;

  // MIDP spec: line does not include the endpoint (x2, y2)
  while (true) {
    if (x1 === x2 && y1 === y2) break;

    if (x1 >= clip_x1 && x1 < clip_x2 && y1 >= clip_y1 && y1 < clip_y2 &&
        x1 >= 0 && x1 < scr_w && y1 >= 0 && y1 < scr_h) {
      pixels[y1 * scr_w + x1] = color;
    }

    const e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x1 += sx; }
    if (e2 < dx)  { err += dx; y1 += sy; }
  }
}

export function midp_graphics_fill_rect(gfx, x, y, w, h) {
  x += gfx.translate_x;
  y += gfx.translate_y;

  let x1 = Math.max(x, gfx.clip_x);
  let y1 = Math.max(y, gfx.clip_y);
  let x2 = Math.min(x + w, gfx.clip_x + gfx.clip_width);
  let y2 = Math.min(y + h, gfx.clip_y + gfx.clip_height);

  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > gfx.width)  x2 = gfx.width;
  if (y2 > gfx.height) y2 = gfx.height;

  const rowWidth = x2 - x1;
  if (rowWidth <= 0 || y2 <= y1) return;

  const color = ((gfx.alpha << 24) | gfx.rgb_color) >>> 0;

  if (gfx.alpha === 255) {
    for (let py = y1; py < y2; py++) {
      const base = py * gfx.width + x1;
      gfx.pixels.fill(color, base, base + rowWidth);
    }
  } else {
    const alpha    = gfx.alpha;
    const invAlpha = 255 - alpha;
    const sr = (gfx.rgb_color >> 16) & 0xFF;
    const sg = (gfx.rgb_color >>  8) & 0xFF;
    const sb =  gfx.rgb_color        & 0xFF;
    for (let py = y1; py < y2; py++) {
      const base = py * gfx.width;
      for (let px = x1; px < x2; px++) {
        const dst = gfx.pixels[base + px];
        const r = ((sr * alpha + ((dst >> 16) & 0xFF) * invAlpha + 128) >> 8) & 0xFF;
        const g = ((sg * alpha + ((dst >>  8) & 0xFF) * invAlpha + 128) >> 8) & 0xFF;
        const b = ((sb * alpha +  (dst        & 0xFF) * invAlpha + 128) >> 8) & 0xFF;
        gfx.pixels[base + px] = ((alpha << 24) | (r << 16) | (g << 8) | b) >>> 0;
      }
    }
  }
}

export function midp_graphics_draw_rect(gfx, x, y, w, h) {
  midp_graphics_draw_line(gfx, x,         y,         x + w - 1, y);
  midp_graphics_draw_line(gfx, x,         y + h - 1, x + w - 1, y + h - 1);
  midp_graphics_draw_line(gfx, x,         y,         x,         y + h - 1);
  midp_graphics_draw_line(gfx, x + w - 1, y,         x + w - 1, y + h - 1);
}

export function midp_graphics_draw_arc(gfx, x, y, w, h, start_angle, arc_angle) {
  x += gfx.translate_x;
  y += gfx.translate_y;

  if (w < 0 || h < 0) return;

  const cx = x + w / 2;
  const cy = y + h / 2;
  const rx = w / 2;
  const ry = h / 2;

  // MIDP uses clockwise angles; negate to convert to standard math coordinates
  const start_rad = -start_angle * Math.PI / 180;
  const end_rad   = -(start_angle + arc_angle) * Math.PI / 180;

  let steps = Math.abs(arc_angle) * 2;
  if (steps < 4)   steps = 4;
  if (steps > 720) steps = 720;

  const step = (end_rad - start_rad) / steps;

  const clip_x1 = gfx.clip_x;
  const clip_y1 = gfx.clip_y;
  const clip_x2 = gfx.clip_x + gfx.clip_width;
  const clip_y2 = gfx.clip_y + gfx.clip_height;
  const scr_w   = gfx.width;
  const scr_h   = gfx.height;
  const color   = ((gfx.alpha << 24) | gfx.rgb_color) >>> 0;

  let prev_px = -1, prev_py = -1;
  for (let i = 0; i <= steps; i++) {
    const angle = start_rad + i * step;
    const px = (cx + rx * Math.cos(angle)) | 0;
    const py = (cy + ry * Math.sin(angle)) | 0;
    if (px !== prev_px || py !== prev_py) {
      if (px >= clip_x1 && px < clip_x2 && py >= clip_y1 && py < clip_y2 &&
          px >= 0 && px < scr_w && py >= 0 && py < scr_h) {
        gfx.pixels[py * scr_w + px] = color;
      }
      prev_px = px;
      prev_py = py;
    }
  }
}

export function midp_graphics_fill_arc(gfx, x, y, w, h, start_angle, arc_angle) {
  x += gfx.translate_x;
  y += gfx.translate_y;

  if (w <= 0 || h <= 0) return;

  const cx = x + w / 2;
  const cy = y + h / 2;
  const rx = w / 2;
  const ry = h / 2;

  let start_rad = -start_angle * Math.PI / 180;
  let end_rad   = -(start_angle + arc_angle) * Math.PI / 180;

  const TWO_PI = 2 * Math.PI;
  start_rad = ((start_rad % TWO_PI) + TWO_PI) % TWO_PI;
  end_rad   = ((end_rad   % TWO_PI) + TWO_PI) % TWO_PI;

  let arc_start, arc_end;
  if (arc_angle > 0) {
    arc_start = end_rad;
    arc_end   = start_rad;
  } else {
    arc_start = start_rad;
    arc_end   = end_rad;
  }

  const full_circle = Math.abs(arc_angle) >= 360;

  const clip_x1 = gfx.clip_x;
  const clip_y1 = gfx.clip_y;
  const clip_x2 = gfx.clip_x + gfx.clip_width;
  const clip_y2 = gfx.clip_y + gfx.clip_height;
  const scr_w   = gfx.width;
  const scr_h   = gfx.height;
  const color   = ((gfx.alpha << 24) | gfx.rgb_color) >>> 0;

  if (full_circle) {
    let bx1 = Math.max(x, clip_x1, 0);
    let by1 = Math.max(y, clip_y1, 0);
    let bx2 = Math.min(x + w, clip_x2, scr_w);
    let by2 = Math.min(y + h, clip_y2, scr_h);

    for (let py = by1; py < by2; py++) {
      const ndy  = (py - cy) / ry;
      const ndy2 = ndy * ndy;
      if (ndy2 > 1.0) continue;
      const ndx_max = Math.sqrt(1.0 - ndy2);
      let px_start = Math.max(bx1, (cx - ndx_max * rx) | 0);
      let px_end   = Math.min(bx2 - 1, (cx + ndx_max * rx) | 0);
      const base = py * scr_w;
      for (let px = px_start; px <= px_end; px++) {
        gfx.pixels[base + px] = color;
      }
    }
    return;
  }

  // Cross-product arc sector test avoids per-pixel atan2
  const sin_start = Math.sin(arc_start);
  const cos_start = Math.cos(arc_start);
  const sin_end   = Math.sin(arc_end);
  const cos_end   = Math.cos(arc_end);
  const cross_AB  = cos_start * sin_end - sin_start * cos_end;

  for (let py = y; py < y + h && py < scr_h; py++) {
    if (py < 0 || py < clip_y1 || py >= clip_y2) continue;
    for (let px = x; px < x + w && px < scr_w; px++) {
      if (px < 0 || px < clip_x1 || px >= clip_x2) continue;
      if (rx <= 0 || ry <= 0) continue;

      const dx = (px - cx) / rx;
      const dy = (py - cy) / ry;

      if (dx * dx + dy * dy > 1.0) continue;

      const cross_ap = cos_start * dy - sin_start * dx;
      const cross_pb = dx * sin_end   - dy * cos_end;

      const in_arc = cross_AB >= 0
        ? (cross_ap >= 0 && cross_pb >= 0)
        : (cross_ap >= 0 || cross_pb >= 0);

      if (in_arc) {
        gfx.pixels[py * scr_w + px] = color;
      }
    }
  }
}

export function midp_graphics_draw_round_rect(gfx, x, y, w, h, arc_w, arc_h) {
  midp_graphics_draw_line(gfx, x + arc_w/2|0, y,         x + w - (arc_w/2|0), y);
  midp_graphics_draw_line(gfx, x + arc_w/2|0, y + h - 1, x + w - (arc_w/2|0), y + h - 1);
  midp_graphics_draw_line(gfx, x,         y + arc_h/2|0, x,         y + h - (arc_h/2|0));
  midp_graphics_draw_line(gfx, x + w - 1, y + arc_h/2|0, x + w - 1, y + h - (arc_h/2|0));

  midp_graphics_draw_arc(gfx, x,             y,             arc_w, arc_h,  90, 90);
  midp_graphics_draw_arc(gfx, x + w - arc_w, y,             arc_w, arc_h,   0, 90);
  midp_graphics_draw_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
  midp_graphics_draw_arc(gfx, x,             y + h - arc_h, arc_w, arc_h, 180, 90);
}

export function midp_graphics_fill_round_rect(gfx, x, y, w, h, arc_w, arc_h) {
  midp_graphics_fill_rect(gfx, x + (arc_w/2|0), y,             w - arc_w,          h);
  midp_graphics_fill_rect(gfx, x,               y + (arc_h/2|0), arc_w/2|0,         h - arc_h);
  midp_graphics_fill_rect(gfx, x + w - (arc_w/2|0), y + (arc_h/2|0), arc_w/2|0,    h - arc_h);

  midp_graphics_fill_arc(gfx, x,             y,             arc_w, arc_h,  90, 90);
  midp_graphics_fill_arc(gfx, x + w - arc_w, y,             arc_w, arc_h,   0, 90);
  midp_graphics_fill_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
  midp_graphics_fill_arc(gfx, x,             y + h - arc_h, arc_w, arc_h, 180, 90);
}

export function midp_graphics_draw_string(gfx, str, x, y, anchor) {
  if (!str || !gfx) return;

  headless_log_text(str, x, y, gfx.rgb_color);

  if (str.length === 0) return;

  const MAX_CHARS = 4096;
  let char_count = utf8Strlen(str);
  if (char_count === 0) return;
  if (char_count > MAX_CHARS) char_count = MAX_CHARS;

  const text_width  = char_count * (FONT_WIDTH + 1) - 1;
  const text_height = FONT_HEIGHT;

  let draw_x = x + gfx.translate_x;
  let draw_y = y + gfx.translate_y;

  // MIDP2 anchor: HCENTER=1, VCENTER=2, LEFT=4, RIGHT=8, TOP=16, BOTTOM=32, BASELINE=64
  if      (anchor & 0x01) draw_x -= text_width / 2;  // HCENTER
  else if (anchor & 0x08) draw_x -= text_width;       // RIGHT

  if      (anchor & 0x02) draw_y -= text_height / 2;     // VCENTER
  else if (anchor & 0x40) draw_y -= FONT_HEIGHT - 2;      // BASELINE
  else if (anchor & 0x20) draw_y -= text_height;          // BOTTOM

  let cur_x = draw_x;
  let i = 0;
  let chars_drawn = 0;
  while (i < str.length && chars_drawn < char_count) {
    const r = utf8DecodeAt(str, i);
    if (!r) break;
    drawCharUnicode(gfx, r.codepoint, cur_x, draw_y);
    cur_x += FONT_WIDTH + 1;
    i = r.newIndex;
    chars_drawn++;
  }
}

export function midp_graphics_draw_chars(gfx, chars, offset, length, x, y, anchor) {
  if (!chars || !gfx || length <= 0) return;
  // chars is an array of Unicode codepoints (jchar[] = uint16)
  const str = chars.slice(offset, offset + length).map(c => String.fromCodePoint(c)).join('');
  midp_graphics_draw_string(gfx, str, x, y, anchor);
}

export function midp_graphics_draw_image(gfx, img, x, y, anchor) {
  x += gfx.translate_x;
  y += gfx.translate_y;

  if (!img || !img.pixels) return;

  if      (anchor & 0x01) x -= img.width  / 2;  // HCENTER
  else if (anchor & 0x08) x -= img.width;         // RIGHT

  if      (anchor & 0x02) y -= img.height / 2;  // VCENTER
  else if (anchor & 0x20) y -= img.height;        // BOTTOM

  let dst_x1 = Math.max(x, gfx.clip_x);
  let dst_y1 = Math.max(y, gfx.clip_y);
  let dst_x2 = Math.min(x + img.width,  gfx.clip_x + gfx.clip_width);
  let dst_y2 = Math.min(y + img.height, gfx.clip_y + gfx.clip_height);

  if (dst_x1 < 0) dst_x1 = 0;
  if (dst_y1 < 0) dst_y1 = 0;
  if (dst_x2 > gfx.width)  dst_x2 = gfx.width;
  if (dst_y2 > gfx.height) dst_y2 = gfx.height;

  const copy_w = dst_x2 - dst_x1;
  const copy_h = dst_y2 - dst_y1;
  if (copy_w <= 0 || copy_h <= 0) return;

  const src_x_off = dst_x1 - x;
  const src_y_off = dst_y1 - y;

  // Opacity heuristic: sample corners to avoid per-pixel alpha check on opaque images
  let likely_opaque = !img.alpha;
  if (!likely_opaque) {
    const p1 = img.pixels[src_y_off * img.width + src_x_off];
    if ((p1 >>> 24) === 0xFF) {
      const p2 = img.pixels[(src_y_off + copy_h - 1) * img.width + (src_x_off + copy_w - 1)];
      if ((p2 >>> 24) === 0xFF) {
        const p3 = img.pixels[src_y_off * img.width + (src_x_off + copy_w - 1)];
        const p4 = img.pixels[(src_y_off + copy_h - 1) * img.width + src_x_off];
        if ((p3 >>> 24) === 0xFF && (p4 >>> 24) === 0xFF) likely_opaque = true;
      }
    }
  }

  if (likely_opaque) {
    for (let py = 0; py < copy_h; py++) {
      const src_base = (src_y_off + py) * img.width + src_x_off;
      const dst_base = (dst_y1 + py) * gfx.width + dst_x1;
      gfx.pixels.set(img.pixels.subarray(src_base, src_base + copy_w), dst_base);
    }
  } else {
    for (let py = 0; py < copy_h; py++) {
      const src_base = (src_y_off + py) * img.width + src_x_off;
      const dst_base = (dst_y1 + py) * gfx.width + dst_x1;
      for (let px = 0; px < copy_w; px++) {
        const src_color = img.pixels[src_base + px];
        const alpha = (src_color >>> 24) & 0xFF;
        if (alpha === 255) {
          gfx.pixels[dst_base + px] = src_color;
        } else if (alpha > 0) {
          const dst_color = gfx.pixels[dst_base + px];
          const inv_alpha = 255 - alpha;
          const r = (((src_color >> 16) & 0xFF) * alpha + ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
          const g = (((src_color >>  8) & 0xFF) * alpha + ((dst_color >>  8) & 0xFF) * inv_alpha + 128) >> 8;
          const b = (( src_color        & 0xFF) * alpha + ( dst_color        & 0xFF) * inv_alpha + 128) >> 8;
          gfx.pixels[dst_base + px] = (0xFF000000 | (r << 16) | (g << 8) | b) >>> 0;
        }
      }
    }
  }
}

export function midp_graphics_draw_region(gfx, img, x_src, y_src, w, h,
                                          transform, x_dest, y_dest, anchor) {
  x_dest += gfx.translate_x;
  y_dest += gfx.translate_y;

  if (!img || !img.pixels) return;
  if (w <= 0 || h <= 0) return;
  if (x_src < 0 || y_src < 0 || x_src + w > img.width || y_src + h > img.height) return;

  let out_w = w;
  let out_h = h;

  // Rotations 90/270 swap output dimensions
  if (transform === 5 || transform === 6 || transform === 4 || transform === 7) {
    out_w = h;
    out_h = w;
  }

  if      (anchor & 0x01) x_dest -= out_w / 2;  // HCENTER
  else if (anchor & 0x08) x_dest -= out_w;        // RIGHT

  if      (anchor & 0x02) y_dest -= out_h / 2;  // VCENTER
  else if (anchor & 0x20) y_dest -= out_h;        // BOTTOM

  // Fast path for TRANS_NONE
  if (transform === 0) {
    let dx1 = Math.max(x_dest, gfx.clip_x, 0);
    let dy1 = Math.max(y_dest, gfx.clip_y, 0);
    let dx2 = Math.min(x_dest + w, gfx.clip_x + gfx.clip_width, gfx.width);
    let dy2 = Math.min(y_dest + h, gfx.clip_y + gfx.clip_height, gfx.height);

    const cw = dx2 - dx1;
    const ch = dy2 - dy1;
    if (cw <= 0 || ch <= 0) return;

    const sx_off = dx1 - x_dest;
    const sy_off = dy1 - y_dest;

    let opaque = !img.alpha;
    if (!opaque) {
      const p = img.pixels[(y_src + sy_off) * img.width + (x_src + sx_off)];
      if ((p >>> 24) === 0xFF) {
        const p2 = img.pixels[(y_src + sy_off + ch - 1) * img.width + (x_src + sx_off + cw - 1)];
        if ((p2 >>> 24) === 0xFF) opaque = true;
      }
    }

    if (opaque) {
      for (let py = 0; py < ch; py++) {
        const src_base = (y_src + sy_off + py) * img.width + (x_src + sx_off);
        const dst_base = (dy1 + py) * gfx.width + dx1;
        gfx.pixels.set(img.pixels.subarray(src_base, src_base + cw), dst_base);
      }
    } else {
      for (let py = 0; py < ch; py++) {
        const src_base = (y_src + sy_off + py) * img.width + (x_src + sx_off);
        const dst_base = (dy1 + py) * gfx.width + dx1;
        for (let px = 0; px < cw; px++) {
          const sc = img.pixels[src_base + px];
          const a  = (sc >>> 24) & 0xFF;
          if (a === 255) {
            gfx.pixels[dst_base + px] = sc;
          } else if (a > 0) {
            const dc = gfx.pixels[dst_base + px];
            const ia = 255 - a;
            const r = (((sc >> 16) & 0xFF) * a + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
            const g = (((sc >>  8) & 0xFF) * a + ((dc >>  8) & 0xFF) * ia + 128) >> 8;
            const b = (( sc        & 0xFF) * a + ( dc        & 0xFF) * ia + 128) >> 8;
            gfx.pixels[dst_base + px] = (0xFF000000 | (r << 16) | (g << 8) | b) >>> 0;
          }
        }
      }
    }
    return;
  }

  // General transform path
  const clip_x1 = gfx.clip_x;
  const clip_y1 = gfx.clip_y;
  const clip_x2 = gfx.clip_x + gfx.clip_width;
  const clip_y2 = gfx.clip_y + gfx.clip_height;
  const scr_w   = gfx.width;
  const scr_h   = gfx.height;

  for (let py = 0; py < h; py++) {
    for (let px = 0; px < w; px++) {
      const src_color = img.pixels[(y_src + py) * img.width + (x_src + px)];
      const alpha = (src_color >>> 24) & 0xFF;
      if (alpha === 0) continue;

      let dst_px, dst_py;
      switch (transform) {
        case 1: dst_px = px;       dst_py = h - 1 - py; break; // TRANS_MIRROR_ROT180
        case 2: dst_px = w - 1 - px; dst_py = py;       break; // TRANS_MIRROR
        case 3: dst_px = w - 1 - px; dst_py = h - 1 - py; break; // TRANS_ROT180
        case 4: dst_px = py;          dst_py = px;         break; // TRANS_MIRROR_ROT270
        case 5: dst_px = h - 1 - py;  dst_py = px;         break; // TRANS_ROT90
        case 6: dst_px = py;           dst_py = w - 1 - px; break; // TRANS_ROT270
        case 7: dst_px = h - 1 - py;   dst_py = w - 1 - px; break; // TRANS_MIRROR_ROT90
        default: dst_px = px; dst_py = py; break;
      }

      const dst_x = x_dest + dst_px;
      const dst_y = y_dest + dst_py;

      if (dst_x < clip_x1 || dst_x >= clip_x2 || dst_y < clip_y1 || dst_y >= clip_y2) continue;
      if (dst_x < 0 || dst_x >= scr_w || dst_y < 0 || dst_y >= scr_h) continue;

      if (alpha === 255) {
        gfx.pixels[dst_y * scr_w + dst_x] = src_color;
      } else {
        const dst_color = gfx.pixels[dst_y * scr_w + dst_x];
        const inv_alpha = 255 - alpha;
        const r = (((src_color >> 16) & 0xFF) * alpha + ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
        const g = (((src_color >>  8) & 0xFF) * alpha + ((dst_color >>  8) & 0xFF) * inv_alpha + 128) >> 8;
        const b = (( src_color        & 0xFF) * alpha + ( dst_color        & 0xFF) * inv_alpha + 128) >> 8;
        gfx.pixels[dst_y * scr_w + dst_x] = (0xFF000000 | (r << 16) | (g << 8) | b) >>> 0;
      }
    }
  }
}

export function midp_graphics_copy_area(gfx, x_src, y_src, w, h, x_dest, y_dest, anchor) {
  x_src  += gfx.translate_x;
  y_src  += gfx.translate_y;
  x_dest += gfx.translate_x;
  y_dest += gfx.translate_y;

  if (w <= 0 || h <= 0) return;

  if      (anchor & 0x01) x_dest -= w / 2;  // HCENTER
  else if (anchor & 0x08) x_dest -= w;        // RIGHT

  if      (anchor & 0x02) y_dest -= h / 2;  // VCENTER
  else if (anchor & 0x20) y_dest -= h;        // BOTTOM

  const sx1 = Math.max(x_src, 0);
  const sy1 = Math.max(y_src, 0);
  const sx2 = Math.min(x_src + w, gfx.width);
  const sy2 = Math.min(y_src + h, gfx.height);
  let cw = sx2 - sx1;
  let ch = sy2 - sy1;
  if (cw <= 0 || ch <= 0) return;

  let dx1 = x_dest + (sx1 - x_src);
  let dy1 = y_dest + (sy1 - y_src);

  const clip_x2 = gfx.clip_x + gfx.clip_width;
  const clip_y2 = gfx.clip_y + gfx.clip_height;

  let adj;
  adj = gfx.clip_x - dx1; if (adj > 0) { dx1 += adj; cw -= adj; }
  adj = gfx.clip_y - dy1; if (adj > 0) { dy1 += adj; ch -= adj; }
  if (dx1 + cw > clip_x2) cw = clip_x2 - dx1;
  if (dy1 + ch > clip_y2) ch = clip_y2 - dy1;
  if (dx1 + cw > gfx.width)  cw = gfx.width  - dx1;
  if (dy1 + ch > gfx.height) ch = gfx.height - dy1;
  if (cw <= 0 || ch <= 0) return;

  // Copy into a temp buffer to handle overlapping regions safely
  const temp = new Uint32Array(cw * ch);
  for (let py = 0; py < ch; py++) {
    const src_base = (sy1 + py) * gfx.width + sx1;
    temp.set(gfx.pixels.subarray(src_base, src_base + cw), py * cw);
  }
  for (let py = 0; py < ch; py++) {
    const dst_base = (dy1 + py) * gfx.width + dx1;
    gfx.pixels.set(temp.subarray(py * cw, (py + 1) * cw), dst_base);
  }
}

export function midp_graphics_get_rgb(gfx, rgb_data, offset, scanlength, x, y, w, h) {
  x += gfx.translate_x;
  y += gfx.translate_y;

  for (let py = 0; py < h; py++) {
    const src_y = y + py;
    if (src_y < 0 || src_y >= gfx.height) continue;
    for (let px = 0; px < w; px++) {
      const src_x = x + px;
      if (src_x < 0 || src_x >= gfx.width) continue;
      rgb_data[offset + py * scanlength + px] = gfx.pixels[src_y * gfx.width + src_x];
    }
  }
}

// ---------------------------------------------------------------------------
// Image constants
// ---------------------------------------------------------------------------

const MIDP_MAX_IMAGE_DIMENSION = 2048;
const MIDP_MAX_IMAGE_PIXELS    = MIDP_MAX_IMAGE_DIMENSION * MIDP_MAX_IMAGE_DIMENSION;

// ---------------------------------------------------------------------------
// MidpImage factory / operations
// ---------------------------------------------------------------------------

export function midp_image_create(width, height, mutable) {
  if (width <= 0 || height <= 0) return null;
  if (width > MIDP_MAX_IMAGE_DIMENSION || height > MIDP_MAX_IMAGE_DIMENSION) return null;
  const pixel_count = width * height;
  if (pixel_count > MIDP_MAX_IMAGE_PIXELS) return null;

  const pixels = new Uint32Array(pixel_count);

  // MIDP spec: mutable images are initially filled with white (0xFFFFFFFF)
  if (mutable) pixels.fill(0xFFFFFFFF);

  return { pixels, width, height, mutable: !!mutable, alpha: true };
}

export function midp_image_create_from_rgb(rgb, width, height, process_alpha) {
  const img = midp_image_create(width, height, false);
  if (!img) return null;

  const n = width * height;
  for (let i = 0; i < n; i++) {
    if (process_alpha) {
      img.pixels[i] = rgb[i] >>> 0;
    } else {
      img.pixels[i] = (0xFF000000 | (rgb[i] & 0xFFFFFF)) >>> 0;
    }
  }

  img.alpha = !!process_alpha;
  return img;
}

export function midp_image_destroy(img) {
  // GC handles deallocation; no-op
}

export function midp_image_get_rgb(img, rgb, offset, scanlength, x, y, width, height) {
  if (!img || !rgb) return;
  for (let py = 0; py < height; py++) {
    const src_y = y + py;
    if (src_y < 0 || src_y >= img.height) continue;
    for (let px = 0; px < width; px++) {
      const src_x = x + px;
      if (src_x < 0 || src_x >= img.width) continue;
      rgb[offset + py * scanlength + px] = img.pixels[src_y * img.width + src_x];
    }
  }
}

export function midp_image_get_graphics(img) {
  if (!img || !img.mutable) return null;
  const gfx = midp_graphics_create(img.pixels, img.width, img.height);
  return gfx;
}

// ---------------------------------------------------------------------------
// Image decoding (delegates to stb_image_impl.mjs)
// ---------------------------------------------------------------------------

function is_png(data)  { return data.length >= 8 && data[0] === 0x89 && data[1] === 0x50 && data[2] === 0x4E && data[3] === 0x47 && data[4] === 0x0D && data[5] === 0x0A && data[6] === 0x1A && data[7] === 0x0A; }
function is_jpeg(data) { return data.length >= 3 && data[0] === 0xFF && data[1] === 0xD8 && data[2] === 0xFF; }
function is_bmp(data)  { return data.length >= 2 && data[0] === 0x42 && data[1] === 0x4D; }

export function midp_image_create_from_data(data, offset, length) {
  if (!data || length < 8) return null;

  const img_data = data instanceof Uint8Array ? data.subarray(offset, offset + length - offset) : new Uint8Array(data.buffer ?? data, offset, length - offset);
  const img_len  = length - offset;

  if (img_len < 8) return null;

  const slice = data instanceof Uint8Array ? data.subarray(offset, offset + img_len) : new Uint8Array(data, offset, img_len);

  const result = stbi_load_from_memory(slice, img_len, 4);
  if (!result) {
    process.stderr.write(`[MIDP] Failed to decode image: ${stbi_failure_reason()}\n`);
    return null;
  }

  const { pixels: raw, width, height, channels } = result;

  const img = midp_image_create(width, height, false);
  if (!img) return null;

  // Convert from stb_image RGBA bytes to ARGB Uint32
  const total = width * height;
  for (let i = 0; i < total; i++) {
    const si = i * 4;
    img.pixels[i] = ((raw[si + 3] << 24) | (raw[si] << 16) | (raw[si + 1] << 8) | raw[si + 2]) >>> 0;
  }

  img.alpha = (channels === 4);
  return img;
}

// ---------------------------------------------------------------------------
// Font operations
// ---------------------------------------------------------------------------

const _default_font = {
  face:     FONT_FACE_SYSTEM,
  style:    FONT_STYLE_PLAIN,
  size:     FONT_SIZE_MEDIUM,
  height:   FONT_HEIGHT,
  baseline: FONT_HEIGHT - 2,
  native_font: null,
};

export function midp_font_get_default() {
  return _default_font;
}

export function midp_font_get(face, style, size) {
  return {
    face,
    style,
    size,
    height:   FONT_HEIGHT,
    baseline: FONT_HEIGHT - 2,
    native_font: null,
  };
}

export function midp_font_string_width(font, str) {
  if (!font || !str) return 0;
  let char_count = utf8Strlen(str);
  if (char_count > 4096) char_count = 4096;
  return char_count > 0 ? char_count * (FONT_WIDTH + 1) - 1 : 0;
}

export function midp_font_char_width(font) {
  if (!font) return 0;
  return FONT_WIDTH;
}

export function midp_font_height(font) {
  return font ? font.height : FONT_HEIGHT;
}

export function midp_font_baseline_position(font) {
  return font ? font.baseline : FONT_HEIGHT - 2;
}

// ---------------------------------------------------------------------------
// Display operations
// ---------------------------------------------------------------------------

export function midp_display_get_dimensions() {
  return { width: g_midp_screen_width, height: g_midp_screen_height };
}

export function midp_display_is_color() {
  return true;
}

export function midp_display_num_colors() {
  return 16777216;
}

export function midp_display_num_alpha_levels() {
  return 256;
}
