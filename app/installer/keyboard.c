/**
 * Copyright (c) 2011 ~ 2013 Deepin, Inc.
 *               2011 ~ 2013 Long Wei
 *
 * Author:      Long Wei <yilang2007lw@gmail.com>
 * Maintainer:  Long Wei <yilang2007lw@gamil.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 **/

#include "keyboard.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <libxklavier/xklavier.h>

static XklConfigRec *config = NULL;
static GHashTable *layout_variants_hash = NULL;


static void 
_foreach_variant (XklConfigRegistry *config, const XklConfigItem *item, gpointer data)
{
    const gchar *layout = (const gchar *)data;
    GList *variants = g_list_copy (g_hash_table_lookup (layout_variants_hash, layout));
    variants = g_list_append (variants, g_strdup (item->description));
    g_hash_table_replace (layout_variants_hash, g_strdup (layout), variants);
}

static void 
_foreach_layout(XklConfigRegistry *config, const XklConfigItem *item, gpointer data)
{
    GList *variants = NULL;
    g_hash_table_insert (layout_variants_hash, g_strdup (item->description), variants);
    xkl_config_registry_foreach_layout_variant(config, 
                                               item->name,
                                               _foreach_variant, 
                                               (gpointer) item->description);
}

void
init_keyboard_layouts () 
{
    g_printf ("init keyboard layouts");
    layout_variants_hash = g_hash_table_new_full ((GHashFunc) g_str_hash, 
                                                  (GEqualFunc) g_str_equal, 
                                                  (GDestroyNotify) g_free, 
                                                  (GDestroyNotify) g_list_free);

    Display *dpy = NULL;
    XklEngine *engine = NULL;
    XklConfigRegistry *cfg_reg = NULL;
    
    dpy = XOpenDisplay (NULL);
    if (dpy == NULL) {
        g_warning ("init keyboard layouts: XOpenDisplay\n");
        goto out;
    }

    engine = xkl_engine_get_instance (dpy);
    if (engine == NULL) {
        g_warning ("init keyboard layouts: xkl engine get instance\n");
        goto out;
    }

    config = xkl_config_rec_new ();
    xkl_config_rec_get_from_server (config, engine);
    if (config == NULL) {
        g_warning ("init keyboard layouts: xkl config rec\n");
        goto out;
    }

    cfg_reg = xkl_config_registry_get_instance (engine);
    if (cfg_reg == NULL) {
        g_warning ("init keyboard layouts: xkl config registry get instance\n");
        goto out;
    }
    if (!xkl_config_registry_load(cfg_reg, TRUE)) {
        g_warning ("init keyboard layouts: xkl config registry load\n");
        goto out;
    }

    xkl_config_registry_foreach_layout(cfg_reg, _foreach_layout, NULL);
out:
    if (engine != NULL) {
        g_object_unref (engine);
    }
    if (cfg_reg != NULL) {
        g_object_unref (cfg_reg);
    }
    if (dpy != NULL) {
        XCloseDisplay (dpy);
    }
}

JS_EXPORT_API 
JSObjectRef installer_get_keyboard_layouts ()
{
    JSObjectRef layouts = json_array_create ();
    if (layout_variants_hash == NULL) {
        init_keyboard_layouts ();
    }

    gsize index = 0;
    GList *keys = g_hash_table_get_keys (layout_variants_hash);

    for (index = 0; index < g_list_length (keys); index++) {
        gchar *layout = g_strdup (g_list_nth_data (keys, index));
        json_array_insert (layouts, index, jsvalue_from_cstr (get_global_context (), layout));
        g_free (layout);
    }

    return layouts;
}

JS_EXPORT_API 
JSObjectRef installer_get_layout_variants (const gchar *layout_name) 
{
    JSObjectRef layout_variants = json_array_create ();
    if (layout_variants_hash == NULL) {
        init_keyboard_layouts ();
    }

    gsize index = 0;
    GList *variants = (GList *) g_hash_table_lookup (layout_variants_hash, layout_name);

    for (index = 0; index < g_list_length (variants); index++) {
        gchar *variant = g_strdup (g_list_nth_data (variants, index));
        json_array_insert (layout_variants, index, jsvalue_from_cstr (get_global_context (), variant));
        g_free (variant);
    }

    return layout_variants;
}

JS_EXPORT_API
JSObjectRef installer_get_current_layout_variant ()
{
    JSObjectRef current = json_create ();

    if (config == NULL) {
        g_warning ("get current layout variant: config is NULL, init it\n");
        init_keyboard_layouts ();
    }
    if (config == NULL) {
        g_warning ("get current layout variant:xkl config null after init\n");
        return current;
    }

    gchar **layouts = g_strdupv (config->layouts);
    gchar **variants = g_strdupv (config->variants);

    JSObjectRef layout_array = json_array_create ();
    JSObjectRef variant_array = json_array_create ();

    gsize index = 0;
    for (index = 0; index < sizeof(layouts)/sizeof(gchar*); index++) {
        json_array_insert (layout_array, index, jsvalue_from_cstr (get_global_context (), layouts[index]));
    }
    json_append_value (current, "layouts", (JSValueRef) layout_array);

    for (index = 0; index < sizeof(variants)/sizeof(gchar*); index++) {
        json_array_insert (variant_array, index, jsvalue_from_cstr (get_global_context (), variants[index]));
    }
    json_append_value (current, "variants", (JSValueRef) variant_array);

    g_strfreev (layouts);
    g_strfreev (variants);

    return current;
}

JS_EXPORT_API 
void installer_set_keyboard_layout_variant (const gchar *layout, const gchar *variant)
{
    if (config == NULL) {
        g_warning ("set keyboard layout variant:xkl config null, init it\n");
        init_keyboard_layouts ();
    }
    if (config == NULL) {
        g_warning ("set keyboard layout variant:xkl config null after init\n");
        goto out;
    }

    gchar **layouts = g_new0 (char *, 2);
    layouts[0] = g_strdup (layout);

    if (layouts == NULL) {
        g_warning ("set keyboard layout variant:must specify layout\n");
        goto out;
    }
    xkl_config_rec_set_layouts (config, (const gchar **)layouts);

    gchar **variants = g_new0 (char *, 2);
    variants[0] = g_strdup (variant);

    if (variants != NULL) {
        xkl_config_rec_set_variants (config, (const gchar **)variants);
    }

    g_strfreev (layouts);
    g_strfreev (variants);
    emit_progress ("keyboard", "finish");
out:
    g_warning ("set keyboard layout variant failed, just skip this step");
    emit_progress ("keyboard", "finish");
}