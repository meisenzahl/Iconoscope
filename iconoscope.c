#include <locale.h>
#include <cairo.h>
#include <gtk/gtk.h>

#include "common.h"
#include "slo_timers.h"
#include "gtk_utils.c"

struct app_t app;
void app_set_selected_theme (struct app_t *app, const char *theme_name, const char *selected_icon);

#include "icon_view.h"

// TODO: Support svgz extension (at least Kdenlive uses it). Because GtkImage
// doesn't understand them (yet), we may need to call gzip.
// NOTE: The order here defines the priority, from highest to lowest.
#define VALID_EXTENSIONS \
    EXTENSION(EXT_SVG, ".svg") \
    EXTENSION(EXT_SYMBOLIC_PNG, ".symbolic.png") \
    EXTENSION(EXT_PNG, ".png") \
    EXTENSION(EXT_XPM, ".xpm")

enum valid_extensions {
#define EXTENSION(name,str) name,
    VALID_EXTENSIONS
#undef EXTENSION
    NUM_EXTENSIONS
};

struct icon_theme_t {
    mem_pool_t pool;

    char *name;
    uint32_t num_dirs;
    char **dirs;
    char *index_file;
    char *dir_name;

    GHashTable *icon_names;

    struct icon_theme_t *next;
};

struct app_t {
    // App state
    struct icon_theme_t *selected_theme;
    bool all_theme_selected;
    char *selected_icon;
    dvec4 bg_color;

    GtkWidget *icon_list;
    GtkWidget *search_entry;
    GtkWidget *icon_view_widget;
    GtkWidget *theme_selector;

    // Special (fake) "All" theme
    mem_pool_t all_icon_names_pool;
    GTree *all_icon_names;
    GtkWidget *all_icon_names_widget;
    const char *all_icon_names_first;
    struct fake_list_box_t fake_list_box;

    // Linked list head for all themes
    struct icon_theme_t *themes;

    // Icon view for the selected icon
    mem_pool_t icon_view_pool;
    struct icon_view_t icon_view;

    const char* valid_extensions[NUM_EXTENSIONS];
};

#include "icon_view.c"

static inline
char* consume_line (char *c)
{
    while (*c && *c != '\n') {
           c++;
    }

    if (*c) {
        c++;
    }

    return c;
}

static inline
char* consume_spaces (char *c)
{
    while (is_space(c)) {
           c++;
    }
    return c;
}

// INI or desktop file format parser.
//
// The idea of the following functions is to allow seeking through a INI format
// file without making any allocations. All returned strings point into the
// original file string and are NOT null terminated. The following code shows
// how to use them by printing back the input file:
//
//    char *theme_index = full_file_read (&pool, str_data(&path));
//    char *c = theme_index;
//    while (*c) {
//        char *section_name;
//        uint32_t section_name_len;
//        c = seek_next_section (c, &section_name, &section_name_len);
//        printf ("[%.*s]\n", section_name_len, section_name);
//
//        while ((c = consume_ignored_lines (c)) && !is_end_of_section(c)) {
//            char *key, *value;
//            uint32_t key_len, value_len;
//            c = seek_next_key_value (c, &key, &key_len, &value, &value_len);
//            printf ("%.*s=%.*s\n", key_len, key, value_len, value);
//        }
//        printf ("\n");
//    }

char *seek_next_section (char *c, char **section_name, uint32_t *section_name_len)
{
    while (*c && *c != '[') {
        c = consume_line (c);
    }

    if (*c == '\0') {
        // NOTE: There are no more sections
        return c;
    }

    c++;

    uint32_t len = 0;
    while (*(c + len) && *(c + len) != ']') {
        len++;
    }

    if (*(c + len) == '\0' || !is_end_of_line_or_file(c + len + 1)) {
        printf ("Syntax error in INI/desktop file.\n");
        return c;
    }

    if (section_name != NULL) {
        *section_name = c;
    }
    if (section_name_len != NULL) {
        *section_name_len = len;
    }
    return consume_line (c);
}

char *seek_next_key_value (char *c,
                           char **key, uint32_t *key_len,
                           char **value, uint32_t *value_len)
{
    if (*c == '[') {
        // NOTE: End of section
        return c;
    }

    uint32_t len = 0;
    while (*(c + len) && *(c + len) != '=' && !is_space (c)) {
        len++;
    }

    *key = c;
    *key_len = len;

    c = consume_spaces (c+len);

    if (*c != '=') {
        printf ("Syntax error in INI/desktop file.\n");
        return c;
    }

    c++;
    c = consume_spaces (c);

    len = 0;
    while (*(c + len) && !is_end_of_line_or_file (c + len)) {
        len++;
    }

    *value = c;
    *value_len = len;
    return consume_line (c);
}

char *consume_ignored_lines (char *c)
{
    while (*c && (is_end_of_line(c) || *c == ';' || *c == '#')) {
        c = consume_line (c);
    }
    return c;
}

char *consume_section (char *c)
{
    while (*c && *c != '[') {
        c = consume_line (c);
    }
    return c;
}

bool is_end_of_section (char *c)
{
    return *c == '[' || *c == '\0';
}

