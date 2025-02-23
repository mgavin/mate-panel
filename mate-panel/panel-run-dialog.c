/*
 * panel-run-dialog.c:
 *
 * Copyright (C) 2003 Frank Worsley <fworsley@shaw.ca>
 * Copyright (C) 2012-2021 MATE Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.

 * Authors:
 *	Frank Worsley <fworsley@shaw.ca>
 *
 * Based on code by:
 *	Havoc Pennington <hp@pobox.com>
 *      George Lebl <jirka@5z.com>
 *	Mark McLoughlin <mark@skynet.ie>
 */

#include <config.h>

#include "panel-run-dialog.h"

#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <glib.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include <matemenu-tree.h>

#define MATE_DESKTOP_USE_UNSTABLE_API
#include <libmate-desktop/mate-desktop-utils.h>

#include <libpanel-util/panel-error.h>
#include <libpanel-util/panel-glib.h>
#include <libpanel-util/panel-gtk.h>
#include <libpanel-util/panel-keyfile.h>
#include <libpanel-util/panel-show.h>

#include "panel-util.h"
#include "panel-globals.h"
#include "panel-enums.h"
#include "panel-profile.h"
#include "panel-schemas.h"
#include "panel-stock-icons.h"
#include "panel-multimonitor.h"
#include "menu.h"
#include "panel-lockdown.h"
#include "panel-icon-names.h"

#ifdef HAVE_X11
#include "xstuff.h"
#endif

typedef struct {
	GtkWidget        *run_dialog;

	GtkWidget        *main_box;
	GtkWidget        *program_list_box;

	GtkWidget        *combobox;
	GtkWidget        *pixmap;
	GtkWidget        *run_button;
	GtkWidget        *file_button;
	GtkWidget        *list_expander;
	GtkWidget        *terminal_checkbox;
	GtkWidget        *program_label;
	GtkWidget        *program_list;

	long              changed_id;

	GtkListStore     *program_list_store;

	GHashTable       *dir_hash;
	GList		 *possible_executables;
	GList		 *completion_items;
	GtkEntryCompletion *completion;

	int	          add_items_idle_id;
	int		  find_command_idle_id;
	gboolean	  use_program_list;
	gboolean	  completion_started;

	GIcon		 *icon;
	char             *desktop_path;
	char		 *item_name;

	GSettings        *settings;
} PanelRunDialog;

enum {
	COLUMN_GICON,
	COLUMN_NAME,
	COLUMN_ACCELERATOR_MASK,
	COLUMN_ACCELERATOR_KEY_VALUE,
	COLUMN_COMMENT,
	COLUMN_PATH,
	COLUMN_EXEC,
	COLUMN_VISIBLE,
	NUM_COLUMNS
};

typedef struct {
        gint list_item_idx;
        GdkModifierType modifier;
        guint key_id;
} AcceleratorKeyMapping;

const AcceleratorKeyMapping accelerator_key_mapping[] =
{
        {0, GDK_MOD1_MASK, GDK_KEY_1 },
        {1, GDK_MOD1_MASK, GDK_KEY_2 },
        {2, GDK_MOD1_MASK, GDK_KEY_3 },
        {3, GDK_MOD1_MASK, GDK_KEY_4 },
        {4, GDK_MOD1_MASK, GDK_KEY_5 },
        {5, GDK_MOD1_MASK, GDK_KEY_6 },
        {6, GDK_MOD1_MASK, GDK_KEY_7 },
        {7, GDK_MOD1_MASK, GDK_KEY_8 },
        {8, GDK_MOD1_MASK, GDK_KEY_9 },
        {9, GDK_MOD1_MASK, GDK_KEY_0 },
};

static GHashTable *accelerator_keys_to_tree_iter_map = NULL;

static PanelRunDialog *static_dialog = NULL;

static void panel_run_dialog_disconnect_pixmap (PanelRunDialog *dialog);

#define PANEL_RUN_SCHEMA "org.mate.panel"
#define PANEL_RUN_HISTORY_KEY "history-mate-run"
#define PANEL_RUN_HISTORY_MAX_SIZE_KEY "history-max-size-mate-run"
#define PANEL_RUN_HISTORY_REVERSE_KEY "history-reverse-mate-run"
#define PANEL_RUN_SHOW_PROGRAM_LIST_KEY "show-program-list"

static GtkTreeModel *
_panel_run_get_recent_programs_list (PanelRunDialog *dialog)
{
	GtkListStore *list;
	gchar       **items;
	guint         history_max_size;
	gboolean      history_reverse = FALSE;
	guint         i = 0;

	list = gtk_list_store_new (1, G_TYPE_STRING);

	history_max_size = g_settings_get_uint (dialog->settings, PANEL_RUN_HISTORY_MAX_SIZE_KEY);
	history_reverse = g_settings_get_boolean (dialog->settings, PANEL_RUN_HISTORY_REVERSE_KEY);
	items = g_settings_get_strv (dialog->settings, PANEL_RUN_HISTORY_KEY);
	for (i = 0; i < history_max_size && items[i]; i++) {
		GtkTreeIter iter;
		/* add history in reverse */
		if (history_reverse)
			gtk_list_store_prepend (list, &iter);
		else
			gtk_list_store_append (list, &iter);
		gtk_list_store_set (list, &iter, 0, items[i], -1);
	}

	g_strfreev (items);

	return GTK_TREE_MODEL (list);
}

static void
_panel_run_save_recent_programs_list (PanelRunDialog   *dialog,
				      GtkComboBox      *entry,
				      char             *lastcommand)
{
	gboolean history_reverse =
		g_settings_get_boolean (dialog->settings, PANEL_RUN_HISTORY_REVERSE_KEY);
	guint history_max_size =
		g_settings_get_uint (dialog->settings, PANEL_RUN_HISTORY_MAX_SIZE_KEY);

	if (history_max_size == 0)
		g_settings_set_strv (dialog->settings, PANEL_RUN_HISTORY_KEY, NULL);
	else {
		GtkTreeIter   iter;
		GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (entry));
		/* reasonable upper bound for zero-terminated array with new command */
		gchar        *items[gtk_tree_model_iter_n_children (model, NULL) + 2];
		guint         items_added = 0;

		items[0] = lastcommand;

		if (gtk_tree_model_get_iter_first (model, &iter)) {
			char *command;
			do {
				gtk_tree_model_get (model, &iter, 0, &command, -1);
				if (g_strcmp0 (command, lastcommand) == 0) {
					g_free (command);
					continue;
				}
				items[items_added + 1] = command;
				items_added++;
			} while (gtk_tree_model_iter_next (model, &iter));
		}

		if (history_reverse) {
			guint pos;
			for (pos = 0; pos < items_added / 2; pos++)
			{
				gchar *tmp = items[pos + 1];
				items[pos + 1] = items[items_added - pos];
				items[items_added - pos] = tmp;
			}
		}

		if (history_max_size < items_added + 1) {
			g_clear_pointer (&items[history_max_size], g_free);
		} else {
			items[items_added + 1] = NULL;
		}
		g_settings_set_strv (dialog->settings, PANEL_RUN_HISTORY_KEY, (const gchar **)items);

		/* free the items */
		for (; items_added; items_added--)
			g_free (items[items_added]);
	}
}

static void
panel_run_dialog_destroy (PanelRunDialog *dialog)
{
	GList *l;

	dialog->changed_id = 0;

	g_object_unref (dialog->program_list_box);

	g_clear_object (&dialog->icon);
	g_clear_pointer (&dialog->desktop_path, g_free);
	g_clear_pointer (&dialog->item_name, g_free);

	if (dialog->add_items_idle_id)
		g_source_remove (dialog->add_items_idle_id);
	dialog->add_items_idle_id = 0;

	if (dialog->find_command_idle_id)
		g_source_remove (dialog->find_command_idle_id);
	dialog->find_command_idle_id = 0;

	g_clear_object (&dialog->settings);

	if (dialog->dir_hash)
		g_hash_table_destroy (dialog->dir_hash);
	dialog->dir_hash = NULL;

	if (accelerator_keys_to_tree_iter_map)
		g_hash_table_destroy (accelerator_keys_to_tree_iter_map);
	accelerator_keys_to_tree_iter_map = NULL;

	for (l = dialog->possible_executables; l; l = l->next)
		g_free (l->data);
	g_list_free (dialog->possible_executables);
	dialog->possible_executables = NULL;

	for (l = dialog->completion_items; l; l = l->next)
		g_free (l->data);
	g_list_free (dialog->completion_items);
	dialog->completion_items = NULL;

	panel_run_dialog_disconnect_pixmap (dialog);

	g_free (dialog);
}

