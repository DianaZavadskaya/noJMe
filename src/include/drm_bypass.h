/*
 * J2ME Emulator - DRM Bypass System
 * Based on KEmulator analysis
 * 
 * This module implements various DRM/protection bypass mechanisms
 * similar to those found in KEmulator:
 * 
 * 1. hideEmulation - hide emulator presence
 * 2. IMEI spoofing - fake device identifiers  
 * 3. checkPermission bypass - always return "allowed"
 * 4. Platform spoofing - fake device model
 */

#ifndef DRM_BYPASS_H
#define DRM_BYPASS_H

#include <stdbool.h>
#include <stdint.h>

/* Global DRM bypass configuration */
typedef struct {
    /* Hide emulator presence (like KEmulator's hideEmulation) */
    bool hide_emulation;
    
    /* Spoof device identifiers */
    char imei[16];           /* 15-digit IMEI + null */
    char imsi[16];           /* IMSI */
    char platform[64];       /* microedition.platform */
    char device_model[32];   /* Device model name */
    
    /* Permission bypass */
    bool bypass_permissions; /* Always return "allowed" for checkPermission */
    
    /* Vserv/ALW1 bypass */
    bool bypass_vserv;
    
    /* Debug */
    bool debug_drm;
} DrmBypassConfig;

/* Global configuration instance */
extern DrmBypassConfig g_drm_bypass;

/* Initialize DRM bypass with defaults */
void drm_bypass_init(void);

/* Load DRM bypass config from file */
void drm_bypass_load_config(const char* filename);

/* 
 * Check if property should be hidden (hideEmulation mode)
 * Returns true if property should return null
 */
bool drm_should_hide_property(const char* key);

/*
 * Get spoofed device property
 * Returns value if property is spoofed, NULL otherwise
 */
const char* drm_get_spoofed_property(const char* key);

/*
 * Check if class should be hidden from application
 * Used to hide emulator-specific classes
 */
bool drm_should_hide_class(const char* class_name);

/*
 * Get spoofed IMEI
 */
const char* drm_get_imei(void);

/*
 * Check permission - always returns allowed if bypass enabled
 * Returns: 1 = allowed, 0 = denied, -1 = unknown
 */
int drm_check_permission(const char* permission);

/*
 * Universal DRM class bypass system
 * 
 * Instead of hardcoding specific DRM class names in multiple locations,
 * maintain a registry of class prefixes that should be bypassed.
 * Any class whose name starts with one of these prefixes will:
 * - Have null-object method calls return safe defaults instead of NPE
 * - Have null-object field access return 0/null instead of NPE
 * - Have unimplemented native methods return type-appropriate defaults
 * - Not crash the game if exceptions propagate from them
 *
 * To add a new DRM system, simply add its package prefix to this list.
 */

/* Maximum number of DRM class prefixes */
#define DRM_MAX_PREFIXES 32

/*
 * Check if a class name matches any DRM bypass prefix.
 * This is THE central function used by opcodes.c and native.c
 * to decide whether to suppress exceptions for null objects.
 *
 * Usage:
 *   if (drm_is_drm_class(class_name)) { return safe_default; }
 *
 * Returns: true if class should be bypassed
 */
bool drm_is_drm_class(const char* class_name);

/*
 * Get the default return value for a method descriptor.
 * Used by native.c fallback handler to return correct type.
 *
 * Returns: descriptor of the default value (last char of method descriptor)
 *   ';' = object ref -> return null
 *   'Z' = boolean -> return 0
 *   'I','S','B','C' = integer types -> return 0
 *   'J' = long -> return 0
 *   'D' = double -> return 0.0
 *   'F' = float -> return 0.0f
 *   'V' = void -> return nothing
 */
char drm_get_default_return_type(const char* descriptor);

/*
 * Add a class prefix to the DRM bypass registry.
 * Can be called at runtime to register additional DRM systems.
 */
void drm_add_bypass_prefix(const char* prefix);

/*
 * Get the list of all registered DRM bypass prefixes (for debugging).
 * Returns pointer to NULL-terminated array of prefix strings.
 */
const char* const* drm_get_bypass_prefixes(void);

#endif /* DRM_BYPASS_H */