bool read_dir (DIR *dirp, struct dirent **res)
{
    errno = 0;
    *res = readdir (dirp);
    if (*res == NULL) {
        if (errno != 0) {
            printf ("Error while reading directory: %s", strerror (errno));
        }
        return false;
    }
    return true;
}

// NOTE: If multiple icons are found, ties are broken according to the order in
// valid_extensions.
bool fname_has_valid_extension (char *fname, size_t *icon_name_len)
{
    bool ret = false;
    size_t len = strlen (fname);
    for (int i=0; i<ARRAY_SIZE(app.valid_extensions); i++) {
        if (g_str_has_suffix(fname, app.valid_extensions[i])) {
            if (icon_name_len != NULL) {
                len -= strlen (app.valid_extensions[i]);
            }
            ret = true;
            break;
        }
    }

    if (icon_name_len != NULL) {
        *icon_name_len = len;
    }
    return ret;
}

bool icon_lookup (mem_pool_t *pool, char *dir, const char *icon_name, char **found_file)
{
    struct stat st;
    if (stat(dir, &st) == -1 && errno == ENOENT) {
        // NOTE: There are index.theme files that have entries for @2
        // directories, even though such directories do not exist in the system.
        //printf ("No directory named: %s\n", dir);
        return false;
    }

    int ext_id = -1;
    DIR *d = opendir (dir);
    struct dirent *entry_info;
    while (read_dir (d, &entry_info)) {
        string_t icon_name_str = str_new(icon_name);
        uint32_t icon_name_len = str_len(&icon_name_str);

        for (int i=0; i<ARRAY_SIZE(app.valid_extensions); i++) {
            str_put_c (&icon_name_str, icon_name_len, app.valid_extensions[i]);
            if (strcmp (str_data(&icon_name_str), entry_info->d_name) == 0) {
                ext_id = i;
            }
        }

        str_free (&icon_name_str);
    }
    closedir (d);

    if (ext_id == -1) {
        // Found no icon named icon_name
        return false;
    } else {
        string_t found_path = str_new (dir);
        if (str_last (&found_path) != '/') {
            str_cat_c (&found_path, "/");
        }
        str_cat_c (&found_path, icon_name);
        str_cat_c (&found_path, app.valid_extensions[ext_id]);

        *found_file = mem_pool_push_size (pool, str_len(&found_path) + 1);
        memcpy (*found_file, str_data(&found_path), str_len(&found_path) + 1);
        str_free (&found_path);
        return true;
    }
}

bool file_lookup (char *dir, char *file)
{
    struct stat st;
    if (stat(dir, &st) == -1 && errno == ENOENT) {
        //printf ("No directory named: %s\n", dir);
        return 0;
    }

    bool res = false;
    DIR *d = opendir (dir);
    struct dirent *entry_info;
    while (read_dir (d, &entry_info)) {
        uint32_t file_len = strlen(file);
        uint32_t entry_len = strlen(entry_info->d_name);
        int cmp = strncmp (file, entry_info->d_name, MIN (file_len, entry_len));
        if (cmp == 0) {
            if (file_len == entry_len) {
                res = true;
                break;
            }
        }
    }
    closedir (d);
    return res;
}

templ_sort_ll(icon_theme_sort, struct icon_theme_t, strcasecmp((*a)->name, (*b)->name) < 0)

void set_theme_name (struct icon_theme_t *theme)
{
    char *c = theme->index_file;

    char *section_name;
    uint32_t section_name_len;
    c = seek_next_section (c, &section_name, &section_name_len);

    char *name_str = "Name";
    while ((c = consume_ignored_lines (c)) && *c && !is_end_of_section(c)) {
        char *key, *value;
        uint32_t key_len, value_len;
        c = seek_next_key_value (c, &key, &key_len, &value, &value_len);
        if (strlen (name_str) == key_len && strncmp (name_str, key, key_len) == 0) {
            theme->name = pom_push_size (&theme->pool, value_len+1);
            memcpy (theme->name, value, value_len);
            theme->name[value_len] = '\0';
        }

    }
}

struct icon_theme_t* app_icon_theme_new (struct app_t *app)
{
    mem_pool_t bootstrap = {0};
    struct icon_theme_t *new_icon_theme = mem_pool_push_size (&bootstrap, sizeof(struct icon_theme_t));
    *new_icon_theme = ZERO_INIT (struct icon_theme_t);
    new_icon_theme->pool = bootstrap;

    new_icon_theme->next = app->themes;
    app->themes = new_icon_theme;
    return new_icon_theme;
}

void icon_theme_destroy (struct icon_theme_t *icon_theme)
{
    if (icon_theme->icon_names != NULL)
        g_hash_table_destroy (icon_theme->icon_names);
    mem_pool_destroy (&icon_theme->pool);
}