static const char *
panel_run_dialog_get_combo_text (PanelRunDialog *dialog)
{
	GtkWidget *entry;

	entry = gtk_bin_get_child (GTK_BIN (dialog->combobox));

	return gtk_entry_get_text (GTK_ENTRY (entry));
}

static void
panel_run_dialog_set_default_icon (PanelRunDialog *dialog, gboolean set_drag)
{
	gtk_image_set_from_icon_name (GTK_IMAGE (dialog->pixmap),
				      PANEL_ICON_RUN,
				      GTK_ICON_SIZE_DIALOG);

	gtk_window_set_icon_name (GTK_WINDOW (dialog->run_dialog),
				  PANEL_ICON_RUN);

	if (set_drag)
		gtk_drag_source_set_icon_name (dialog->run_dialog,
					       PANEL_ICON_LAUNCHER);
}

static void
panel_run_dialog_set_icon (PanelRunDialog *dialog,
			   GIcon          *icon,
			   gboolean        force)
{
	GdkPixbuf *pixbuf = NULL;

	if (!force && g_icon_equal(icon, dialog->icon))
		return;

	g_clear_object(&(dialog->icon));

	if (icon) {
		int          size;

		gtk_icon_size_lookup (GTK_ICON_SIZE_DIALOG, &size, NULL);

		GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();
		GtkIconInfo *icon_info = gtk_icon_theme_lookup_by_gicon (icon_theme, icon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
		pixbuf = gtk_icon_info_load_icon (icon_info, NULL);
		g_object_unref (icon_info);
	}

	if (pixbuf) {
		dialog->icon = g_object_ref (icon);

		/* Don't bother scaling the image if it's too small.
		 * Scaled looks worse than a smaller image.
		 */
		gtk_image_set_from_pixbuf (GTK_IMAGE (dialog->pixmap), pixbuf);

		/* FIXME: it'd be better to set an icon of the correct size,
		 * (ditto for the drag icon?)
		 */
		gtk_window_set_icon (GTK_WINDOW (dialog->run_dialog), pixbuf);

		gtk_drag_source_set_icon_gicon (dialog->run_dialog, dialog->icon);
		g_object_unref (pixbuf);
	} else {
		panel_run_dialog_set_default_icon (dialog, TRUE);
	}
}

static gboolean
command_is_executable (const char   *command,
		       int          *argcp,
		       char       ***argvp)
{
	gboolean   result;
	GRegex    *regex = NULL;
	gsize      argv_len;
	g_autofree gchar *home_path = NULL;
	char     **argv;
	char      *path;
	int        argc;

	result = g_shell_parse_argv (command, &argc, &argv, NULL);

	if (!result)
		return FALSE;

	regex = g_regex_new ("^~/", 0, 0, NULL);
	argv_len = g_strv_length (argv);
	home_path = g_build_filename (g_get_home_dir (), "/", NULL);
	for (gsize i = 0; i < argv_len; i++)
	{
		gchar *tmp_argv = NULL;

		tmp_argv = g_regex_replace_literal (regex, argv[i], -1, 0, home_path, 0, NULL);
		g_free (argv[i]);
		argv[i] = tmp_argv;
	}
	g_regex_unref (regex);
	path = g_find_program_in_path (argv[0]);

	if (!path) {
		g_strfreev (argv);
		return FALSE;
	}

	/* If we pass an absolute path to g_find_program it just returns
	 * that absolute path without checking if it is executable. Also
	 * make sure its a regular file so we don't try to launch
	 * directories or device nodes.
	 */
	if (!g_file_test (path, G_FILE_TEST_IS_EXECUTABLE) ||
	    !g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
		g_free (path);
		g_strfreev (argv);
		return FALSE;
	}

	g_free (path);

	if (argcp)
		*argcp = argc;
	if (argvp)
		*argvp = argv;

	return TRUE;
}

/*
 * Set the DISPLAY variable, to be use by g_spawn_async.
 */
static void
set_environment (gpointer display)
{
  g_setenv ("DISPLAY", display, TRUE);
}

static void
dummy_child_watch (GPid         pid,
		   		   gint         status,
		  		   gpointer user_data)
{
	/* Nothing, this is just to ensure we don't double fork
	 * and break pkexec:
	 * https://bugzilla.gnome.org/show_bug.cgi?id=675789
	 */
}

static gboolean
panel_run_dialog_launch_command (PanelRunDialog *dialog,
				 const char     *command,
				 const char     *locale_command)
{
	GdkDisplay *display;
	GdkScreen  *screen;
	gboolean    result;
	GError     *error = NULL;
	char      **argv;
	int         argc;
	char       *display_name;
	GPid        pid;

	if (!command_is_executable (locale_command, &argc, &argv))
		return FALSE;

	screen = gtk_window_get_screen (GTK_WINDOW (dialog->run_dialog));

	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->terminal_checkbox)))
		mate_desktop_prepend_terminal_to_vector (&argc, &argv);

	display = gdk_screen_get_display (screen);
	display_name = g_strdup (gdk_display_get_name (display));

	result = g_spawn_async (NULL, /* working directory */
				argv,
				NULL, /* envp */
				G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
				set_environment,
				display_name,
				&pid,
				&error);

	if (!result) {
		char *primary;

		primary = g_markup_printf_escaped (_("Could not run command '%s'"),
						   command);
		panel_error_dialog (GTK_WINDOW (dialog->run_dialog), NULL,
				    "cannot_spawn_command", TRUE,
				    primary, error->message);
		g_free (primary);

		g_error_free (error);
	} else {
		g_child_watch_add (pid, dummy_child_watch, NULL);
	}

	g_strfreev (argv);
	g_free (display_name);

	return result;
}

static void
panel_run_dialog_execute (PanelRunDialog *dialog)
{
	GError   *error;
	gboolean  result;
	char     *command;
	char     *disk;
	char     *scheme;

	command = g_strdup (panel_run_dialog_get_combo_text (dialog));
	command = g_strchug (command);

	if (!command || !command [0]) {
		g_free (command);
		return;
	}

	error = NULL;
	disk = g_locale_from_utf8 (command, -1, NULL, NULL, &error);

	if (!disk || error) {
		char *primary;

		primary = g_strdup_printf (_("Could not convert '%s' from UTF-8"),
					   command);
		panel_error_dialog (GTK_WINDOW (dialog->run_dialog), NULL,
				    "cannot_convert_command_from_utf8", TRUE,
				    primary, error->message);
		g_free (primary);

		g_error_free (error);
		return;
	}

	result = FALSE;

	scheme = g_uri_parse_scheme (disk);
	/* if it's an absolute path or not a URI, it's possibly an executable,
	 * so try it before displaying it */
	if (g_path_is_absolute (disk) || !scheme)
		result = panel_run_dialog_launch_command (dialog, command, disk);

	if (!result) {
		GFile     *file;
		char      *uri;
		GdkScreen *screen;

		file = panel_util_get_file_optional_homedir (command);
		uri = g_file_get_uri (file);
		g_object_unref (file);

		screen = gtk_window_get_screen (GTK_WINDOW (dialog->run_dialog));
		result = panel_show_uri (screen, uri,
					 gtk_get_current_event_time (), NULL);

		g_free (uri);
	}

	if (result) {
		/* only save working commands in history */
		_panel_run_save_recent_programs_list
			(dialog, GTK_COMBO_BOX (dialog->combobox), command);

		/* only close the dialog if we successfully showed or launched
		 * something */
		gtk_widget_destroy (dialog->run_dialog);
	}

	g_free (command);
	g_free (disk);
	g_free (scheme);
}

static void
panel_run_dialog_response (PanelRunDialog *dialog,
			   int             response,
			   GtkWidget      *run_dialog)
{

	dialog->completion_started = FALSE;

	switch (response) {
	case GTK_RESPONSE_OK:
		panel_run_dialog_execute (dialog);
		break;
	case GTK_RESPONSE_CANCEL:
		gtk_widget_destroy (dialog->run_dialog);
		break;
	case GTK_RESPONSE_HELP:
		panel_show_help (gtk_window_get_screen (GTK_WINDOW (run_dialog)),
				 "mate-user-guide", "gospanel-23", NULL);
		break;
	default:
		break;
	}
}

