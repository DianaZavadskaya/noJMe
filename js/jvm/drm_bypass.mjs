/*
 * J2ME Emulator - DRM Bypass Implementation (JavaScript port)
 * Based on KEmulator analysis
 *
 * Implements protection bypass mechanisms similar to KEmulator:
 * - hideEmulation mode
 * - IMEI/IMSI spoofing
 * - Permission bypass
 * - Platform spoofing
 */

import { readFileSync } from 'fs';

// --- Constants ----------------------------------------------------------

/** Maximum number of DRM class prefixes in the bypass registry. */
export const DRM_MAX_PREFIXES = 32;

// --- Global configuration (DrmBypassConfig) -----------------------------

/**
 * Global DRM bypass configuration object.
 * Mirrors the C `DrmBypassConfig` struct and the `g_drm_bypass` global.
 *
 * Fields:
 *   hide_emulation    {boolean} - hide emulator presence (KEmulator hideEmulation)
 *   imei              {string}  - spoofed 15-digit IMEI
 *   imsi              {string}  - spoofed IMSI
 *   platform          {string}  - microedition.platform value
 *   device_model      {string}  - device model name
 *   bypass_permissions {boolean} - always return "allowed" for checkPermission
 *   bypass_vserv      {boolean} - bypass Vserv/ALW1
 *   debug_drm         {boolean} - enable DRM-specific debug logging
 */
export const g_drm_bypass = {
  hide_emulation: false,
  imei: '',
  imsi: '',
  platform: '',
  device_model: '',
  bypass_permissions: false,
  bypass_vserv: false,
  debug_drm: false,
};

// --- Module-private state -----------------------------------------------

/* Properties to hide in hideEmulation mode (from KEmulator) */
const HIDDEN_PROPERTY_PREFIXES = [
  'os.',           // os.name, os.version, os.arch
  'java.',         // java.version, java.vendor, etc.
  'sun.',          // Sun-specific properties
  'org.pigler.',   // Pigler automation
  'ru.nnproject.', // NNProject emulator detection
  'kemnn.',        // KEmulator NN detection
  'emulator.',     // Generic emulator detection
  'club.',         // Club detection
  'com.github.',   // GitHub packages
];

/* Properties that reveal emulator presence */
const EMULATOR_REVEALING_PROPERTIES = new Set([
  'emulator.version',
  'emulator.name',
  'kemulator.version',
  'kemulator.name',
  'nojvme.version',
  'nojvme.name',
]);

/* Classes to hide in hideEmulation mode (dot-separated names) */
// NOTE: used by drm_should_hide_class which receives slash-separated names
// matching drm_detection_classes from the C source.
const DRM_DETECTION_CLASS_PREFIXES = [
  'kemnn/',
  'emulator/',
  'club/',
  'com/github/',
  'nojvme/',
];

/*
 * Universal DRM bypass class prefix registry.
 * Any class whose fully-qualified name (using '/' separators) starts with one
 * of these prefixes will be treated as a DRM class.
 */
const g_drm_bypass_prefixes = [
  'GlomoReg/',         // popular J2ME copy protection system
  'GlomoRegStarter',   // entry class (not in GlomoReg/ package)
];

// --- Internal helper ----------------------------------------------------

/**
 * Emit a debug message to stderr, but only when debug_drm is enabled.
 * Maps to the C LOG_SAFE / NATIVE_DEBUG macros (which write to stderr).
 *
 * @param {string} msg
 */
function drmLog(msg) {
  if (g_drm_bypass.debug_drm) {
    process.stderr.write(msg + '\n');
  }
}

// --- Exported functions -------------------------------------------------

/**
 * Initialize DRM bypass with defaults.
 * Mirrors: void drm_bypass_init(void)
 */
export function drm_bypass_init() {
  g_drm_bypass.hide_emulation    = true;
  g_drm_bypass.imei              = '000000000000000';
  g_drm_bypass.imsi              = '000000000000000';
  g_drm_bypass.platform          = 'NokiaN73-1';
  g_drm_bypass.device_model      = 'Nokia N73';
  g_drm_bypass.bypass_permissions = true;
  g_drm_bypass.bypass_vserv      = true;
  g_drm_bypass.debug_drm         = false;

  // NATIVE_DEBUG is only active when g_j2me_runtime_debug != 0.
  // In JS we mirror that with debug_drm (which is false here, so no output).
  drmLog(
    `[DRM] Bypass initialized: hide=${g_drm_bypass.hide_emulation}, ` +
    `permissions=${g_drm_bypass.bypass_permissions}, ` +
    `vserv=${g_drm_bypass.bypass_vserv}`
  );
}