// I have to find this information directly from the icon directories and
// index.theme files. The alternative of using GtkIconTheme with a custom theme
// and then calling gtk_icon_theme_list_icons() on it does not only return icons
// from the chosen theme. Instead it also includes:
//
//      * Unthemed icons
//      * Deprecated stock id's (see GTK/testsuite/gtk/check-icon-names.c)
//      * Internal icons (see GTK/testsuite/gtk/check-icon-names.c)
//      * All icons from Hicolor, GNOME and Adwaita themes
//
// I expected Hicolor icons to be there because it's the fallback theme, but I
// didn't expect any of the rest. All this is probably done for backward
// compatibility reasons but it does not work for what we want.
void set_theme_icon_names (struct icon_theme_t *theme)
{
  theme->icon_names = g_hash_table_new (g_str_hash, g_str_equal);

  if (theme->dir_name != NULL) {
      int i;
      for (i=0; i<theme->num_dirs; i++) {
          char *c = theme->index_file;

          // Ignore the first section: [Icon Theme]
          c = seek_next_section (c, NULL, NULL);
          c = consume_section (c);

          string_t theme_dir = str_new (theme->dirs[i]);
          str_cat_c (&theme_dir, "/");
          uint32_t theme_dir_len = str_len (&theme_dir);
          while ((c = consume_section (c)) && *c) {
              char *section_name;
              uint32_t section_name_len;
              c = seek_next_section (c, &section_name, &section_name_len);
              string_t curr_dir = strn_new (section_name, section_name_len);
              str_put (&theme_dir, theme_dir_len, &curr_dir);
              if (section_name[section_name_len-1] != '/') {
                  str_cat_c (&theme_dir, "/");
              }
              uint32_t curr_dir_len = str_len (&theme_dir);

              struct stat st;
              if (stat(str_data(&theme_dir), &st) == -1 && errno == ENOENT) {
                  continue;
              }

              DIR *d = opendir (str_data(&theme_dir));
              struct dirent *entry_info;
              while (read_dir (d, &entry_info)) {
                  if (entry_info->d_name[0] != '.') {
                      str_put_c (&theme_dir, curr_dir_len, entry_info->d_name);
                      size_t icon_name_len;
                      if (stat(str_data(&theme_dir), &st) == 0 &&
                          S_ISREG(st.st_mode) &&
                          fname_has_valid_extension (entry_info->d_name, &icon_name_len)) {
                          char *icon_name = pom_strndup (&theme->pool, entry_info->d_name, icon_name_len);
                          g_hash_table_insert (theme->icon_names, icon_name, NULL);
                      }
                  }
              }
              closedir (d);
          }
      }

  } else {
      // This is the case for non themed icons.
      int i;
      for (i=0; i<theme->num_dirs; i++) {
        string_t path_str = str_new (theme->dirs[i]);
        if (str_last(&path_str) != '/') {
            str_cat_c (&path_str, "/");
        }
        uint32_t path_len = str_len (&path_str);

        DIR *d = opendir (str_data(&path_str));
        struct dirent *entry_info;
        while (read_dir (d, &entry_info)) {
            struct stat st;
            str_put_c (&path_str, path_len, entry_info->d_name);

            if (stat(str_data(&path_str), &st) == 0 && S_ISREG(st.st_mode)) {
                size_t icon_name_len;
                if (fname_has_valid_extension(entry_info->d_name, &icon_name_len)) {
                    char *icon_name = pom_strndup (&theme->pool, entry_info->d_name, icon_name_len);
                    g_hash_table_insert (theme->icon_names, icon_name, NULL);
                }
            }
        }
        str_free (&path_str);
        closedir (d);
      }
  }
}

gint strcase_cmp_callback (gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp ((const char*)a, (const char*)b);
}

// This is case sensitive but will sort correctly strings with different cases
// into alphabetical order AaBbCc not ABCabc.
gint str_cmp_callback (gconstpointer a, gconstpointer b)
{
    int cmp = g_ascii_strcasecmp ((const char*)a, (const char*)b);
    if (cmp == 0) {
        return g_strcmp0 ((const char*)a, (const char*)b);
    } else {
        return cmp;
    }
}