/* only quote the string if really needed */
static char *
quote_string (const char *s)
{
	const char *p;

	for (p = s; *p != '\0'; p++) {
		if ((*p >= 'a' && *p <= 'z') ||
		    (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') ||
		    strchr ("-_./=:", *p) != NULL)
			;
		else
			return g_shell_quote (s);
	}

	return g_strdup (s);
}

static void
panel_run_dialog_append_file_utf8 (PanelRunDialog *dialog,
				   const char     *file)
{
	const char *text;
	char       *quoted;
	GtkWidget  *entry;

	/* Don't allow filenames beginning with '-' */
	if (!file || !file[0] || file[0] == '-')
		return;

	quoted = quote_string (file);
	entry = gtk_bin_get_child (GTK_BIN (dialog->combobox));
	text = gtk_entry_get_text (GTK_ENTRY (entry));
	if (text && text [0]) {
		char *temp = g_strconcat (text, " ", quoted, NULL);
		gtk_entry_set_text (GTK_ENTRY (entry), temp);
		g_free (temp);
	} else
		gtk_entry_set_text (GTK_ENTRY (entry), quoted);

	g_free (quoted);
}

static void
panel_run_dialog_append_file (PanelRunDialog *dialog,
			      const char *file)
{
	char *utf8_file;

	if (!file)
		return;

	utf8_file = g_filename_to_utf8 (file, -1, NULL, NULL, NULL);

	if (utf8_file)
		panel_run_dialog_append_file_utf8 (dialog, utf8_file);

	g_free (utf8_file);
}

static gboolean
fuzzy_command_match (const char *cmd1,
		     const char *cmd2,
		     gboolean   *fuzzy)
{
	char **tokens;
	char  *word1, *word2;

	g_return_val_if_fail (cmd1 && cmd2, TRUE);

	*fuzzy = FALSE;

	if (!strcmp (cmd1, cmd2))
		return TRUE;

	/* find basename of exec from desktop item.
	   strip of all arguments after the initial command */
	tokens = g_strsplit (cmd1, " ", -1);
	if (!tokens || !tokens [0]) {
		g_strfreev (tokens);
		return FALSE;
	}

	word1 = g_path_get_basename (tokens [0]);
	g_strfreev (tokens);

	/* same for the user command */
	tokens = g_strsplit (cmd2, " ", -1);
	if (!tokens || !tokens [0]) {
		g_free (word1);
		g_strfreev (tokens);
		return FALSE;
	}

	word2 = g_path_get_basename (tokens [0]);
	g_strfreev (tokens);

	if (!strcmp (word1, word2)) {
		g_free (word1);
		g_free (word2);
		*fuzzy = TRUE;
		return TRUE;
	}

	g_free (word1);
	g_free (word2);

	return FALSE;
}

static gboolean
panel_run_dialog_find_command_idle (PanelRunDialog *dialog)
{
	GtkTreeIter   iter;
	GtkTreeModel *model;
	GtkTreePath  *path;
	char         *text;
	GIcon        *found_icon;
	char         *found_name;
	gboolean      fuzzy;
	gint          visible_program_idx = 0;

	model = GTK_TREE_MODEL (dialog->program_list_store);
	path = gtk_tree_path_new_first ();

	if (!path || !gtk_tree_model_get_iter (model, &iter, path)) {
		if (path)
			gtk_tree_path_free (path);

		panel_run_dialog_set_icon (dialog, NULL, FALSE);

		dialog->find_command_idle_id = 0;
		return FALSE;
	}

	text = g_strdup (panel_run_dialog_get_combo_text (dialog));
	found_icon = NULL;
	found_name = NULL;
	fuzzy = FALSE;
	g_hash_table_remove_all (accelerator_keys_to_tree_iter_map);

	do {
		char *exec = NULL;
		GIcon *icon = NULL;
		char *name = NULL;
		char *comment = NULL;
		gboolean visible = FALSE;

		gtk_tree_model_get (model, &iter,
				    COLUMN_EXEC,      &exec,
				    COLUMN_GICON,     &icon,
				    COLUMN_NAME,      &name,
				    COLUMN_COMMENT,   &comment,
				    -1);

		if (!fuzzy && exec && icon &&
		    fuzzy_command_match (text, exec, &fuzzy)) {
			g_clear_object (&found_icon);
			g_free (found_name);

			found_icon = g_object_ref (icon);
			found_name = g_strdup (name);

			gtk_list_store_set (dialog->program_list_store,
					    &iter,
					    COLUMN_VISIBLE, TRUE,
					    -1);
		} else if (panel_g_utf8_strstrcase (exec, text) != NULL ||
			   panel_g_utf8_strstrcase (name, text) != NULL ||
			   panel_g_utf8_strstrcase (comment, text) != NULL) {
			gtk_list_store_set (dialog->program_list_store,
					    &iter,
					    COLUMN_VISIBLE, TRUE,
					    -1);
		} else {
			gtk_list_store_set (dialog->program_list_store,
					    &iter,
					    COLUMN_VISIBLE, FALSE,
					    -1);
		}

		gtk_tree_model_get (model, &iter, COLUMN_VISIBLE, &visible, -1);
		if (visible && visible_program_idx < G_N_ELEMENTS (accelerator_key_mapping)) {
			gtk_list_store_set (dialog->program_list_store,
					    &iter,
					    COLUMN_ACCELERATOR_MASK, (gint)accelerator_key_mapping[visible_program_idx].modifier,
					    COLUMN_ACCELERATOR_KEY_VALUE, accelerator_key_mapping[visible_program_idx].key_id,
					    -1);
			g_hash_table_insert (accelerator_keys_to_tree_iter_map, GUINT_TO_POINTER(accelerator_key_mapping[visible_program_idx].key_id), GINT_TO_POINTER(visible_program_idx));
			visible_program_idx++;
		} else {
			gtk_list_store_set (dialog->program_list_store,
					    &iter,
					    COLUMN_ACCELERATOR_MASK, 0,
					    COLUMN_ACCELERATOR_KEY_VALUE, 0,
					    -1);
		}

		g_free (exec);
		if (icon != NULL)
			g_object_unref (icon);
		g_free (name);
		g_free (comment);

        } while (gtk_tree_model_iter_next (model, &iter));

	if (gtk_tree_model_get_iter (gtk_tree_view_get_model (GTK_TREE_VIEW (dialog->program_list)),
				     &iter, path))
		gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (dialog->program_list),
					      path, NULL, FALSE, 0, 0);

	gtk_tree_path_free (path);

	panel_run_dialog_set_icon (dialog, found_icon, FALSE);
	/* FIXME update dialog->program_label */

	g_clear_object (&found_icon);
	g_free (text);

	g_free (dialog->item_name);
	dialog->item_name = found_name;

	dialog->find_command_idle_id = 0;
	return FALSE;
}

static int
compare_applications (MateMenuTreeEntry *a,
		      MateMenuTreeEntry *b)
{
	GDesktopAppInfo *infoa, *infob;
	const gchar* name1;
	const gchar* name2;
	gint compare;

	infoa = matemenu_tree_entry_get_app_info (a);
	infob = matemenu_tree_entry_get_app_info (b);
	name1 = g_app_info_get_name(G_APP_INFO(infoa));
	name2 = g_app_info_get_name(G_APP_INFO(infob));
	compare = g_utf8_collate(name1, name2);
	return compare;
}

static GSList *get_all_applications_from_dir (MateMenuTreeDirectory *directory,
					      GSList            *list);

static GSList *
get_all_applications_from_alias (MateMenuTreeAlias *alias,
				 GSList         *list)
{
	gpointer item;
	switch (matemenu_tree_alias_get_aliased_item_type (alias)) {
		case MATEMENU_TREE_ITEM_ENTRY:
			item = matemenu_tree_alias_get_aliased_entry (alias);
			list = g_slist_append (list, (MateMenuTreeEntry*) item);
			break;
		case MATEMENU_TREE_ITEM_DIRECTORY:
			item = matemenu_tree_alias_get_aliased_directory (alias);
			list = get_all_applications_from_dir ((MateMenuTreeDirectory*) item, list);
			matemenu_tree_item_unref(item);
			break;
		default:
			break;
	}
	return list;
}