/**
 * Load DRM bypass configuration from a key=value text file.
 * Mirrors: void drm_bypass_load_config(const char* filename)
 *
 * @param {string} filename - path to config file
 */
export function drm_bypass_load_config(filename) {
  let text;
  try {
    text = readFileSync(filename, 'utf8');
  } catch (_e) {
    drmLog(`[DRM] Config file not found: ${filename} (using defaults)`);
    return;
  }

  for (const rawLine of text.split(/\r?\n/)) {
    // Skip comments and empty lines
    const line = rawLine.trimStart();
    if (line === '' || line[0] === '#') continue;

    const eqIdx = line.indexOf('=');
    if (eqIdx === -1) continue;

    const key   = line.slice(0, eqIdx).trim();
    const value = line.slice(eqIdx + 1).trim();

    switch (key) {
      case 'hide_emulation':
        g_drm_bypass.hide_emulation = (value === 'true' || value === '1');
        break;
      case 'bypass_permissions':
        g_drm_bypass.bypass_permissions = (value === 'true' || value === '1');
        break;
      case 'bypass_vserv':
        g_drm_bypass.bypass_vserv = (value === 'true' || value === '1');
        break;
      case 'imei':
        // Truncate to 15 chars (C field is char[16])
        g_drm_bypass.imei = value.slice(0, 15);
        break;
      case 'imsi':
        g_drm_bypass.imsi = value.slice(0, 15);
        break;
      case 'platform':
        g_drm_bypass.platform = value.slice(0, 63);
        break;
      case 'device_model':
        g_drm_bypass.device_model = value.slice(0, 31);
        break;
      case 'debug_drm':
        g_drm_bypass.debug_drm = (value === 'true' || value === '1');
        break;
      default:
        break;
    }
  }

  drmLog(
    `[DRM] Config loaded: hide=${g_drm_bypass.hide_emulation}, ` +
    `imei=${g_drm_bypass.imei}, platform=${g_drm_bypass.platform}`
  );
}

/**
 * Check if a system property should be hidden (hideEmulation mode).
 * Returns true if the property should return null to the application.
 * Mirrors: bool drm_should_hide_property(const char* key)
 *
 * @param {string|null} key
 * @returns {boolean}
 */
export function drm_should_hide_property(key) {
  if (!g_drm_bypass.hide_emulation) return false;
  if (key == null) return false;

  for (const prefix of HIDDEN_PROPERTY_PREFIXES) {
    if (key.startsWith(prefix)) {
      drmLog(`[DRM] Hiding property: ${key}`);
      return true;
    }
  }

  if (EMULATOR_REVEALING_PROPERTIES.has(key)) {
    drmLog(`[DRM] Hiding emulator property: ${key}`);
    return true;
  }

  return false;
}

/**
 * Get the spoofed value for a device property, or null if not spoofed.
 * Mirrors: const char* drm_get_spoofed_property(const char* key)
 *
 * @param {string|null} key
 * @returns {string|null}
 */
export function drm_get_spoofed_property(key) {
  if (key == null) return null;

  // IMEI spoofing - various property names used by different manufacturers
  if (key === 'com.nokia.mid.imei' ||
      key === 'device.imei' ||
      key === 'phone.imei' ||
      key === 'com.sonyericsson.imei' ||
      key === 'imei' ||
      key === 'IMEI') {
    return g_drm_bypass.imei;
  }

  // IMSI spoofing
  if (key === 'com.nokia.mid.imsi' ||
      key === 'device.imsi' ||
      key === 'phone.imsi' ||
      key === 'imsi' ||
      key === 'IMSI') {
    return g_drm_bypass.imsi;
  }

  // Platform spoofing
  if (key === 'microedition.platform') {
    return g_drm_bypass.platform || 'NokiaN73-1';
  }

  // Device model
  if (key === 'device.model' || key === 'device.model.name') {
    return g_drm_bypass.device_model || 'Nokia N73';
  }

  // Nokia-specific
  if (key === 'com.nokia.mid.ui.version')    return '1.0';
  if (key === 'com.nokia.mid.ui.fullscreen') return 'true';

  // Siemens-specific
  if (key === 'com.siemens.mp.screenwidth')  return '240';
  if (key === 'com.siemens.mp.screenheight') return '320';

  // Motorola-specific
  if (key === 'motorola.microedition.locale') return 'en';

  // Samsung-specific
  if (key === 'samsung.mime') return 'true';

  // Sony Ericsson-specific (already covered above, kept for exact C parity)
  if (key === 'com.sonyericsson.imei') return g_drm_bypass.imei;

  return null;
}