void app_load_all_icon_themes (struct app_t *app)
{
    GtkIconTheme *icon_theme = gtk_icon_theme_get_default ();
    gchar **path;
    gint num_paths;
    gtk_icon_theme_get_search_path (icon_theme, &path, &num_paths);

    // Locate all index.theme files that are in the search paths, and append a
    // new icon_theme_t struct for each one.
    int i;
    for (i=0; i<num_paths; i++) {
        char *curr_search_path = path[i];
        string_t path_str = str_new (curr_search_path);
        if (str_last(&path_str) != '/') {
            str_cat_c (&path_str, "/");
        }
        uint32_t path_len = str_len (&path_str);

        struct stat st;
        if (stat(curr_search_path, &st) != -1 || errno != ENOENT) {
            DIR *d = opendir (curr_search_path);
            struct dirent *entry_info;
            while (read_dir (d, &entry_info)) {
                if (strcmp ("default", entry_info->d_name) != 0 && entry_info->d_name[0] != '.') {
                    string_t theme_dir = str_new (entry_info->d_name);
                    str_put (&path_str, path_len, &theme_dir);

                    if (stat(str_data(&path_str), &st) == 0 && S_ISDIR(st.st_mode) &&
                        file_lookup (str_data(&path_str), "index.theme")) {

                        struct icon_theme_t *theme = app_icon_theme_new (app);
                        theme->dir_name = pom_strdup (&theme->pool, entry_info->d_name);

                        str_cat_c (&path_str, "/index.theme");
                        theme->index_file = full_file_read (&theme->pool, str_data(&path_str));
                        set_theme_name(theme);
                    }
                    str_free (&theme_dir);
                }
            }

            closedir (d);

        } else {
            // curr_search_path does not exist.
        }

        str_free (&path_str);
    }

    // A theme can be spread across multiple search paths. Now that we know the
    // internal name for each theme, we look for subdirectories with this
    // internal name to know which directories a theme is spread across.
    for (struct icon_theme_t *curr_theme = app->themes; curr_theme; curr_theme = curr_theme->next) {
        char *found_dirs[num_paths];
        uint32_t num_found = 0;
        int j;
        for (j=0; j<num_paths; j++) {
            char *curr_search_path = path[j];
            string_t path_str = str_new (curr_search_path);
            if (str_last(&path_str) != '/') {
                str_cat_c (&path_str, "/");
            }
            str_cat_c (&path_str, curr_theme->dir_name);

            struct stat st;
            if (stat(str_data(&path_str), &st) == 0 && S_ISDIR(st.st_mode)) {
                found_dirs[num_found] = pom_strdup (&curr_theme->pool, str_data(&path_str));
                num_found++;
            }
            str_free (&path_str);
        }

        curr_theme->dirs = (char**)pom_push_size (&curr_theme->pool, sizeof(char*)*num_found);
        memcpy (curr_theme->dirs, found_dirs, sizeof(char*)*num_found);
        curr_theme->num_dirs = num_found;
    }

    icon_theme_sort (&app->themes, -1);

    // Unthemed icons are found inside search path directories but not in a
    // directory. For these icons we add a zero initialized theme, and set as
    // dirs all search paths with icons in them.
    //
    // NOTE: Search paths are not explored recursiveley for icons.
    struct icon_theme_t *no_theme = app_icon_theme_new (app);
    no_theme->name = "None";

    char *found_dirs[num_paths];
    uint32_t num_found = 0;
    for (i=0; i<num_paths; i++) {
        string_t path_str = str_new (path[i]);
        if (str_last(&path_str) != '/') {
            str_cat_c (&path_str, "/");
        }
        uint32_t path_len = str_len (&path_str);

        struct stat st;
        if (stat(str_data(&path_str), &st) == -1 && errno == ENOENT) {
            // NOTE: Current search paths may contain non existent directories.
            //printf ("Search path does not exist.\n");
            continue;
        }

        DIR *d = opendir (str_data(&path_str));
        struct dirent *entry_info;
        while (read_dir (d, &entry_info)) {
            str_put_c (&path_str, path_len, entry_info->d_name);

            if (stat(str_data(&path_str), &st) == 0 && S_ISREG(st.st_mode)) {
                if (fname_has_valid_extension (entry_info->d_name, NULL)) {
                    uint32_t res_len = strlen (path[i]) + 1;
                    found_dirs[num_found] = (char*)pom_push_size (&no_theme->pool, res_len);
                    memcpy (found_dirs[num_found], path[i], res_len);
                    num_found++;
                    break;
                }
            }
        }
        str_free (&path_str);
        closedir (d);
    }

    no_theme->dirs = (char**)pom_push_size (&no_theme->pool, sizeof(char*)*num_found);
    memcpy (no_theme->dirs, found_dirs, sizeof(char*)*num_found);
    no_theme->num_dirs = num_found;

    // Find all icon names for each found theme and store them in the icon_names
    // hash table.
    for (struct icon_theme_t *curr_theme = app->themes; curr_theme; curr_theme = curr_theme->next) {
        set_theme_icon_names (curr_theme);
    }

    // Add all icon themes into a structure so we can fake an "All" theme.
    app->all_icon_names_pool = ZERO_INIT (mem_pool_t);
    app->all_icon_names = g_tree_new (str_cmp_callback);

    struct icon_theme_t *curr_theme = app->themes;
    for (; curr_theme; curr_theme = curr_theme->next) {
        GList *icon_names = g_hash_table_get_keys (curr_theme->icon_names);
        for (GList *l = icon_names; l != NULL; l = l->next) {
            if (!g_tree_lookup_extended (app->all_icon_names, l->data, NULL, NULL)) {
                char *icon_name = pom_strdup (&app->all_icon_names_pool, l->data);
                g_tree_insert (app->all_icon_names, icon_name, NULL);
            }
        }
        g_list_free (icon_names);
    }
}

void app_destroy (struct app_t *app)
{
    struct icon_theme_t *curr_theme = app->themes;
    while (curr_theme != NULL) {
        struct icon_theme_t *to_destroy = curr_theme;
        curr_theme = curr_theme->next;

        icon_theme_destroy (to_destroy);
    }

    mem_pool_destroy(&app->icon_view_pool);
    free (app->selected_icon);

    mem_pool_destroy(&app->all_icon_names_pool);
    g_tree_destroy (app->all_icon_names);
}