static GSList *
get_all_applications_from_dir (MateMenuTreeDirectory *directory,
			       GSList             *list)
{
	MateMenuTreeIter *iter;
	MateMenuTreeItemType type;

	iter = matemenu_tree_directory_iter (directory);
	while ((type = matemenu_tree_iter_next (iter)) != MATEMENU_TREE_ITEM_INVALID) {
		gpointer item;
		switch (type) {
			case MATEMENU_TREE_ITEM_ENTRY:
				item = matemenu_tree_iter_get_entry (iter);
				list = g_slist_append (list, (MateMenuTreeEntry*) item);
				break;

			case MATEMENU_TREE_ITEM_DIRECTORY:
				item = matemenu_tree_iter_get_directory (iter);
				list = get_all_applications_from_dir ((MateMenuTreeDirectory*)item, list);
				matemenu_tree_item_unref(item);
				break;

			case MATEMENU_TREE_ITEM_ALIAS:
				item = matemenu_tree_iter_get_alias (iter);
				list = get_all_applications_from_alias ((MateMenuTreeAlias *)item, list);
				matemenu_tree_item_unref(item);
				break;

			default:
				break;
		}
	}
	matemenu_tree_iter_unref (iter);
	return list;
}

static GSList* get_all_applications(void)
{
	MateMenuTree* tree;
	MateMenuTreeDirectory* root;
	GError        *error = NULL;
	GSList* retval;

	tree = matemenu_tree_new ("mate-applications.menu", MATEMENU_TREE_FLAGS_SORT_DISPLAY_NAME);
	if (!matemenu_tree_load_sync (tree, &error)) {
		g_printerr ("Failed to load tree: %s\n", error->message);
		g_error_free(error);
		g_object_unref(tree);
		return NULL;
	}

	root = matemenu_tree_get_root_directory (tree);
	if (root == NULL){
		g_object_unref(tree);
		return NULL;
	}

	retval = get_all_applications_from_dir(root, NULL);

	matemenu_tree_item_unref(root);
	g_object_unref(tree);

	retval = g_slist_sort(retval, (GCompareFunc) compare_applications);

	return retval;
}

static gboolean
panel_run_dialog_add_items_idle (PanelRunDialog *dialog)
{
	GtkCellRenderer   *renderer;
	GtkTreeViewColumn *column;
	GtkTreeModel      *model_filter;
	GSList            *all_applications;
	GSList            *l;
	GSList            *next;
	const char        *prev_name;

	/* create list store */
	dialog->program_list_store = gtk_list_store_new (NUM_COLUMNS,
							 G_TYPE_ICON,
							 G_TYPE_STRING,
							 G_TYPE_INT, // For accelerator modifier mask
							 G_TYPE_UINT, // For accelerator key value
							 G_TYPE_STRING,
							 G_TYPE_STRING,
							 G_TYPE_STRING,
							 G_TYPE_BOOLEAN);

	all_applications = get_all_applications ();

	/* Strip duplicates */
	prev_name = NULL;
	for (l = all_applications; l; l = next) {
		MateMenuTreeEntry *entry = l->data;
		const char     *entry_name;

		next = l->next;
		GDesktopAppInfo *ginfo;
		ginfo = matemenu_tree_entry_get_app_info (entry);

		entry_name = g_app_info_get_display_name(G_APP_INFO(ginfo));
		if (prev_name && entry_name && strcmp (entry_name, prev_name) == 0) {
			matemenu_tree_item_unref (entry);

			all_applications = g_slist_delete_link (all_applications, l);
		} else {
			prev_name = entry_name;
		}
	}

	gint i = 0;
	g_hash_table_remove_all (accelerator_keys_to_tree_iter_map);

	for (l = all_applications; l; l = l->next) {
		MateMenuTreeEntry *entry = l->data;
		GtkTreeIter    iter;
		GDesktopAppInfo *ginfo;
		GIcon *gicon = NULL;

		ginfo = matemenu_tree_entry_get_app_info (entry);
		gicon = g_app_info_get_icon(G_APP_INFO(ginfo));

		gtk_list_store_append (dialog->program_list_store, &iter);
		gtk_list_store_set (dialog->program_list_store, &iter,
				    COLUMN_GICON,     gicon,
				    COLUMN_NAME,      g_app_info_get_display_name(G_APP_INFO(ginfo)),
				    COLUMN_COMMENT,   g_app_info_get_description(G_APP_INFO(ginfo)),
				    COLUMN_EXEC,      g_app_info_get_commandline(G_APP_INFO(ginfo)),
				    COLUMN_PATH,      matemenu_tree_entry_get_desktop_file_path (entry),
				    COLUMN_VISIBLE,   TRUE,
				    -1);
		if (i < G_N_ELEMENTS (accelerator_key_mapping)) {
			gtk_list_store_set (dialog->program_list_store, &iter,
					    COLUMN_ACCELERATOR_MASK, (gint)accelerator_key_mapping[i].modifier,
					    COLUMN_ACCELERATOR_KEY_VALUE, accelerator_key_mapping[i].key_id,
					    -1);
			g_hash_table_insert (accelerator_keys_to_tree_iter_map, GUINT_TO_POINTER(accelerator_key_mapping[i].key_id), GINT_TO_POINTER(i));
			i++;
		} else {
			gtk_list_store_set (dialog->program_list_store, &iter,
					    COLUMN_ACCELERATOR_MASK, (gint)GDK_MOD1_MASK,
					    COLUMN_ACCELERATOR_KEY_VALUE, 0,
					    -1);
		}
	}
	g_slist_free_full (all_applications, matemenu_tree_item_unref);

	model_filter = gtk_tree_model_filter_new (GTK_TREE_MODEL (dialog->program_list_store),
						  NULL);
	gtk_tree_model_filter_set_visible_column (GTK_TREE_MODEL_FILTER (model_filter),
						  COLUMN_VISIBLE);

	gtk_tree_view_set_model (GTK_TREE_VIEW (dialog->program_list),
				 model_filter);
	/* FIXME use the same search than the fuzzy one? */
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (dialog->program_list),
					 COLUMN_NAME);

	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "stock-size", panel_menu_icon_get_size(), NULL);
	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
                                             "gicon", COLUMN_GICON,
                                             NULL);

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
                                             "text", COLUMN_NAME,
                                             NULL);

	gtk_tree_view_column_set_sizing (column, 
	                                 GTK_TREE_VIEW_COLUMN_AUTOSIZE);

	gtk_tree_view_append_column (GTK_TREE_VIEW (dialog->program_list), column);

	renderer = gtk_cell_renderer_accel_new ();
	g_object_set (renderer, "accel-mode", GTK_CELL_RENDERER_ACCEL_MODE_GTK,
	             "editable", FALSE, NULL);

	column = gtk_tree_view_column_new_with_attributes ("Shortcut", renderer,
	                                                  "accel-mods", COLUMN_ACCELERATOR_MASK, "accel-key",
	                                                  COLUMN_ACCELERATOR_KEY_VALUE, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (dialog->program_list), column);

	dialog->add_items_idle_id = 0;
	return G_SOURCE_REMOVE;
}

static char *
remove_parameters (const char *exec)
{
	GString *str;
	char    *retval, *p;

	str = g_string_new (exec);

	while ((p = strstr (str->str, "%"))) {
		switch (p [1]) {
		case '%':
			g_string_erase (str, p - str->str, 1);
			break;
		case 'U':
		case 'F':
		case 'N':
		case 'D':
		case 'f':
		case 'u':
		case 'd':
		case 'n':
		case 'm':
		case 'i':
		case 'c':
		case 'k':
		case 'v':
			g_string_erase (str, p - str->str, 2);
			break;
		default:
			break;
		}
	}

	retval = str->str;
	g_string_free (str, FALSE);

	return retval;
}

