/*
   src/filemanager - tests for Enter key path handling in panels

   This file is part of the Midnight Commander.

   The goal is to ensure that directory activation doesn't accidentally
   resolve absolute-looking entries relative to the current panel directory.
 */

#define TEST_SUITE_NAME "/src/filemanager"

#include "tests/mctest.h"

#include "src/vfs/local/local.c"

#include "src/filemanager/panel.h"

/*
 * We include panel.c to access its static do_enter_on_file_entry(), but we
 * must avoid clashing with real (non-static) symbols also defined there.
 */
#define panel_cd panel_cd__real

#include "src/filemanager/panel.c"

#undef panel_cd

/* --------------------------------------------------------------------------------------------- */

/* @ThenReturnValue */
static gboolean panel_cd__return_value;
/* @CapturedValue */
static vfs_path_t *panel_cd__new_dir_vpath__captured;

gboolean
/* @Mock */
gboolean
panel_cd (WPanel *panel, const vfs_path_t *new_dir_vpath, enum cd_enum exact)
{
    (void) panel;
    (void) exact;

    vfs_path_free (panel_cd__new_dir_vpath__captured, TRUE);
    panel_cd__new_dir_vpath__captured = vfs_path_clone (new_dir_vpath);
    return panel_cd__return_value;
}

/* --------------------------------------------------------------------------------------------- */

/* @Mock */
void
cd_error_message (const char *path)
{
    (void) path;
}

/* --------------------------------------------------------------------------------------------- */

/* regex_command is referenced for non-directories only. Provide a stub anyway. */
/* @Mock */
int
regex_command (const vfs_path_t *filename_vpath, const char *action)
{
    (void) filename_vpath;
    (void) action;
    return 0;
}

/* @Mock */
void
file_error_message (const char *format, const char *text)
{
    (void) format;
    (void) text;
}

/* --------------------------------------------------------------------------------------------- */

/* @Before */
static void
setup (void)
{
    str_init_strings (NULL);

    vfs_init ();
    vfs_init_localfs ();
    vfs_setup_work_dir ();

    panel_cd__return_value = TRUE;
    panel_cd__new_dir_vpath__captured = NULL;
}

/* --------------------------------------------------------------------------------------------- */

/* @After */
static void
teardown (void)
{
    vfs_path_free (panel_cd__new_dir_vpath__captured, TRUE);
    panel_cd__new_dir_vpath__captured = NULL;

    vfs_shut ();
    str_uninit_strings ();
}

/* --------------------------------------------------------------------------------------------- */

/* @Test */
START_TEST (test_enter_dir_absolute_name_does_not_get_prefixed)
{
    WPanel panel;
    file_entry_t fe;

    memset (&panel, 0, sizeof (panel));
    memset (&fe, 0, sizeof (fe));

    panel.cwd_vpath = vfs_path_from_str ("/tmp");

    fe.fname = g_string_new ("/data");
    fe.st.st_mode = S_IFDIR;

    (void) do_enter_on_file_entry (&panel, &fe);

    ck_assert_ptr_nonnull (panel_cd__new_dir_vpath__captured);
    mctest_assert_str_eq ("/data", vfs_path_as_str (panel_cd__new_dir_vpath__captured));

    g_string_free (fe.fname, TRUE);
    vfs_path_free (panel.cwd_vpath, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

/* @Test */
START_TEST (test_enter_dir_relative_name_gets_prefixed_with_cwd)
{
    WPanel panel;
    file_entry_t fe;

    memset (&panel, 0, sizeof (panel));
    memset (&fe, 0, sizeof (fe));

    panel.cwd_vpath = vfs_path_from_str ("/tmp");

    fe.fname = g_string_new ("data");
    fe.st.st_mode = S_IFDIR;

    (void) do_enter_on_file_entry (&panel, &fe);

    ck_assert_ptr_nonnull (panel_cd__new_dir_vpath__captured);
    mctest_assert_str_eq ("/tmp/data", vfs_path_as_str (panel_cd__new_dir_vpath__captured));

    g_string_free (fe.fname, TRUE);
    vfs_path_free (panel.cwd_vpath, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

int
main (void)
{
    TCase *tc_core;

    tc_core = tcase_create ("Core");

    tcase_add_checked_fixture (tc_core, setup, teardown);

    tcase_add_test (tc_core, test_enter_dir_absolute_name_does_not_get_prefixed);
    tcase_add_test (tc_core, test_enter_dir_relative_name_gets_prefixed_with_cwd);

    return mctest_run_all (tc_core);
}