// This makes scalable images always sort as the largest.
bool is_img_lt (struct icon_image_t *a, struct icon_image_t *b)
{
    if (a->is_scalable == b->is_scalable) {
        return a->size < b->size;
    } else {
        return b->is_scalable;
    }
}

templ_sort_ll(icon_image_sort, struct icon_image_t, is_img_lt(*a, *b))

void icon_view_compute (mem_pool_t *pool,
                        struct icon_theme_t *theme, const char *icon_name,
                        struct icon_view_t *icon_view)
{
    assert (strcmp (theme->name, "All") != 0);

    *icon_view = ZERO_INIT (struct icon_view_t);
    icon_view->scale = 1;
    icon_view->icon_name = pom_strndup (pool, icon_name, strlen(icon_name));
    struct icon_image_t **last_image[] = {&icon_view->images[0], &icon_view->images[1], &icon_view->images[2]};
    int num_images[] = {0, 0, 0};

    if (theme->index_file != NULL) {
        bool found_image = false;
        int i;
        for (i = 0; i < theme->num_dirs; i++) {
            string_t path = str_new (theme->dirs[i]);
            if (str_last (&path) != '/') {
                str_cat_c (&path, "/");
            }
            uint32_t path_len = str_len (&path);
            char *c = theme->index_file;

            // Ignore the first section: [Icon Theme]
            c = seek_next_section (c, NULL, NULL);
            c = consume_section (c);

            while (*c) {
                // FIXME: We currently ignore the Directories key in the first
                // section [Icon Theme], some themes (Oxygen) have repeated
                // directory sections while they are unique in the Directories
                // key. Icons in these folders will show several times. Maybe
                // read the Directories key or do nothing so theme developers
                // can notice something strange is going on.
                char *section_name;
                uint32_t section_name_len;
                c = seek_next_section (c, &section_name, &section_name_len);
                string_t dir = strn_new (section_name, section_name_len);
                str_put (&path, path_len, &dir);

                char *icon_path;
                if (icon_lookup (pool, str_data (&path), icon_name, &icon_path)) {
                    // TODO: Maybe get this information before looking up the directory
                    // and conditionally call icon_lookup() depending on the information
                    // we get.

                    struct icon_image_t img = ZERO_INIT(struct icon_image_t);
                    mem_pool_temp_marker_t mrkr = mem_pool_begin_temporary_memory (pool);
                    img.scale = 1;
                    img.min_size = -1;
                    img.max_size = -1;
                    img.size = -1;
                    int icon_path_len = strlen(icon_path);
                    img.full_path = pom_strndup (pool, icon_path, icon_path_len);
                    img.theme_dir = pom_strndup (pool, icon_path, path_len);
                    img.path = pom_strndup (pool, icon_path + path_len, icon_path_len - path_len);

                    // NOTE: We say an image is scalable if dir contains the
                    // substring "scalable" as this is what developers seem to
                    // use. The index file may disagree, and Gtk for example
                    // makes any .svg icon 'scalable' no matter what the index
                    // file or dir says.
                    img.is_scalable = strstr (str_data(&dir), "scalable") != NULL ? true : false;

                    while ((c = consume_ignored_lines (c)) && !is_end_of_section(c)) {
                        char *key, *value;
                        uint32_t key_len, value_len;
                        c = seek_next_key_value (c, &key, &key_len, &value, &value_len);
                        if (strncmp (key, "Size", MIN(4, key_len)) == 0) {
                            sscanf (value, "%"SCNi32, &img.size);

                        } else if (strncmp (key, "MinSize", MIN(7, key_len)) == 0) {
                            sscanf (value, "%"SCNi32, &img.min_size);

                        } else if (strncmp (key, "MaxSize", MIN(7, key_len)) == 0) {
                            sscanf (value, "%"SCNi32, &img.max_size);

                        } else if (strncmp (key, "Scale", MIN(5, key_len)) == 0) {
                            sscanf (value, "%"SCNi32, &img.scale);

                        } else if (strncmp (key, "Type", MIN(4, key_len)) == 0) {
                            img.type = pom_strndup (pool, value, value_len);

                        } else if (strncmp (key, "Context", MIN(7, key_len)) == 0) {
                            img.context = pom_strndup (pool, value, value_len);
                        }
                    }

                    // Create the icon_image_t structure inside pool.
                    struct icon_image_t *new_img =
                        mem_pool_push_size (pool, sizeof(struct icon_image_t));
                    *new_img = img;

                    // Add the new image at the end of the corresponding linked list
                    if (img.scale <= 3) {
                        found_image = true;
                        *last_image[new_img->scale-1] = new_img;
                        last_image[new_img->scale-1] = &new_img->next;
                        num_images[img.scale-1]++;
                    }  else {
                        // TODO: Will we ever use x4 icons?
                        mem_pool_end_temporary_memory (mrkr);
                    }


                } else {
                    c = consume_section (c);
                }

                str_free (&dir);
            }

            // Sort each icon_image_t linked list
            for (int i=0; i<ARRAY_SIZE(icon_view->images); i++) {
                if (num_images[i] > 1) {
                    // TODO: Sorted linked lists seem to be the common case we
                    // can detect if sorting is required before and only sort if
                    // necessary.
                    // @performance
                    icon_image_sort (&icon_view->images[i], num_images[i]);
                }
            }

            str_free (&path);

            // If we found something in a search path then stop looking in the
            // other ones.
            if (found_image) break;
        }

        assert (found_image && "Icon not found in that theme");

    } else {
        int i;
        for (i = 0; i < theme->num_dirs; i++) {
            string_t path = str_new (theme->dirs[i]);
            if (str_last (&path) != '/') {
                str_cat_c (&path, "/");
            }

            char *icon_path;
            if (icon_lookup (pool, str_data (&path), icon_name, &icon_path)) {
                struct icon_image_t *new_img =
                    mem_pool_push_size (pool, sizeof(struct icon_image_t));
                *new_img = ZERO_INIT(struct icon_image_t);
                new_img->path = pom_strndup(pool, icon_path, strlen(icon_path));
                new_img->full_path = new_img->path;
                new_img->scale = 1;

                // Add the new image at the end of the image linked list
                *last_image[0] = new_img;
                last_image[0] = &new_img->next;
            }

            str_free (&path);
        }
    }