static void
program_list_selection_changed (GtkTreeSelection *selection,
				PanelRunDialog   *dialog)
{
	GtkTreeModel *filter_model;
	GtkTreeModel *child_model;
	GtkTreeIter   iter;
	GtkTreeIter   filter_iter;
	char         *temp;
	char         *path;
	gboolean      terminal;
	GKeyFile     *key_file;
	GtkWidget    *entry;
	GIcon        *icon = NULL;

	if (!gtk_tree_selection_get_selected (selection, &filter_model,
					      &filter_iter))
		return;

	gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (filter_model),
							  &iter, &filter_iter);

	path = NULL;
	child_model = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (filter_model));
	gtk_tree_model_get (child_model, &iter,
			    COLUMN_PATH, &path,
			    -1);

	if (!path)
		return;

	key_file = g_key_file_new ();

	if (!g_key_file_load_from_file (key_file, path,
					G_KEY_FILE_NONE, NULL)) {
		g_key_file_free (key_file);
		g_free (path);
		return;
	}

	dialog->use_program_list = TRUE;
	g_free (dialog->desktop_path);
	dialog->desktop_path = g_strdup (path);
	g_clear_pointer (&dialog->item_name, g_free);

	/* Order is important here. We have to set the text first so that the
	 * drag source is enabled, otherwise the drag icon can't be set by
	 * panel_run_dialog_set_icon.
	 */
	entry = gtk_bin_get_child (GTK_BIN (dialog->combobox));
	temp = panel_key_file_get_string (key_file, "Exec");
	if (temp) {
		char *stripped = remove_parameters (temp);
		gtk_entry_set_text (GTK_ENTRY (entry), stripped);
		g_free (stripped);
	} else {
		temp = panel_key_file_get_string (key_file, "URL");
		gtk_entry_set_text (GTK_ENTRY (entry), sure_string (temp));
	}
	g_free (temp);

	temp = panel_key_file_get_locale_string (key_file, "Icon");
	icon = panel_gicon_from_icon_name (temp);
	panel_run_dialog_set_icon (dialog, icon, FALSE);
	if (icon != NULL)
		g_object_unref (icon);
	g_free (temp);

	temp = panel_key_file_get_locale_string (key_file, "Comment");
	/* FIXME: if sure_string () == "", we should display "Will run..." as in entry_changed() */
	gtk_label_set_text (GTK_LABEL (dialog->program_label),
			    sure_string (temp));
	g_free (temp);

	terminal = panel_key_file_get_boolean (key_file, "Terminal", FALSE);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dialog->terminal_checkbox),
				      terminal);

	g_key_file_free (key_file);

	g_free (path);
}

static void
program_list_selection_activated (GtkTreeView       *view,
				  GtkTreePath       *path,
				  GtkTreeViewColumn *column,
				  PanelRunDialog    *dialog)
{
	GtkTreeSelection *selection;

	/* update the entry with the info from the selection */
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (dialog->program_list));
	program_list_selection_changed (selection, dialog);

	/* now launch the command */
	gtk_dialog_response (GTK_DIALOG (dialog->run_dialog), GTK_RESPONSE_OK);
}

static void
panel_run_dialog_setup_program_list (PanelRunDialog *dialog,
				     GtkBuilder     *gui)
{
	dialog->program_list = PANEL_GTK_BUILDER_GET (gui, "program_list");
	dialog->program_list_box = PANEL_GTK_BUILDER_GET (gui, "program_list_box");
	dialog->program_label = PANEL_GTK_BUILDER_GET (gui, "program_label");
	dialog->main_box = PANEL_GTK_BUILDER_GET (gui, "main_box");

	gtk_widget_set_can_focus (dialog->program_label, FALSE);

	/* Ref the box so it doesn't get destroyed when it is
	 * removed from the visible area of the dialog box.
	 */
	g_object_ref (dialog->program_list_box);

	if (panel_profile_get_enable_program_list ()) {
		GtkTreeSelection *selection =
			gtk_tree_view_get_selection (GTK_TREE_VIEW (dialog->program_list));

		gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	        g_signal_connect (selection, "changed",
				  G_CALLBACK (program_list_selection_changed),
				  dialog);

	        g_signal_connect (dialog->program_list, "row-activated",
				  G_CALLBACK (program_list_selection_activated),
				  dialog);

		/* start loading the list of applications */
		dialog->add_items_idle_id =
			g_idle_add_full (G_PRIORITY_LOW, (GSourceFunc) panel_run_dialog_add_items_idle,
					 dialog, NULL);
	}
}

static void
panel_run_dialog_update_content (PanelRunDialog *dialog,
				 gboolean        show_list)
{

	if (!panel_profile_get_enable_program_list ()) {
		GtkWidget *parent;

		parent = gtk_widget_get_parent (dialog->list_expander);
		if (parent)
			gtk_container_remove (GTK_CONTAINER (parent),
					      dialog->list_expander);

		gtk_window_set_resizable (GTK_WINDOW (dialog->run_dialog), FALSE);
                gtk_widget_grab_focus (dialog->combobox);

	} else {

        /* the following two conditions occur, when the user clicks the expander in the dialog
         * if the list is closed and the user wants to see it */
        if (show_list && gtk_expander_get_expanded (GTK_EXPANDER (dialog->list_expander))) {

            /* open the expander, this shows the list */
            gtk_expander_set_expanded (GTK_EXPANDER (dialog->list_expander), TRUE);

            gtk_window_resize (GTK_WINDOW (dialog->run_dialog), 100, 300);
            gtk_window_set_resizable (GTK_WINDOW (dialog->run_dialog), TRUE);
            gtk_widget_grab_focus (dialog->program_list);

        /* if the list is open and the user wants to close it */
        } else if (!show_list && !gtk_expander_get_expanded (GTK_EXPANDER (dialog->list_expander))) {

            /* close the expander, this hides the list */
            gtk_expander_set_expanded (GTK_EXPANDER (dialog->list_expander), FALSE);

            gtk_window_set_resizable (GTK_WINDOW (dialog->run_dialog), FALSE);
            gtk_widget_grab_focus (dialog->combobox);
        }

        /* the following two conditions occur, when the user changes the expander setting in GSettings */
        if (show_list && !gtk_expander_get_expanded (GTK_EXPANDER (dialog->list_expander))) {

            /* open the expander, this shows the list */
            gtk_expander_set_expanded (GTK_EXPANDER (dialog->list_expander), TRUE);

        /* if the list is open and the user wants to close it */
        } else if (!show_list && gtk_expander_get_expanded (GTK_EXPANDER (dialog->list_expander))) {

            /* close the expander, this hides the list */
            gtk_expander_set_expanded (GTK_EXPANDER (dialog->list_expander), FALSE);
        }
    }
}

static void
panel_run_dialog_content_notify (GSettings      *settings,
				 gchar          *key,
				 PanelRunDialog *dialog)
{
	panel_run_dialog_update_content (dialog, g_settings_get_boolean (settings, key));
}

static void
list_expander_toggled (GtkExpander    *expander,
		       GParamSpec     *pspec,
		       PanelRunDialog *dialog)
{
	panel_profile_set_show_program_list (gtk_expander_get_expanded (expander));
}

static void
panel_run_dialog_setup_list_expander (PanelRunDialog *dialog,
				      GtkBuilder     *gui)
{
	dialog->list_expander = PANEL_GTK_BUILDER_GET (gui, "list_expander");

	if (panel_profile_get_enable_program_list ()) {
		gtk_expander_set_expanded (GTK_EXPANDER (dialog->list_expander),
					   panel_profile_get_show_program_list ());

		if ( ! panel_profile_is_writable_show_program_list ())
			gtk_widget_set_sensitive (dialog->list_expander, FALSE);

	        g_signal_connect (dialog->list_expander, "notify::expanded",
				  G_CALLBACK (list_expander_toggled),
				  dialog);

		g_signal_connect (dialog->settings,
				  "changed::" PANEL_RUN_SHOW_PROGRAM_LIST_KEY,
				  G_CALLBACK (panel_run_dialog_content_notify),
				  dialog);
	}
}

static void
file_button_browse_response (GtkWidget      *chooser,
			     gint            response,
			     PanelRunDialog *dialog)
{

	if (response == GTK_RESPONSE_OK) {
		char *file = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));
		panel_run_dialog_append_file (dialog, file);
		g_free (file);
	}

	gtk_widget_destroy (chooser);

	gtk_widget_grab_focus (dialog->combobox);
}

static void
file_button_clicked (GtkButton      *button,
		     PanelRunDialog *dialog)
{
	GtkWidget *chooser;

	chooser = panel_file_chooser_dialog_new (_("Choose a file to append to the command..."),
						 GTK_WINDOW (dialog->run_dialog),
						 GTK_FILE_CHOOSER_ACTION_OPEN,
						 "process-stop", GTK_RESPONSE_CANCEL,
						 "gtk-ok", GTK_RESPONSE_OK,
						 NULL);

	gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser),
					     g_get_home_dir ());

	gtk_dialog_set_default_response (GTK_DIALOG (chooser), GTK_RESPONSE_OK);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (chooser), TRUE);

	g_signal_connect (chooser, "response",
			  G_CALLBACK (file_button_browse_response), dialog);

	gtk_window_present (GTK_WINDOW (chooser));
}

static void
panel_run_dialog_setup_file_button (PanelRunDialog *dialog,
				    GtkBuilder     *gui)
{
	dialog->file_button = PANEL_GTK_BUILDER_GET (gui, "file_button");

        g_signal_connect (dialog->file_button, "clicked",
			  G_CALLBACK (file_button_clicked),
			  dialog);
}

