/* os_win32.c */
int dyn_libintl_init __ARGS((char *libname));
void dyn_libintl_end __ARGS((void));
char *null_libintl_gettext __ARGS((const char *msgid));
char *null_libintl_bindtextdomain __ARGS((const char *domainname, const char *dirname));
char *null_libintl_textdomain __ARGS((const char *domainname));
void PlatformId __ARGS((void));
int mch_windows95 __ARGS((void));
void mch_setmouse __ARGS((int on));
void mch_update_cursor __ARGS((void));
int mch_char_avail __ARGS((void));
int mch_inchar __ARGS((char_u *buf, int maxlen, long time));
void mch_shellinit __ARGS((void));
void mch_windexit __ARGS((int r));
int mch_check_win __ARGS((int argc, char **argv));
void fname_case __ARGS((char_u *name));
int mch_get_user_name __ARGS((char_u *s, int len));
void mch_get_host_name __ARGS((char_u *s, int len));
long mch_get_pid __ARGS((void));
int mch_dirname __ARGS((char_u *buf, int len));
long mch_getperm __ARGS((char_u *name));
int mch_setperm __ARGS((char_u *name, long perm));
void mch_hide __ARGS((char_u *name));
int mch_isdir __ARGS((char_u *name));
int mch_writable __ARGS((char_u *name));
int mch_can_exe __ARGS((char_u *name));
int mch_nodetype __ARGS((char_u *name));
vim_acl_T mch_get_acl __ARGS((char_u *fname));
void mch_set_acl __ARGS((char_u *fname, vim_acl_T acl));
void mch_free_acl __ARGS((vim_acl_T acl));
void mch_settmode __ARGS((int tmode));
int mch_get_shellsize __ARGS((void));
void mch_set_shellsize __ARGS((void));
void mch_new_shellsize __ARGS((void));
void mch_set_winsize_now __ARGS((void));
int mch_call_shell __ARGS((char_u *cmd, int options));
int mch_expandpath __ARGS((garray_T *gap, char_u *path, int flags));
void mch_set_normal_colors __ARGS((void));
void mch_write __ARGS((char_u *s, int len));
void mch_delay __ARGS((long msec, int ignoreinput));
int mch_remove __ARGS((char_u *name));
void mch_breakcheck __ARGS((void));
long_u mch_avail_mem __ARGS((int special));
int mch_rename __ARGS((const char *pszOldFile, const char *pszNewFile));
char *default_shell __ARGS((void));
void mch_print_cleanup __ARGS((void));
int mch_print_init __ARGS((prt_settings_T *psettings, char_u *jobname, int forceit));
int mch_print_begin __ARGS((prt_settings_T *psettings));
void mch_print_end __ARGS((void));
int mch_print_end_page __ARGS((void));
int mch_print_begin_page __ARGS((void));
int mch_print_text_out __ARGS((int x, int y, char_u *p, int len, int *must_break));
void mch_print_setfont __ARGS((int iBold, int iItalic, int iUnderline));
void mch_print_set_bg __ARGS((long bgcol));
void mch_print_set_fg __ARGS((long fgcol));
/* vim: set ft=c : */