    // Compute the remaining fields based on the ones found above
    for (int i=0; i<ARRAY_SIZE(icon_view->images); i++) {
        struct icon_image_t *img = icon_view->images[i];

        while (img != NULL) {
            // Compute label for the image
            // NOTE: If it's the theme that contains unthemed icons. Leave the
            // label as NULL.
            img->label = NULL;
            if (theme->dir_name != NULL) {
                img->label = mem_pool_push_size (pool, sizeof(char)*16);
                if (img->is_scalable) {
                    sprintf (img->label, "Scalable");
                } else {
                    sprintf (img->label, "%d", img->size);
                }
            }

            // Set back pointer into icon_view_t
            img->view = icon_view;

            // Create a GtkImage for the found image
            img->image = gtk_image_new_from_file (img->full_path);
            struct stat st;
            stat(img->full_path, &st);
            img->file_size = st.st_size;
            gtk_widget_set_valign (img->image, GTK_ALIGN_END);

            // Find the size of the created image
            GdkPixbuf *pixbuf = gtk_image_get_pixbuf (GTK_IMAGE(img->image));
            if (pixbuf) {
                img->width = gdk_pixbuf_get_width(pixbuf);
                img->height = gdk_pixbuf_get_height(pixbuf);
            }
            gtk_widget_set_size_request (img->image, img->width, img->height);

            g_assert (img->image != NULL);
            // The container to which images will be parented will get destroyed
            // when changing icon scales, we need to take a reference here so we
            // can go back to them. The lifespan of these images should be equal
            // to icon_view_t, not to their parent container.
            // @scale_change_destroys_images
            g_object_ref_sink (G_OBJECT(img->image));

            img = img->next;
        }
    }
}

void app_update_selected_icon (struct app_t *app, const char *selected_icon)
{
    if (app->selected_icon) {
        if (strcmp (app->selected_icon, selected_icon) != 0) {
            free (app->selected_icon);
            app->selected_icon = strdup (selected_icon);
        }
    } else {
        app->selected_icon = strdup (selected_icon);
    }
}

void app_set_icon_view (struct app_t *app, const char *icon_name)
{
    // Unref all GtkImages before creating the new icon_view. I don't like this,
    // istead of storing a GtkImage we should store our own data structure that
    // has things inside icon_view_pool.
    // @scale_change_destroys_images
    for (int i=0; i<ARRAY_SIZE(app->icon_view.images); i++) {
        struct icon_image_t *img = app->icon_view.images[i];

        while (img != NULL) {
            if (img->image != NULL) {
                g_object_unref (G_OBJECT(img->image));
            }
            img = img->next;
        }
    }

    // Update data in the icon_view_t structure
    mem_pool_destroy (&app->icon_view_pool);
    app->icon_view_pool = ZERO_INIT(mem_pool_t);
    app_update_selected_icon (app, icon_name);
    icon_view_compute (&app->icon_view_pool, app->selected_theme, icon_name, &app->icon_view);

    replace_wrapped_widget_defered (&app->icon_view_widget, draw_icon_view (&app->icon_view));
}

void on_icon_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    if (row == NULL) {
        return;
    }

    GtkWidget *row_label = gtk_bin_get_child (GTK_BIN(row));
    const char *icon_name = gtk_label_get_text (GTK_LABEL(row_label));

    app_set_icon_view (&app, icon_name);
}

FAKE_LIST_BOX_ROW_SELECTED_CB (on_all_theme_row_selected)
{
    const char *icon_name = fake_list_box->rows[idx];

    if (app.all_theme_selected) {
        struct icon_theme_t *theme;
        for (theme = app.themes; theme; theme = theme->next) {
            if (g_hash_table_contains (theme->icon_names, icon_name)) break;
        }
        assert (theme != NULL);
        app.selected_theme = theme;
    }

    app_set_icon_view (&app, icon_name);
}