static GList *
fill_files_from (const char *dirname,
		 const char *dirprefix,
		 char        prefix,
		 GList      *existing_items)
{
	GList         *list;
	DIR           *dir;
	struct dirent *dent;

	list = NULL;
	dir = opendir (dirname);

	if (!dir)
		return list;

	while ((dent = readdir (dir))) {
		char       *file;
		char       *item;
		const char *suffix;

		if (dent->d_name [0] != prefix)
			continue;

		file = g_build_filename (dirname, dent->d_name, NULL);

		suffix = NULL;
		if (
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
		/* don't use g_file_test at first so we don't stat() */
		    dent->d_type == DT_DIR ||
		    (dent->d_type == DT_LNK &&
		     g_file_test (file, G_FILE_TEST_IS_DIR))
#else
		    g_file_test (file, G_FILE_TEST_IS_DIR)
#endif
		   )
			suffix = "/";

		g_free (file);

		item = g_build_filename (dirprefix, dent->d_name, suffix, NULL);

		list = g_list_prepend (list, item);
	}

	closedir (dir);

	return list;
}

static GList *
fill_possible_executables (void)
{
	GList         *list;
	const char    *path;
	char         **pathv;
	int            i;

	list = NULL;
	path = g_getenv ("PATH");

	if (!path || !path [0])
		return list;

	pathv = g_strsplit (path, ":", 0);

	for (i = 0; pathv [i]; i++) {
		const char *file;
		GDir       *dir;

		dir = g_dir_open (pathv [i], 0, NULL);

		if (!dir)
			continue;

		while ((file = g_dir_read_name (dir))) {
			list = g_list_prepend (list, g_build_filename (pathv [i], file, NULL));
		}

		g_dir_close (dir);
	}

	g_strfreev (pathv);

	return list;
}

static GList *
fill_executables (GList *possible_executables,
		  GList *existing_items,
		  char   prefix)
{
	GList *list;
	GList *l;

	list = NULL;

	for (l = possible_executables; l; l = l->next) {
		const char *filename;
		char       *basename;

		filename = l->data;
		basename = g_path_get_basename (filename);

		if (basename [0] == prefix &&
		    g_file_test (filename, G_FILE_TEST_IS_REGULAR) &&
		    g_file_test (filename, G_FILE_TEST_IS_EXECUTABLE)) {

			if (g_list_find_custom (existing_items, basename,
						(GCompareFunc) strcmp)) {
				g_free (basename);
				return NULL;
			}

			list = g_list_prepend (list, basename);
		 } else {
			g_free (basename);
		 }
	}

	return list;
}

static GtkTreeModel *
create_completion_model (GList *list)
{
	GtkListStore *store;
	GtkTreeIter   iter;
	GList        *l;

	store = gtk_list_store_new (1, G_TYPE_STRING);
	for (l = list; l; l = l->next)
	{
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, l->data, -1);
	}

	return GTK_TREE_MODEL (store);
}

static void
completion_add_items (GtkEntryCompletion *completion, GList *list)
{
	GtkTreeModel          *completion_model;

	completion_model = create_completion_model (list);
	gtk_entry_completion_set_model (completion, completion_model);
	g_object_unref (completion_model);
}

static void
panel_run_dialog_update_completion (PanelRunDialog *dialog,
				    const char     *text)
{
	GList *list;
	GList *executables;
	char   prefix;
	char  *buf;
	char  *dirname;
	char  *dirprefix;
	char  *key;

	g_assert (text != NULL && *text != '\0' && !g_ascii_isspace (*text));

	list = NULL;
	executables = NULL;

	buf = g_path_get_basename (text);
	prefix = buf[0];
	g_free (buf);
	if (prefix == '/' || prefix == '.')
		return;

	if (text [0] == '/') {
		/* complete against absolute path */
		dirname = g_path_get_dirname (text);
		dirprefix = g_strdup (dirname);
	} else {
		/* complete against relative path and executable name */
		if (!strchr (text, '/')) {
			executables = fill_executables (dialog->possible_executables,
							dialog->completion_items,
							text [0]);
			dirprefix = g_strdup ("");
		} else {
			dirprefix = g_path_get_dirname (text);
		}

		dirname = g_build_filename (g_get_home_dir (), dirprefix, NULL);
	}

	key = g_strdup_printf ("%s%c%c", dirprefix, G_DIR_SEPARATOR, prefix);

	if (!g_hash_table_lookup (dialog->dir_hash, key)) {
		g_hash_table_insert (dialog->dir_hash, key, dialog);

		list = fill_files_from (dirname, dirprefix, prefix,
					dialog->completion_items);
	} else {
		g_free (key);
	}

	list = g_list_concat (list, executables);

	g_free (dirname);
	g_free (dirprefix);

	if (list == NULL)
		return;

	completion_add_items (dialog->completion, list);
	dialog->completion_items = g_list_concat (dialog->completion_items,
						  list);
}

static gboolean
entry_event (GtkEditable    *entry,
	     GdkEventKey    *event,
	     PanelRunDialog *dialog)
{
	char             *prefix;
	char             *nospace_prefix;
	char             *temp;
	int               pos, tmp;

	if (event->type != GDK_KEY_PRESS)
		return FALSE;

	/* if user typed something we're not using the list anymore
	   (but only if it is enabled anyway)
	*/
	dialog->use_program_list = FALSE;
	if (panel_profile_get_enable_program_list ()) {
		GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (dialog->program_list));
		gtk_tree_selection_unselect_all (selection);
	}

	if (!panel_profile_get_enable_autocompletion ())
		return FALSE;

	/* tab completion */
	if (event->keyval == GDK_KEY_Tab) {
		gtk_editable_get_selection_bounds (entry, &pos, &tmp);

		if (dialog->completion_started &&
		    pos != tmp &&
		    pos != 1 &&
		    ((size_t) tmp) == strlen (gtk_entry_get_text (GTK_ENTRY (entry)))) {
	    		gtk_editable_select_region (entry, 0, 0);
			gtk_editable_set_position (entry, -1);

			return TRUE;
		}
	} else if (event->length > 0) {

		gtk_editable_get_selection_bounds (entry, &pos, &tmp);

		if (dialog->completion_started &&
		    pos != tmp &&
		    pos != 0 &&
		    ((size_t) tmp) == strlen (gtk_entry_get_text (GTK_ENTRY (entry)))) {
			temp = gtk_editable_get_chars (entry, 0, pos);
			prefix = g_strconcat (temp, event->string, NULL);
			g_free (temp);
		} else if (pos == tmp &&
			   ((size_t) tmp) == strlen (gtk_entry_get_text (GTK_ENTRY (entry)))) {
			prefix = g_strconcat (gtk_entry_get_text (GTK_ENTRY (entry)),
					      event->string, NULL);
		} else {
			return FALSE;
		}

		nospace_prefix = prefix;
		while (*nospace_prefix != '\0' &&
		       g_ascii_isspace (*nospace_prefix))
			nospace_prefix++;
		if (*nospace_prefix == '\0')
			return FALSE;

		panel_run_dialog_update_completion (dialog, nospace_prefix);

		if (!dialog->completion) {
			g_free (prefix);
			return FALSE;
		}

		g_free (prefix);
	}

	return FALSE;
}