/**
 * Check if a class should be hidden from the application.
 * Used to hide emulator-specific classes in hideEmulation mode.
 * Mirrors: bool drm_should_hide_class(const char* class_name)
 *
 * @param {string|null} class_name - slash-separated class name
 * @returns {boolean}
 */
export function drm_should_hide_class(class_name) {
  if (!g_drm_bypass.hide_emulation) return false;
  if (class_name == null) return false;

  for (const prefix of DRM_DETECTION_CLASS_PREFIXES) {
    if (class_name.startsWith(prefix)) return true;
  }

  return false;
}

/**
 * Return the spoofed IMEI string.
 * Mirrors: const char* drm_get_imei(void)
 *
 * @returns {string}
 */
export function drm_get_imei() {
  return g_drm_bypass.imei;
}

/**
 * Check a permission against the bypass configuration.
 * Mirrors: int drm_check_permission(const char* permission)
 *
 * @param {string|null} _permission - ignored when bypassing all
 * @returns {1|0|-1}  1 = allowed, 0 = denied, -1 = unknown
 */
export function drm_check_permission(_permission) {
  if (g_drm_bypass.bypass_permissions) {
    // KEmulator returns 1 (allowed) for all permissions
    return 1;
  }
  // Unknown — let system handle it
  return -1;
}

// --- Universal DRM class bypass system ----------------------------------

/**
 * Check if a class name matches any registered DRM bypass prefix.
 * Mirrors: bool drm_is_drm_class(const char* class_name)
 *
 * @param {string|null} class_name - slash-separated class name
 * @returns {boolean}
 */
export function drm_is_drm_class(class_name) {
  if (class_name == null) return false;

  for (const prefix of g_drm_bypass_prefixes) {
    if (class_name.startsWith(prefix)) {
      drmLog(`[DRM] Bypassing class: ${class_name} (matched prefix: ${prefix})`);
      return true;
    }
  }

  return false;
}

/**
 * Get the default return type character code from a method descriptor.
 * Returns the last character of the descriptor if it is a valid JVM type
 * descriptor character, otherwise returns the empty string (C '\0' analog).
 * Mirrors: char drm_get_default_return_type(const char* descriptor)
 *
 * Valid return characters:
 *   'V'  void
 *   'Z'  boolean
 *   'B'  byte
 *   'C'  char
 *   'S'  short
 *   'I'  int
 *   'J'  long
 *   'F'  float
 *   'D'  double
 *   ';'  object reference  (descriptor ends with L...;)
 *   ']'  array reference   (descriptor ends with [...;] or [...[)
 *
 * @param {string|null} descriptor
 * @returns {string} single character, or '' for invalid/empty input
 */
export function drm_get_default_return_type(descriptor) {
  if (!descriptor || descriptor.length === 0) return '';

  const last = descriptor[descriptor.length - 1];

  switch (last) {
    case 'V': // void
    case 'Z': // boolean
    case 'B': // byte
    case 'C': // char
    case 'S': // short
    case 'I': // int
    case 'J': // long
    case 'F': // float
    case 'D': // double
    case ';': // object reference
    case ']': // array reference
      return last;
    default:
      return '';
  }
}

/**
 * Add a class prefix to the DRM bypass registry.
 * Can be called at runtime to register additional DRM systems.
 * Mirrors: void drm_add_bypass_prefix(const char* prefix)
 *
 * @param {string|null} prefix
 */
export function drm_add_bypass_prefix(prefix) {
  if (!prefix || prefix.length === 0) return;

  if (g_drm_bypass_prefixes.length >= DRM_MAX_PREFIXES - 1) {
    // LOG_SAFE is always active (not gated on debug flag)
    process.stderr.write(
      `[DRM] Cannot add prefix '${prefix}': registry full (max ${DRM_MAX_PREFIXES - 1})\n`
    );
    return;
  }

  // Check for duplicates (exact match, same length as in C strncmp with plen+1)
  if (g_drm_bypass_prefixes.includes(prefix)) return;

  g_drm_bypass_prefixes.push(prefix);

  // LOG_SAFE is always active
  process.stderr.write(
    `[DRM] Registered bypass prefix: ${prefix} (total: ${g_drm_bypass_prefixes.length})\n`
  );
}

/**
 * Get the list of all registered DRM bypass prefixes.
 * Returns a shallow copy of the internal array (read-only snapshot).
 * Mirrors: const char* const* drm_get_bypass_prefixes(void)
 *
 * @returns {string[]}
 */
export function drm_get_bypass_prefixes() {
  return g_drm_bypass_prefixes.slice();
}