gboolean on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer data) {
    if (event->keyval == GDK_KEY_Escape){
        gtk_entry_set_text (GTK_ENTRY(app.search_entry), "");
        return TRUE;
    }
    return FALSE;
}

gboolean search_filter (GtkListBoxRow *row, gpointer user_data)
{
    const gchar *search_str = gtk_entry_get_text (GTK_ENTRY(app.search_entry));
    GtkWidget *row_label = gtk_bin_get_child (GTK_BIN(row));
    const char * icon_name = gtk_label_get_text (GTK_LABEL(row_label));

    if (strstr (icon_name, search_str) != NULL) {
        return TRUE;
    } else {
        return FALSE;
    }
}

// The only way to iterate through a GTree is using a callback an
// g_tree_foreach, this is the callback that builds the "All" theme icon name
// list.
struct all_theme_list_build_clsr_t {
    GtkWidget *new_icon_list;
    bool first;
    const char *selected_icon;
};

gboolean all_theme_list_build (gpointer key, gpointer value, gpointer data)
{
    struct all_theme_list_build_clsr_t *clsr = (struct all_theme_list_build_clsr_t*)data;

    GtkWidget *row = gtk_label_new (key);
    gtk_container_add (GTK_CONTAINER(clsr->new_icon_list), row);
    gtk_widget_set_halign (row, GTK_ALIGN_START);

    if (clsr->selected_icon == NULL && clsr->first) {
        clsr->first = false;
        clsr->selected_icon = key;
    }

    if (strcmp (clsr->selected_icon, key) == 0) {
        GtkWidget *r = gtk_widget_get_parent (row);
        gtk_list_box_select_row (GTK_LIST_BOX(clsr->new_icon_list), GTK_LIST_BOX_ROW(r));
    }

    gtk_widget_set_margin_start (row, 6);
    gtk_widget_set_margin_end (row, 6);
    gtk_widget_set_margin_top (row, 3);
    gtk_widget_set_margin_bottom (row, 3);

    return FALSE;
}

GtkWidget *all_icon_names_list_new (const char *selected_icon, const char **choosen_icon)
{
    assert (choosen_icon != NULL);

    GtkWidget *new_icon_list = gtk_list_box_new ();
    gtk_widget_set_vexpand (new_icon_list, TRUE);
    gtk_widget_set_hexpand (new_icon_list, TRUE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX(new_icon_list), search_filter, NULL, NULL);

    struct all_theme_list_build_clsr_t clsr;
    clsr.new_icon_list = new_icon_list;
    clsr.first = true;
    clsr.selected_icon = selected_icon;

    g_tree_foreach (app.all_icon_names, all_theme_list_build, &clsr);

    *choosen_icon = clsr.selected_icon;
    g_signal_connect (G_OBJECT(new_icon_list), "row-selected", G_CALLBACK (on_icon_selected), NULL);

    return new_icon_list;
}

GtkWidget *icon_list_new (const char *theme_name, const char *selected_icon, const char **choosen_icon)
{
    assert (choosen_icon != NULL);

    struct icon_theme_t *theme;
    for (theme = app.themes; theme; theme = theme->next) {
        if (strcmp (theme_name, theme->name) == 0) break;
    }
    assert (theme != NULL && "Theme name not found");

    GtkWidget *new_icon_list = gtk_list_box_new ();
    gtk_widget_set_vexpand (new_icon_list, TRUE);
    gtk_widget_set_hexpand (new_icon_list, TRUE);
    gtk_list_box_set_filter_func (GTK_LIST_BOX(new_icon_list), search_filter, NULL, NULL);

    GList *icon_names = g_hash_table_get_keys (theme->icon_names);
    icon_names = g_list_sort (icon_names, strcase_cmp_callback);

    bool first = true;
    uint32_t i = 0;
    GList *l = NULL;
    for (l = icon_names; l != NULL; l = l->next)
    {
        GtkWidget *row = gtk_label_new (l->data);
        gtk_container_add (GTK_CONTAINER(new_icon_list), row);
        gtk_widget_set_halign (row, GTK_ALIGN_START);

        if (selected_icon == NULL && first) {
            first = false;
            selected_icon = l->data;
        }

        if (strcmp (selected_icon, l->data) == 0) {
            GtkWidget *r = gtk_widget_get_parent (row);
            gtk_list_box_select_row (GTK_LIST_BOX(new_icon_list), GTK_LIST_BOX_ROW(r));
        }

        gtk_widget_set_margin_start (row, 6);
        gtk_widget_set_margin_end (row, 6);
        gtk_widget_set_margin_top (row, 3);
        gtk_widget_set_margin_bottom (row, 3);
        i++;
    }
    g_list_free (icon_names);

    *choosen_icon = selected_icon;
    g_signal_connect (G_OBJECT(new_icon_list), "row-selected", G_CALLBACK (on_icon_selected), NULL);
    return new_icon_list;
}