static void
combobox_changed (GtkComboBox    *combobox,
		  PanelRunDialog *dialog)
{
	char *text = g_strdup (panel_run_dialog_get_combo_text (dialog));
	char *start = text;

	while (*start != '\0' && g_ascii_isspace (*start))
		start++;

	/* update item name to use for dnd */
	if (!dialog->use_program_list) {
		if (dialog->desktop_path) {
			g_free (dialog->desktop_path);
			dialog->desktop_path = NULL;
		}
		if (dialog->item_name) {
			g_free (dialog->item_name);
			dialog->item_name = NULL;
		}
	}

	/* desensitize run button if no text entered */
	if (!start [0]) {
		g_free (text);

		gtk_widget_set_sensitive (dialog->run_button, FALSE);
		gtk_drag_source_unset (dialog->run_dialog);

		if (panel_profile_get_enable_program_list ())
			gtk_label_set_text (GTK_LABEL (dialog->program_label),
					    _("Select an application to view its description."));

		panel_run_dialog_set_default_icon (dialog, FALSE);

		if (dialog->find_command_idle_id) {
			g_source_remove (dialog->find_command_idle_id);
			dialog->find_command_idle_id = 0;
		}

		if (panel_profile_get_enable_program_list ()) {
			GtkTreeIter   iter;
			GtkTreePath  *path;
			GtkTreeModel *model;
			gint          i = 0;
			gboolean      valid = FALSE;

			g_hash_table_remove_all (accelerator_keys_to_tree_iter_map);
			model = GTK_TREE_MODEL (dialog->program_list_store);
			for (valid = gtk_tree_model_get_iter_first (model, &iter); valid; valid = gtk_tree_model_iter_next (model, &iter)) {
				if (i < G_N_ELEMENTS (accelerator_key_mapping)) {
					gtk_list_store_set (GTK_LIST_STORE (GTK_TREE_MODEL (dialog->program_list_store)), &iter,
							    COLUMN_VISIBLE, TRUE,
							    COLUMN_ACCELERATOR_MASK, (gint)accelerator_key_mapping[i].modifier,
							    COLUMN_ACCELERATOR_KEY_VALUE, accelerator_key_mapping[i].key_id,
							    -1);
					g_hash_table_insert (accelerator_keys_to_tree_iter_map, GUINT_TO_POINTER (accelerator_key_mapping[i].key_id), GINT_TO_POINTER(i));
					i++;
				} else {
					gtk_list_store_set (GTK_LIST_STORE (GTK_TREE_MODEL (dialog->program_list_store)), &iter,
							    COLUMN_VISIBLE, TRUE,
							    COLUMN_ACCELERATOR_MASK, (gint)GDK_MOD1_MASK,
							    COLUMN_ACCELERATOR_KEY_VALUE, 0,
							    -1);
				}
			}

			path = gtk_tree_path_new_first ();
			if (gtk_tree_model_get_iter (gtk_tree_view_get_model (GTK_TREE_VIEW (dialog->program_list)),
						     &iter, path))
				gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (dialog->program_list),
							      path, NULL,
							      FALSE, 0, 0);
			gtk_tree_path_free (path);
		}

		return;
	}

	gtk_widget_set_sensitive (dialog->run_button, TRUE);
	gtk_drag_source_set (dialog->run_dialog,
			     GDK_BUTTON1_MASK,
			     NULL, 0,
			     GDK_ACTION_COPY);
	gtk_drag_source_add_uri_targets (dialog->run_dialog);

	if (panel_profile_get_enable_program_list () &&
	    !dialog->use_program_list) {
		char *msg = g_strdup_printf (_("Will run command: '%s'"),
		                             start);
		gtk_label_set_text (GTK_LABEL (dialog->program_label), msg);
		g_free (msg);
	}

	/* look up icon for the command */
	if (panel_profile_get_enable_program_list () &&
	    !dialog->use_program_list &&
	    !dialog->find_command_idle_id)
		dialog->find_command_idle_id =
			g_idle_add_full (G_PRIORITY_LOW,
					 (GSourceFunc) panel_run_dialog_find_command_idle,
					 dialog, NULL);

	g_free (text);
}

static void
entry_drag_data_received (GtkEditable      *entry,
			  GdkDragContext   *context,
			  gint              x,
			  gint              y,
			  GtkSelectionData *selection_data,
			  guint             info,
			  guint32           time,
			  PanelRunDialog   *dialog)
{
	char **uris;
	char  *file;
	int    i;

        if (gtk_selection_data_get_format (selection_data) != 8 || gtk_selection_data_get_length (selection_data) == 0) {
        	g_warning (_("URI list dropped on run dialog had wrong format (%d) or length (%d)\n"),
			   gtk_selection_data_get_format (selection_data),
			   gtk_selection_data_get_length (selection_data));
		return;
        }

	uris = g_uri_list_extract_uris ((const char *)gtk_selection_data_get_data (selection_data));

	if (!uris) {
		gtk_drag_finish (context, FALSE, FALSE, time);
		return;
	}

	for (i = 0; uris [i]; i++) {
		if (!uris [i] || !uris [i][0])
			continue;

		file = g_filename_from_uri (uris [i], NULL, NULL);

		/* FIXME: I assume the file is in utf8 encoding if coming from a URI? */
		if (file) {
			panel_run_dialog_append_file_utf8 (dialog, file);
			g_free (file);
		} else
			panel_run_dialog_append_file_utf8 (dialog, uris [i]);
	}

	g_strfreev (uris);
	gtk_drag_finish (context, TRUE, FALSE, time);
}

static void
panel_run_dialog_setup_entry (PanelRunDialog *dialog,
			      GtkBuilder     *gui)
{
	int                    width_request;
	GtkWidget             *entry;

	dialog->combobox = PANEL_GTK_BUILDER_GET (gui, "comboboxentry");
	dialog->possible_executables = fill_possible_executables ();
	dialog->dir_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	entry = gtk_bin_get_child (GTK_BIN (dialog->combobox));
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);

	dialog->completion = gtk_entry_completion_new ();
	gtk_entry_completion_set_inline_completion (dialog->completion, TRUE);
	gtk_entry_completion_set_popup_completion (dialog->completion, FALSE);
	gtk_entry_set_completion (GTK_ENTRY (entry), dialog->completion);
	gtk_entry_completion_set_text_column (dialog->completion, 0);

	gtk_combo_box_set_model (GTK_COMBO_BOX (dialog->combobox),
				 _panel_run_get_recent_programs_list (dialog));
	gtk_combo_box_set_entry_text_column
		(GTK_COMBO_BOX (dialog->combobox), 0);

        /* 1/4 the width of the first monitor should be a good value */
	width_request = panel_multimonitor_width (0) / 4;
	g_object_set (G_OBJECT (dialog->combobox),
		      "width_request", width_request,
		      NULL);

        g_signal_connect (entry, "key-press-event",
			  G_CALLBACK (entry_event), dialog);

        dialog->changed_id = g_signal_connect (dialog->combobox, "changed",
					       G_CALLBACK (combobox_changed),
					       dialog);

	gtk_drag_dest_unset (dialog->combobox);

	gtk_drag_dest_set (dialog->combobox,
			   GTK_DEST_DEFAULT_MOTION|GTK_DEST_DEFAULT_HIGHLIGHT,
			   NULL, 0,
			   GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets (dialog->combobox);

	g_signal_connect (dialog->combobox, "drag-data-received",
			  G_CALLBACK (entry_drag_data_received), dialog);
}

static char *
panel_run_dialog_create_desktop_file (PanelRunDialog *dialog)
{
	GKeyFile *key_file;
	gboolean  exec = FALSE;
	char     *text;
	char     *name;
	char     *disk;
	char     *scheme;
	char     *save_uri;

	text = g_strdup (panel_run_dialog_get_combo_text (dialog));

	if (!text || !text [0]) {
		g_free (text);
		return NULL;
	}

	key_file = panel_key_file_new_desktop ();
	disk = g_locale_from_utf8 (text, -1, NULL, NULL, NULL);

	scheme = g_uri_parse_scheme (disk);
	/* if it's an absolute path or not a URI, it's possibly an executable */
	if (g_path_is_absolute (disk) || !scheme)
		exec = command_is_executable (disk, NULL, NULL);
	g_free (scheme);

	if (exec) {
		panel_key_file_set_string (key_file, "Type", "Application");
		panel_key_file_set_string (key_file, "Exec", text);
		name = g_strdup (text);
	} else {
		GFile *file;
		char  *uri;

		file = panel_util_get_file_optional_homedir (disk);
		uri = g_file_get_uri (file);
		g_object_unref (file);

		panel_key_file_set_string (key_file, "Type", "Link");
		panel_key_file_set_string (key_file, "URL", uri);
		name = uri;
	}

	g_free (disk);

	panel_key_file_set_locale_string (key_file, "Name",
					  (dialog->item_name) ?
					  dialog->item_name : text);

	panel_key_file_set_boolean (key_file, "Terminal",
				    gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->terminal_checkbox)));

	if (dialog->icon) {
		gchar *icon_path = g_icon_to_string (dialog->icon);
		panel_key_file_set_locale_string (key_file, "Icon", icon_path);
		g_free (icon_path);
	}
	else
		panel_key_file_set_locale_string (key_file, "Icon",
						  PANEL_ICON_LAUNCHER);

	save_uri = panel_make_unique_desktop_uri (g_get_tmp_dir (), name);
	disk = g_filename_from_uri (save_uri, NULL, NULL);

	if (!disk || !panel_key_file_to_file (key_file, disk, NULL))
		g_clear_pointer (&save_uri, g_free);

	g_key_file_free (key_file);
	g_free (disk);
	g_free (name);
	g_free (text);

	return save_uri;
}