void on_theme_changed (GtkComboBox *themes_combobox, gpointer user_data);
GtkWidget *theme_selector_new (const char *theme_name)
{
    GtkWidget *themes_combobox;
    GtkWidget *theme_selector = labeled_combobox_new ("Theme:", &themes_combobox);
    combo_box_text_append_text_with_id (GTK_COMBO_BOX_TEXT(themes_combobox), "All");
    for (struct icon_theme_t *curr_theme = app.themes; curr_theme; curr_theme = curr_theme->next) {
        combo_box_text_append_text_with_id (GTK_COMBO_BOX_TEXT(themes_combobox), curr_theme->name);
    }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX(themes_combobox), theme_name);
    g_signal_connect (G_OBJECT(themes_combobox), "changed", G_CALLBACK (on_theme_changed), NULL);
    return theme_selector;
}

void on_theme_changed (GtkComboBox *themes_combobox, gpointer user_data)
{
    const char *icon_name = NULL;
    const char* theme_name = gtk_combo_box_get_active_id (themes_combobox);
    if (strcmp (theme_name, "All") == 0) {
        app.all_theme_selected = true;

        replace_wrapped_widget (&app.icon_list, app.all_icon_names_widget);

        struct icon_theme_t *theme;
        for (theme = app.themes; theme; theme = theme->next) {
            if (g_hash_table_contains (theme->icon_names, app.all_icon_names_first)) break;
        }
        icon_name = app.all_icon_names_first;
        assert (theme != NULL);
        theme_name = theme->name;

    } else {
        app.all_theme_selected = false;
    }

    app_set_selected_theme (&app, theme_name, icon_name);
}

void app_set_selected_theme (struct app_t *app, const char *theme_name, const char *selected_icon)
{
    assert (strcmp (theme_name, "All") != 0);

    struct icon_theme_t *curr_theme;
    for (curr_theme = app->themes; curr_theme; curr_theme = curr_theme->next) {
        if (strcmp (theme_name, curr_theme->name) == 0) break;
    }
    assert (curr_theme != NULL && "Theme name not found");
    app->selected_theme = curr_theme;

    const char *choosen_icon = selected_icon;
    if (!app->all_theme_selected) {
        GtkWidget *new_icon_list = icon_list_new (theme_name, selected_icon, &choosen_icon);
        replace_wrapped_widget (&app->icon_list, new_icon_list);

        GtkWidget *new_theme_selector = theme_selector_new (theme_name);
        replace_wrapped_widget_defered (&app->theme_selector, new_theme_selector);
    }

    app_update_selected_icon (app, choosen_icon);
    app_set_icon_view (app, app->selected_icon);
}

void on_search_changed (GtkEditable *search_entry, gpointer user_data)
{
    gtk_list_box_invalidate_filter (GTK_LIST_BOX(app.icon_list));
}

gboolean delete_callback (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    gtk_main_quit ();
    return FALSE;
}

int main(int argc, char *argv[])
{
    GtkWidget *window;
    app = (struct app_t){
#define EXTENSION(name,str) str,
        .valid_extensions = { VALID_EXTENSIONS }
#undef EXTENSION
    };

    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_resize (GTK_WINDOW(window), 970, 650);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    GtkWidget *header_bar = gtk_header_bar_new ();
    gtk_header_bar_set_title (GTK_HEADER_BAR(header_bar), "Iconoscope");
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR(header_bar), TRUE);
    gtk_window_set_titlebar (GTK_WINDOW(window), header_bar);

    g_signal_connect (G_OBJECT(window), "delete-event", G_CALLBACK (delete_callback), NULL);
    g_signal_connect (G_OBJECT(window), "key-press-event", G_CALLBACK (on_key_press), NULL);

    app_load_all_icon_themes (&app);

    app.search_entry = gtk_search_entry_new ();
    g_signal_connect (G_OBJECT(app.search_entry), "changed", G_CALLBACK (on_search_changed), NULL);

    app.icon_list = gtk_grid_new (); // Placeholder
    GtkWidget *scrolled_icon_list = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_disable_hscroll (GTK_SCROLLED_WINDOW(scrolled_icon_list));
    gtk_container_add (GTK_CONTAINER (scrolled_icon_list), app.icon_list);

    app.theme_selector = gtk_grid_new (); // Placeholder

    GtkWidget *sidebar = gtk_grid_new ();
    gtk_grid_attach (GTK_GRID(sidebar), app.search_entry, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID(sidebar), scrolled_icon_list, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID(sidebar), wrap_gtk_widget(app.theme_selector), 0, 2, 1, 1);

    app.icon_view_widget = gtk_grid_new (); // Placeholder
    GtkWidget *paned = fix_gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1 (GTK_PANED(paned), sidebar, FALSE, FALSE);
    gtk_paned_pack2 (GTK_PANED(paned), wrap_gtk_widget(app.icon_view_widget), TRUE, TRUE);

    app.all_icon_names_widget = fake_list_box_init (&app.fake_list_box,
                                                    app.all_icon_names,
                                                    on_all_theme_row_selected);
    app.all_icon_names_first = app.fake_list_box.rows[0];
    g_object_ref_sink (app.all_icon_names_widget);

    app_set_selected_theme (&app, "Hicolor", NULL);

    gtk_container_add(GTK_CONTAINER(window), paned);

    gtk_widget_show_all(window);

    gtk_main();

    app_destroy (&app);
    return 0;
}