static void
pixmap_drag_data_get (GtkWidget          *run_dialog,
	  	      GdkDragContext     *context,
		      GtkSelectionData   *selection_data,
		      guint               info,
		      guint               time,
		      PanelRunDialog     *dialog)
{
	char *uri;

	if (dialog->use_program_list && dialog->desktop_path)
		uri = g_filename_to_uri (dialog->desktop_path, NULL, NULL);
	else
		uri = panel_run_dialog_create_desktop_file (dialog);

	if (uri) {
		gtk_selection_data_set (selection_data,
					gtk_selection_data_get_target (selection_data), 8,
					(unsigned char *) uri, strlen (uri));
		g_free (uri);
	}
}

static void
panel_run_dialog_style_updated (GtkWidget *widget,
				PanelRunDialog *dialog)
{
  if (dialog->icon) {
	  GIcon *icon;

	  icon = g_object_ref (dialog->icon);
	  panel_run_dialog_set_icon (dialog, icon, TRUE);
	  g_object_unref (icon);
  }
}

static void
panel_run_dialog_screen_changed (GtkWidget      *widget,
				 GdkScreen      *prev_screen,
				 PanelRunDialog *dialog)
{
  if (dialog->icon) {
	  GIcon *icon;

	  icon = g_object_ref (dialog->icon);
	  panel_run_dialog_set_icon (dialog, icon, TRUE);
	  g_object_unref (icon);;
  }
}

static void
panel_run_dialog_setup_pixmap (PanelRunDialog *dialog,
			       GtkBuilder     *gui)
{
	dialog->pixmap = PANEL_GTK_BUILDER_GET (gui, "icon_pixmap");

	g_signal_connect (dialog->pixmap, "style-updated",
			  G_CALLBACK (panel_run_dialog_style_updated),
			  dialog);
	g_signal_connect (dialog->pixmap, "screen-changed",
			  G_CALLBACK (panel_run_dialog_screen_changed),
			  dialog);

	g_signal_connect (dialog->run_dialog, "drag-data-get",
			  G_CALLBACK (pixmap_drag_data_get),
			  dialog);
}

/* this runs after entry_event() */
static gboolean
key_press_event (GtkWidget    *run_dialog,
				GdkEventKey    *event,
				PanelRunDialog *dialog)
{

	/* allow only key presses */
	if (event->type != GDK_KEY_PRESS)
		return FALSE;

	/* If the program list is enabled and open and the user presses the F6 key
	 * the focus should jump between GtkComboBoxText and the program list  */
	if (panel_profile_get_enable_program_list () && panel_profile_get_show_program_list () && event->keyval == GDK_KEY_F6) {

		/* jump to the program list from anywhere */
		if (!gtk_widget_has_focus (dialog->program_list)) {
			gtk_widget_grab_focus (dialog->program_list);
		/* on the program list, jump to the entry box */
		} else {
			gtk_widget_grab_focus (dialog->combobox);
		}

		return TRUE;
	}

	return FALSE;
}

static void
panel_run_dialog_accelerator_key_pressed (GtkAccelGroup   *accel_group,
					  GObject         *acceleratable,
					  guint            keyval,
					  GdkModifierType  modifier,
					  PanelRunDialog  *dialog)
{
	GtkTreePath *path;
	gpointer index_of_entry;
	gboolean found = g_hash_table_lookup_extended (accelerator_keys_to_tree_iter_map, GUINT_TO_POINTER(keyval), NULL, &index_of_entry);
	if (!found) return;
	path = gtk_tree_path_new_from_indices(GPOINTER_TO_INT(index_of_entry), -1);
	gtk_tree_view_set_cursor (GTK_TREE_VIEW (dialog->program_list), path, NULL, FALSE);
	gtk_widget_grab_focus (dialog->program_list);
}

static PanelRunDialog *
panel_run_dialog_new (GdkScreen  *screen,
		      GtkBuilder *gui,
		      guint32    activate_time)
{
	PanelRunDialog *dialog;

	dialog = g_new0 (PanelRunDialog, 1);

	dialog->run_dialog = PANEL_GTK_BUILDER_GET (gui, "panel_run_dialog");

	g_signal_connect_swapped (dialog->run_dialog, "response",
				  G_CALLBACK (panel_run_dialog_response), dialog);

	g_signal_connect_swapped (dialog->run_dialog, "destroy",
				  G_CALLBACK (panel_run_dialog_destroy), dialog);

	GtkAccelGroup* accel_group = gtk_accel_group_new ();
	gtk_window_add_accel_group (GTK_WINDOW(dialog->run_dialog), accel_group);
	g_object_unref (accel_group);
	for (gint i = 0; i < G_N_ELEMENTS (accelerator_key_mapping); i++)
	{
		GClosure *closure_key = g_cclosure_new (
			G_CALLBACK (panel_run_dialog_accelerator_key_pressed), dialog, NULL);
		gtk_accel_group_connect (accel_group,
					 accelerator_key_mapping[i].key_id,
					 accelerator_key_mapping[i].modifier,
					 0,
					 closure_key);
	}

	dialog->run_button = PANEL_GTK_BUILDER_GET (gui, "run_button");
	dialog->terminal_checkbox = PANEL_GTK_BUILDER_GET (gui, "terminal_checkbox");

	dialog->settings = g_settings_new (PANEL_RUN_SCHEMA);

	panel_run_dialog_setup_pixmap        (dialog, gui);
	panel_run_dialog_setup_entry         (dialog, gui);
	panel_run_dialog_setup_file_button   (dialog, gui);
	panel_run_dialog_setup_program_list  (dialog, gui);
	panel_run_dialog_setup_list_expander (dialog, gui);

    /* key press event signal for the whole dialog */
	g_signal_connect (dialog->run_dialog, "key-press-event",
			  G_CALLBACK (key_press_event), dialog);

	panel_run_dialog_set_default_icon    (dialog, FALSE);

	panel_run_dialog_update_content (dialog, panel_profile_get_show_program_list ());

	gtk_widget_set_sensitive (dialog->run_button, FALSE);

	gtk_dialog_set_default_response (GTK_DIALOG (dialog->run_dialog),
					 GTK_RESPONSE_OK);

	gtk_window_set_screen (GTK_WINDOW (dialog->run_dialog), screen);

	gtk_widget_grab_focus (dialog->combobox);
	gtk_widget_realize (dialog->run_dialog);
#ifdef HAVE_X11
	if (is_using_x11 ())
		gdk_x11_window_set_user_time (gtk_widget_get_window (dialog->run_dialog),
					      activate_time);
#endif
	gtk_widget_show (dialog->run_dialog);

	return dialog;
}

static void
panel_run_dialog_disconnect_pixmap (PanelRunDialog *dialog)
{
	g_signal_handlers_disconnect_by_func (dialog->pixmap,
					      G_CALLBACK (panel_run_dialog_style_updated),
					      dialog);
	g_signal_handlers_disconnect_by_func (dialog->pixmap,
			                      G_CALLBACK (panel_run_dialog_screen_changed),
			                      dialog);
}

static void
panel_run_dialog_static_dialog_destroyed (PanelRunDialog *dialog)
{
	/* just reset the static dialog to NULL for next time */
	static_dialog = NULL;
}

void
panel_run_dialog_present (GdkScreen *screen,
			  guint32    activate_time)
{
	GtkBuilder *gui;
	accelerator_keys_to_tree_iter_map = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);

	if (panel_lockdown_get_disable_command_line ())
		return;

	if (static_dialog) {
		gtk_window_set_screen (GTK_WINDOW (static_dialog->run_dialog), screen);
		gtk_window_present_with_time (GTK_WINDOW (static_dialog->run_dialog),
					      activate_time);
		gtk_widget_grab_focus (static_dialog->combobox);
		return;
	}

	gui = gtk_builder_new ();
	gtk_builder_set_translation_domain (gui, GETTEXT_PACKAGE);
	gtk_builder_add_from_resource (gui,
	                               PANEL_RESOURCE_PATH "panel-run-dialog.ui",
	                               NULL);

	static_dialog = panel_run_dialog_new (screen, gui, activate_time);

	g_signal_connect_swapped (static_dialog->run_dialog, "destroy",
				  G_CALLBACK (panel_run_dialog_static_dialog_destroyed),
				  static_dialog);

	gtk_window_present_with_time (GTK_WINDOW (static_dialog->run_dialog),
				      activate_time);

	g_object_unref (gui);
}

void
panel_run_dialog_quit_on_destroy (void)
{
	g_signal_connect(static_dialog->run_dialog, "destroy",
			 G_CALLBACK(gtk_main_quit), NULL);
}
